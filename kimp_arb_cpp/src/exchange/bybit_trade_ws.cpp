#include "kimp/exchange/bybit/bybit_trade_ws.hpp"

#include <sstream>
#include <iomanip>

namespace kimp::exchange::bybit {

bool BybitTradeWS::connect() {
    if (credentials_.ws_trade_endpoint.empty()) {
        Logger::warn("[Bybit-TradeWS] No ws_trade_endpoint configured, WS trade disabled");
        return false;
    }

    ws_ = std::make_shared<network::WebSocketClient>(io_context_, "Bybit-Trade-WS");

    ws_->set_message_callback([this](std::string_view msg, network::MessageType type) {
        on_message(msg);
    });

    ws_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            connected_ = true;
            Logger::info("[Bybit-TradeWS] Connected, authenticating...");
            authenticate();
        } else {
            Logger::error("[Bybit-TradeWS] Connection failed: {}", error);
        }
    });

    ws_->set_disconnect_callback([this](const std::string& reason) {
        connected_ = false;
        authenticated_ = false;
        Logger::warn("[Bybit-TradeWS] Disconnected: {}", reason);

        // Cancel all pending orders
        std::lock_guard lock(pending_mutex_);
        for (auto& [req_id, promise] : pending_orders_) {
            PlaceOrderResult result;
            result.success = false;
            result.error_msg = "WebSocket disconnected";
            promise.set_value(std::move(result));
        }
        pending_orders_.clear();
    });

    ws_->connect(credentials_.ws_trade_endpoint);
    Logger::info("[Bybit-TradeWS] Connecting to {}", credentials_.ws_trade_endpoint);
    return true;
}

void BybitTradeWS::disconnect() {
    if (heartbeat_timer_) {
        heartbeat_timer_->cancel();
    }
    if (ws_) {
        ws_->disconnect();
    }
    connected_ = false;
    authenticated_ = false;
}

void BybitTradeWS::authenticate() {
    int64_t expires = utils::Crypto::timestamp_ms() + 10000;
    std::string val = "GET/realtime" + std::to_string(expires);
    std::string signature = utils::Crypto::hmac_sha256(credentials_.secret_key, val);

    std::ostringstream ss;
    ss << R"({"op":"auth","args":[")" << credentials_.api_key << R"(",)";
    ss << expires << R"(,")" << signature << R"("]})";

    ws_->send(ss.str());
}

Order BybitTradeWS::place_order_sync(const std::string& symbol, Side side, double qty,
                                      bool reduce_only, int position_idx) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = qty;
    order.create_time = std::chrono::system_clock::now();

    if (!is_connected()) {
        order.status = OrderStatus::Rejected;
        return order;
    }

    std::string req_id = utils::Crypto::generate_uuid();

    // Create promise/future pair
    std::promise<PlaceOrderResult> promise;
    std::future<PlaceOrderResult> future = promise.get_future();

    {
        std::lock_guard lock(pending_mutex_);
        pending_orders_.emplace(req_id, std::move(promise));
    }

    // Build WS order message
    std::ostringstream ss;
    ss << R"({"reqId":")" << req_id << R"(",)";
    ss << R"("header":{"X-BAPI-TIMESTAMP":")" << utils::Crypto::timestamp_ms() << R"("},)";
    ss << R"("op":"order.create",)";
    ss << R"("args":[{)";
    ss << R"("category":"linear",)";
    ss << R"("symbol":")" << symbol << R"(",)";
    ss << R"("side":")" << (side == Side::Buy ? "Buy" : "Sell") << R"(",)";
    ss << R"("orderType":"Market",)";
    ss << R"("qty":")" << std::fixed << std::setprecision(8) << qty << R"(",)";
    if (reduce_only) {
        ss << R"("reduceOnly":true,)";
    }
    if (position_idx > 0) {
        ss << R"("positionIdx":)" << position_idx << R"(,)";
    }
    ss << R"("timeInForce":"GTC")";
    ss << R"(}]})";

    ws_->send(ss.str());

    // Wait for response with timeout
    auto status = future.wait_for(std::chrono::milliseconds(ORDER_TIMEOUT_MS));
    if (status == std::future_status::timeout) {
        // Remove pending entry
        {
            std::lock_guard lock(pending_mutex_);
            pending_orders_.erase(req_id);
        }
        order.status = OrderStatus::Rejected;
        Logger::warn("[Bybit-TradeWS] Order timeout for {} {} {}",
                     symbol, side == Side::Buy ? "Buy" : "Sell", qty);
        return order;
    }

    PlaceOrderResult result = future.get();

    if (result.success) {
        order.status = OrderStatus::Filled;  // Assume filled for market orders (ACK only)
        order.order_id_str = result.order_id;
        order.exchange_order_id = std::hash<std::string>{}(result.order_id);
        Logger::info("[Bybit-TradeWS] Order ACK: {} {} {} orderId={}",
                     symbol, side == Side::Buy ? "Buy" : "Sell", qty, result.order_id);
    } else {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit-TradeWS] Order rejected: {} (retCode={})",
                      result.error_msg, result.ret_code);
    }

    return order;
}

