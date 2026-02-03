#include "kimp/network/ws_broadcast_server.hpp"
#include "kimp/core/logger.hpp"

namespace kimp::network {

// ============================================================================
// WsBroadcastServer Implementation
// ============================================================================

WsBroadcastServer::WsBroadcastServer(net::io_context& ioc, unsigned short port)
    : ioc_(ioc)
    , acceptor_(net::make_strand(ioc))
    , port_(port)
{
}

WsBroadcastServer::~WsBroadcastServer() {
    stop();
}

void WsBroadcastServer::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    beast::error_code ec;

    // Open the acceptor
    auto endpoint = tcp::endpoint(tcp::v4(), port_);
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        Logger::error("[WS-Server] Failed to open acceptor: {}", ec.message());
        running_ = false;
        return;
    }

    // Allow address reuse
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);

    // Bind to the server address
    acceptor_.bind(endpoint, ec);
    if (ec) {
        Logger::error("[WS-Server] Failed to bind to port {}: {}", port_, ec.message());
        running_ = false;
        return;
    }

    // Start listening
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        Logger::error("[WS-Server] Failed to listen: {}", ec.message());
        running_ = false;
        return;
    }

    Logger::info("[WS-Server] Listening on ws://localhost:{}", port_);
    do_accept();
}

void WsBroadcastServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    beast::error_code ec;
    acceptor_.close(ec);

    // Close all sessions
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        session->close();
    }
    sessions_.clear();

    Logger::info("[WS-Server] Stopped");
}

void WsBroadcastServer::do_accept() {
    if (!running_) return;

    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&WsBroadcastServer::on_accept, shared_from_this()));
}

void WsBroadcastServer::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        if (running_) {
            Logger::warn("[WS-Server] Accept error: {}", ec.message());
        }
    } else {
        // Create and start the session
        auto session = std::make_shared<WebSocketSession>(std::move(socket), shared_from_this());
        session->start();
    }

    // Accept next connection
    do_accept();
}

void WsBroadcastServer::broadcast(const std::string& message) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        session->send(message);
    }
}

void WsBroadcastServer::broadcast(std::string&& message) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        session->send(message);
    }
}

size_t WsBroadcastServer::connection_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

void WsBroadcastServer::join(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.insert(session);
    Logger::info("[WS-Server] Client connected (total: {})", sessions_.size());
}

void WsBroadcastServer::leave(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session);
    Logger::info("[WS-Server] Client disconnected (total: {})", sessions_.size());
}

// ============================================================================
// WebSocketSession Implementation
// ============================================================================

WebSocketSession::WebSocketSession(tcp::socket&& socket, std::shared_ptr<WsBroadcastServer> server)
    : ws_(std::move(socket))
    , server_(server)
{
    // Optimize for low latency
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
        res.set(beast::http::field::server, "KIMP-Bot");
        res.set(beast::http::field::access_control_allow_origin, "*");
    }));
}

void WebSocketSession::start() {
    // Accept the WebSocket handshake
    ws_.async_accept(
        beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this()));
}

void WebSocketSession::on_accept(beast::error_code ec) {
    if (ec) {
        Logger::warn("[WS-Session] Accept error: {}", ec.message());
        return;
    }

    // Join the server's session list
    if (auto server = server_.lock()) {
        server->join(shared_from_this());
    }

    // Start reading (to detect disconnection)
    do_read();
}

void WebSocketSession::do_read() {
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
}

void WebSocketSession::on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec == websocket::error::closed) {
        if (auto server = server_.lock()) {
            server->leave(shared_from_this());
        }
        return;
    }

    if (ec) {
        if (auto server = server_.lock()) {
            server->leave(shared_from_this());
        }
        return;
    }

    // Clear the buffer and continue reading
    buffer_.consume(buffer_.size());
    do_read();
}

void WebSocketSession::send(const std::string& message) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(message);

    if (!writing_) {
        writing_ = true;
        // Post to strand to ensure thread safety
        net::post(ws_.get_executor(), [self = shared_from_this()]() {
            self->do_write();
        });
    }
}

void WebSocketSession::do_write() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.empty()) {
            writing_ = false;
            return;
        }
        // Store in member variable so it persists during async_write
        current_message_ = std::move(queue_.front());
        queue_.erase(queue_.begin());
    }

    ws_.text(true);
    ws_.async_write(
        net::buffer(current_message_),
        beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
}

void WebSocketSession::on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        if (auto server = server_.lock()) {
            server->leave(shared_from_this());
        }
        return;
    }

    do_write();  // Write next message in queue
}

void WebSocketSession::close() {
    beast::error_code ec;
    ws_.close(websocket::close_code::normal, ec);
}

} // namespace kimp::network
