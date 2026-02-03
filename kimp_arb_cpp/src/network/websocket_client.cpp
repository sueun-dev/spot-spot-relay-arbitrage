#include "kimp/network/websocket_client.hpp"

#include <regex>
#include <sstream>

namespace kimp::network {

bool WebSocketClient::parse_url(const std::string& url) {
    // Parse wss://host:port/path or wss://host/path
    std::regex url_regex(R"(wss?://([^:/]+)(?::(\d+))?(/.*)?)", std::regex::icase);
    std::smatch match;

    if (!std::regex_match(url, match, url_regex)) {
        Logger::error("[{}] Invalid URL format: {}", name_, url);
        return false;
    }

    host_ = match[1].str();
    port_ = match[2].matched ? match[2].str() : "443";
    path_ = match[3].matched ? match[3].str() : "/";

    Logger::debug("[{}] Parsed URL - host: {}, port: {}, path: {}",
                  name_, host_, port_, path_);
    return true;
}

void WebSocketClient::connect(const std::string& url) {
    if (state_.load() == ConnectionState::Connected ||
        state_.load() == ConnectionState::Connecting) {
        Logger::warn("[{}] Already connected or connecting", name_);
        return;
    }

    if (!parse_url(url)) {
        if (on_connect_) {
            on_connect_(false, "Invalid URL");
        }
        return;
    }

    state_ = ConnectionState::Connecting;
    reconnect_attempts_ = 0;
    should_reconnect_ = true;

    Logger::info("[{}] Connecting to {}", name_, url);
    do_resolve();
}

void WebSocketClient::disconnect() {
    should_reconnect_ = false;
    reconnect_timer_.cancel();
    ping_timer_.cancel();

    if (state_.load() == ConnectionState::Disconnected) {
        return;
    }

    if (ws_ && ws_->is_open()) {
        state_ = ConnectionState::Disconnected;
        do_close();
    } else {
        state_ = ConnectionState::Disconnected;
    }
}

void WebSocketClient::reconnect() {
    if (!should_reconnect_) {
        return;
    }

    state_ = ConnectionState::Reconnecting;
    schedule_reconnect();
}

void WebSocketClient::send(std::string message) {
    if (state_.load() != ConnectionState::Connected) {
        Logger::warn("[{}] Cannot send, not connected", name_);
        return;
    }

    // Lock-free push to write queue
    if (!write_queue_.try_push(std::move(message))) {
        Logger::warn("[{}] Write queue full, dropping message", name_);
        return;
    }

    if (!is_writing_.exchange(true)) {
        net::post(ws_->get_executor(), [self = shared_from_this()] {
            self->do_write();
        });
    }
}

void WebSocketClient::send(std::string_view message) {
    send(std::string(message));
}

void WebSocketClient::do_resolve() {
    resolver_.async_resolve(
        host_, port_,
        beast::bind_front_handler(&WebSocketClient::on_resolve, shared_from_this()));
}

void WebSocketClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        handle_error("resolve", ec);
        return;
    }

    // Create new WebSocket stream
    ws_ = std::make_unique<WebSocketStream>(
        net::make_strand(io_context_), ssl_context_);

    // Set TCP options
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    // Connect
    beast::get_lowest_layer(*ws_).async_connect(
        results,
        beast::bind_front_handler(&WebSocketClient::on_connect, shared_from_this()));
}

void WebSocketClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) {
        handle_error("connect", ec);
        return;
    }

    Logger::debug("[{}] TCP connected to {}:{}", name_, host_, ep.port());

    // Set SNI hostname for SSL
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                               net::error::get_ssl_category());
        handle_error("ssl_sni", ec);
        return;
    }

    // SSL handshake
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WebSocketClient::on_ssl_handshake, shared_from_this()));
}

void WebSocketClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        handle_error("ssl_handshake", ec);
        return;
    }

    Logger::debug("[{}] SSL handshake complete", name_);

    // Turn off timeout on TCP stream (WebSocket has its own)
    beast::get_lowest_layer(*ws_).expires_never();

    // Set WebSocket options
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator([this](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, "KIMP-Bot/1.0");
        req.set(beast::http::field::host, host_);
    }));

    // WebSocket handshake
    ws_->async_handshake(host_, path_,
        beast::bind_front_handler(&WebSocketClient::on_handshake, shared_from_this()));
}

void WebSocketClient::on_handshake(beast::error_code ec) {
    if (ec) {
        handle_error("handshake", ec);
        return;
    }

    state_ = ConnectionState::Connected;
    reconnect_attempts_ = 0;

    Logger::info("[{}] WebSocket connected to {}:{}{}", name_, host_, port_, path_);

    if (on_connect_) {
        on_connect_(true, "");
    }

    // Start reading
    do_read();

    // Start ping timer
    schedule_ping();
}

