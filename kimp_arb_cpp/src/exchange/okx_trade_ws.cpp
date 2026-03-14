#include "kimp/exchange/okx/okx_trade_ws.hpp"

#include <cstdio>

namespace kimp::exchange::okx {

bool OkxTradeWS::connect() {
    // OKX trade orders go through the private WS endpoint
    if (credentials_.ws_private_endpoint.empty()) {
        Logger::warn("[OKX-TradeWS] No ws_private_endpoint configured, WS trade disabled");
        return false;
    }

    ws_ = std::make_shared<network::WebSocketClient>(io_context_, "OKX-Trade-WS");

    ws_->set_message_callback([this](std::string_view msg, network::MessageType /*type*/) {
        on_message(msg);
    });

    ws_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            connected_ = true;
            Logger::info("[OKX-TradeWS] Connected, authenticating...");
            authenticate();
        } else {
            Logger::error("[OKX-TradeWS] Connection failed: {}", error);
        }
    });

    ws_->set_disconnect_callback([this](const std::string& reason) {
        connected_ = false;
        authenticated_ = false;
        Logger::warn("[OKX-TradeWS] Disconnected: {}", reason);

        // Cancel all pending orders
        std::lock_guard lock(pending_mutex_);
        for (auto& [msg_id, promise] : pending_orders_) {
            PlaceOrderResult result;
            result.success = false;
            result.error_msg = "WebSocket disconnected";
            promise.set_value(std::move(result));
        }
        pending_orders_.clear();
    });

    ws_->connect(credentials_.ws_private_endpoint);
    Logger::info("[OKX-TradeWS] Connecting to {}", credentials_.ws_private_endpoint);
    return true;
}

void OkxTradeWS::disconnect() {
    if (heartbeat_timer_) {
        heartbeat_timer_->cancel();
    }
    if (ws_) {
        ws_->disconnect();
    }
    connected_ = false;
    authenticated_ = false;
}

void OkxTradeWS::authenticate() {
    // OKX WS auth: sign = Base64(HMAC-SHA256(timestamp + "GET" + "/users/self/verify", secret))
    int64_t ts = utils::Crypto::timestamp_sec();
    std::string ts_str = std::to_string(ts);
    std::string sign_payload = ts_str + "GET" + "/users/self/verify";

    auto hmac_raw = utils::Crypto::hmac_sha256_raw(credentials_.secret_key, sign_payload);
    std::string signature = utils::Crypto::base64_encode(hmac_raw);

    char buf[512];
    int len = std::snprintf(buf, sizeof(buf),
        "{\"op\":\"login\",\"args\":[{\"apiKey\":\"%s\",\"passphrase\":\"%s\","
        "\"timestamp\":\"%s\",\"sign\":\"%s\"}]}",
        credentials_.api_key.c_str(),
        credentials_.passphrase.c_str(),
        ts_str.c_str(),
        signature.c_str());
    if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
        ws_->send(std::string(buf, static_cast<size_t>(len)));
    }
}

Order OkxTradeWS::place_order_sync(const std::string& inst_id, Side side, double qty,
                                    const std::string& td_mode) {
    Order order;
    order.exchange = Exchange::OKX;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = qty;
    order.create_time = std::chrono::system_clock::now();

    if (!is_connected()) {
        order.status = OrderStatus::Rejected;
        return order;
    }

    std::string msg_id = utils::Crypto::generate_uuid();

    // Create promise/future pair
    std::promise<PlaceOrderResult> promise;
    std::future<PlaceOrderResult> future = promise.get_future();

    {
        std::lock_guard lock(pending_mutex_);
        pending_orders_.emplace(msg_id, std::move(promise));
    }

    // Build WS order message — snprintf avoids ostringstream heap allocs on hot path
    // OKX format: {"id":"msgId","op":"order","args":[{"instId":"BTC-USDT","tdMode":"cross",
    //              "side":"sell","ordType":"market","sz":"0.001"}]}
    char buf[512];
    int len = std::snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"op\":\"order\",\"args\":[{"
        "\"instId\":\"%s\",\"tdMode\":\"%s\",\"side\":\"%s\","
        "\"ordType\":\"market\",\"sz\":\"%.8f\"}]}",
        msg_id.c_str(),
        inst_id.c_str(),
        td_mode.c_str(),
        side == Side::Buy ? "buy" : "sell",
        qty);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) {
        order.status = OrderStatus::Rejected;
        {
            std::lock_guard lock(pending_mutex_);
            pending_orders_.erase(msg_id);
        }
        return order;
    }
    ws_->send(std::string(buf, static_cast<size_t>(len)));

    // Wait for response with timeout
    auto status = future.wait_for(std::chrono::milliseconds(ORDER_TIMEOUT_MS));
    if (status == std::future_status::timeout) {
        // Remove pending entry
        {
            std::lock_guard lock(pending_mutex_);
            pending_orders_.erase(msg_id);
        }
        order.status = OrderStatus::Rejected;
        Logger::warn("[OKX-TradeWS] Order timeout for {} {} {}",
                     inst_id, side == Side::Buy ? "buy" : "sell", qty);
        return order;
    }

    PlaceOrderResult result = future.get();

    if (result.success) {
        order.status = OrderStatus::Filled;  // Assume filled for market orders (ACK only)
        order.order_id_str = result.order_id;
        order.exchange_order_id = std::hash<std::string>{}(result.order_id);
        Logger::info("[OKX-TradeWS] Order ACK: {} {} {} orderId={}",
                     inst_id, side == Side::Buy ? "buy" : "sell", qty, result.order_id);
    } else {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX-TradeWS] Order rejected: {} (code={})",
                      result.error_msg, result.ret_code);
    }

    return order;
}