void BybitTradeWS::on_message(std::string_view msg) {
    try {
        simdjson::padded_string padded(msg);
        auto doc = json_parser_.iterate(padded);

        // Check for auth response
        auto op = doc["op"];
        if (!op.error()) {
            std::string_view op_str = op.get_string().value();

            if (op_str == "auth") {
                auto success_field = doc["success"];
                if (!success_field.error() && success_field.get_bool().value()) {
                    authenticated_ = true;
                    Logger::info("[Bybit-TradeWS] Authenticated successfully");
                    schedule_heartbeat();
                } else {
                    Logger::error("[Bybit-TradeWS] Authentication failed");
                }
                return;
            }

            if (op_str == "pong") {
                return;
            }

            // Order response: op == "order.create"
            if (op_str == "order.create") {
                auto req_id_field = doc["reqId"];
                if (req_id_field.error()) return;

                std::string req_id(req_id_field.get_string().value());

                PlaceOrderResult result;
                auto ret_code = doc["retCode"];
                if (!ret_code.error()) {
                    result.ret_code = static_cast<int>(ret_code.get_int64().value());
                }

                if (result.ret_code == 0) {
                    result.success = true;
                    auto data = doc["data"];
                    if (!data.error()) {
                        auto order_id = data["orderId"];
                        if (!order_id.error()) {
                            result.order_id = std::string(order_id.get_string().value());
                        }
                    }
                } else {
                    result.success = false;
                    auto ret_msg = doc["retMsg"];
                    if (!ret_msg.error()) {
                        result.error_msg = std::string(ret_msg.get_string().value());
                    }
                }

                resolve_pending(req_id, std::move(result));
                return;
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        // Ignore parse errors for non-JSON messages (e.g., pong frames)
    }
}

void BybitTradeWS::schedule_heartbeat() {
    if (!heartbeat_timer_) {
        heartbeat_timer_ = std::make_unique<net::steady_timer>(io_context_);
    }

    heartbeat_timer_->expires_after(std::chrono::seconds(HEARTBEAT_INTERVAL_S));
    heartbeat_timer_->async_wait([this](boost::system::error_code ec) {
        on_heartbeat_timer(ec);
    });
}

void BybitTradeWS::on_heartbeat_timer(boost::system::error_code ec) {
    if (ec) return;  // Timer cancelled

    if (ws_ && connected_.load()) {
        ws_->send(std::string(R"({"op":"ping"})"));
        schedule_heartbeat();
    }
}

void BybitTradeWS::resolve_pending(const std::string& req_id, PlaceOrderResult result) {
    std::lock_guard lock(pending_mutex_);
    auto it = pending_orders_.find(req_id);
    if (it != pending_orders_.end()) {
        it->second.set_value(std::move(result));
        pending_orders_.erase(it);
    }
}

} // namespace kimp::exchange::bybit