void WebSocketClient::do_read() {
    read_buffer_.clear();
    ws_->async_read(
        read_buffer_,
        beast::bind_front_handler(&WebSocketClient::on_read, shared_from_this()));
}

void WebSocketClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        if (ec == websocket::error::closed) {
            Logger::info("[{}] WebSocket closed by server", name_);
            if (on_disconnect_) {
                on_disconnect_("Server closed connection");
            }
        } else {
            handle_error("read", ec);
        }
        reconnect();
        return;
    }

    // Process message
    if (on_message_) {
        auto data = beast::buffers_to_string(read_buffer_.data());
        auto type = ws_->got_text() ? MessageType::Text : MessageType::Binary;
        on_message_(data, type);
    }

    // Continue reading
    do_read();
}

void WebSocketClient::do_write() {
    // Lock-free pop from write queue
    auto message = write_queue_.try_pop();
    if (!message) {
        is_writing_ = false;
        return;
    }

    ws_->text(true);
    ws_->async_write(
        net::buffer(*message),
        beast::bind_front_handler(&WebSocketClient::on_write, shared_from_this()));
}

void WebSocketClient::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        handle_error("write", ec);
        is_writing_ = false;
        reconnect();
        return;
    }

    // Continue writing if there are more messages
    do_write();
}

void WebSocketClient::do_close() {
    ws_->async_close(
        websocket::close_code::normal,
        beast::bind_front_handler(&WebSocketClient::on_close, shared_from_this()));
}

void WebSocketClient::on_close(beast::error_code ec) {
    if (ec && ec != net::error::operation_aborted) {
        Logger::warn("[{}] Close error: {}", name_, ec.message());
    }

    Logger::info("[{}] WebSocket closed", name_);

    if (on_disconnect_) {
        on_disconnect_("Connection closed");
    }
}

void WebSocketClient::schedule_reconnect() {
    if (!should_reconnect_) {
        state_ = ConnectionState::Failed;
        return;
    }

    if (++reconnect_attempts_ > MAX_RECONNECT_ATTEMPTS) {
        Logger::error("[{}] Max reconnect attempts ({}) exceeded", name_, MAX_RECONNECT_ATTEMPTS);
        state_ = ConnectionState::Failed;
        if (on_error_) {
            on_error_("Max reconnect attempts exceeded");
        }
        return;
    }

    int attempts = reconnect_attempts_.load();
    int delay = RECONNECT_DELAY_MS * attempts;
    Logger::info("[{}] Reconnecting in {}ms (attempt {}/{})",
                 name_, delay, attempts, MAX_RECONNECT_ATTEMPTS);

    reconnect_timer_.expires_after(std::chrono::milliseconds(delay));
    reconnect_timer_.async_wait(
        beast::bind_front_handler(&WebSocketClient::on_reconnect_timer, shared_from_this()));
}

void WebSocketClient::on_reconnect_timer(beast::error_code ec) {
    if (ec == net::error::operation_aborted) {
        return;
    }

    if (ec) {
        Logger::error("[{}] Reconnect timer error: {}", name_, ec.message());
        return;
    }

    Logger::debug("[{}] Attempting reconnection", name_);
    state_ = ConnectionState::Connecting;
    do_resolve();
}

void WebSocketClient::schedule_ping() {
    ping_timer_.expires_after(std::chrono::milliseconds(PING_INTERVAL_MS));
    ping_timer_.async_wait(
        beast::bind_front_handler(&WebSocketClient::on_ping_timer, shared_from_this()));
}

void WebSocketClient::on_ping_timer(beast::error_code ec) {
    if (ec == net::error::operation_aborted) {
        return;
    }

    if (ec || state_.load() != ConnectionState::Connected) {
        return;
    }

    // Send ping
    ws_->async_ping({}, [self = shared_from_this()](beast::error_code ec) {
        if (ec) {
            Logger::warn("[{}] Ping failed: {}", self->name_, ec.message());
        }
    });

    // Schedule next ping
    schedule_ping();
}

void WebSocketClient::handle_error(const std::string& operation, beast::error_code ec) {
    if (ec == net::error::operation_aborted) {
        return;
    }

    std::string error = fmt::format("[{}] {} error: {}", name_, operation, ec.message());
    Logger::error("{}", error);
    notify_error(error);

    if (operation != "write") {
        reconnect();
    }
}

void WebSocketClient::notify_error(const std::string& error) {
    if (on_error_) {
        on_error_(error);
    }
}

} // namespace kimp::network