void OkxTradeWS::on_message(std::string_view msg) {
    // OKX keepalive: literal "pong" response
    if (msg == "pong") {
        return;
    }

    try {
        simdjson::padded_string padded(msg);
        auto doc = json_parser_.iterate(padded);

        // Check for event field (login response)
        auto event_field = doc["event"];
        if (!event_field.error()) {
            std::string_view event_str = event_field.get_string().value();
            if (event_str == "login") {
                auto code_field = doc["code"];
                if (!code_field.error()) {
                    std::string_view code_str = code_field.get_string().value();
                    if (code_str == "0") {
                        authenticated_ = true;
                        Logger::info("[OKX-TradeWS] Authenticated successfully");
                        schedule_heartbeat();
                    } else {
                        auto msg_field = doc["msg"];
                        std::string_view msg_str = msg_field.error() ? "unknown" : msg_field.get_string().value();
                        Logger::error("[OKX-TradeWS] Authentication failed: code={} msg={}", code_str, msg_str);
                    }
                }
                return;
            }
            return;
        }

        // Order response: {"id":"msgId","op":"order","code":"0","data":[{"ordId":"...","sCode":"0"}]}
        auto op_field = doc["op"];
        if (!op_field.error()) {
            std::string_view op_str = op_field.get_string().value();

            if (op_str == "order") {
                auto id_field = doc["id"];
                if (id_field.error()) return;

                std::string msg_id(id_field.get_string().value());

                PlaceOrderResult result;
                auto code_field = doc["code"];
                if (!code_field.error()) {
                    std::string_view code_str = code_field.get_string().value();
                    result.ret_code = std::atoi(std::string(code_str).c_str());
                }

                if (result.ret_code == 0) {
                    result.success = true;
                    auto data = doc["data"].get_array();
                    for (auto item : data) {
                        auto s_code = item["sCode"];
                        if (!s_code.error()) {
                            std::string_view sc = s_code.get_string().value();
                            if (sc != "0") {
                                result.success = false;
                                auto s_msg = item["sMsg"];
                                if (!s_msg.error()) {
                                    result.error_msg = std::string(s_msg.get_string().value());
                                }
                                break;
                            }
                        }
                        auto ord_id = item["ordId"];
                        if (!ord_id.error()) {
                            result.order_id = std::string(ord_id.get_string().value());
                        }
                        break;  // Only first element
                    }
                } else {
                    result.success = false;
                    auto msg_field = doc["msg"];
                    if (!msg_field.error()) {
                        result.error_msg = std::string(msg_field.get_string().value());
                    }
                }

                resolve_pending(msg_id, std::move(result));
                return;
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        // Ignore parse errors for non-JSON messages (e.g., pong frames)
    }
}

void OkxTradeWS::schedule_heartbeat() {
    if (!heartbeat_timer_) {
        heartbeat_timer_ = std::make_unique<net::steady_timer>(io_context_);
    }

    heartbeat_timer_->expires_after(std::chrono::seconds(HEARTBEAT_INTERVAL_S));
    heartbeat_timer_->async_wait([this](boost::system::error_code ec) {
        on_heartbeat_timer(ec);
    });
}

void OkxTradeWS::on_heartbeat_timer(boost::system::error_code ec) {
    if (ec) return;  // Timer cancelled

    if (ws_ && connected_.load()) {
        // OKX keepalive: send literal "ping" string
        ws_->send(std::string("ping"));
        schedule_heartbeat();
    }
}

void OkxTradeWS::resolve_pending(const std::string& msg_id, PlaceOrderResult result) {
    std::lock_guard lock(pending_mutex_);
    auto it = pending_orders_.find(msg_id);
    if (it != pending_orders_.end()) {
        it->second.set_value(std::move(result));
        pending_orders_.erase(it);
    }
}

} // namespace kimp::exchange::okx
