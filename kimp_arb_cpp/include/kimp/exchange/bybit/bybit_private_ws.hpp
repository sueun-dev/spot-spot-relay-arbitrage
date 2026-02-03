#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/network/websocket_client.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <functional>

namespace kimp::exchange::bybit {

/**
 * Bybit Private WebSocket for ultra-low latency order execution
 *
 * Benefits over REST:
 * - Persistent connection (no TLS handshake per order)
 * - ~5-20ms vs 50-200ms latency
 * - Real-time order updates
 */
class BybitPrivateWS {
public:
    using OrderCallback = std::function<void(uint64_t order_id, OrderStatus status,
                                              double filled_qty, double avg_price)>;

private:
    net::io_context& io_context_;
    std::shared_ptr<network::WebSocketClient> ws_;
    ExchangeCredentials credentials_;
    simdjson::ondemand::parser json_parser_;

    OrderCallback order_callback_;
    std::atomic<bool> authenticated_{false};

public:
    BybitPrivateWS(net::io_context& ioc, ExchangeCredentials creds)
        : io_context_(ioc), credentials_(std::move(creds)) {}

    bool connect() {
        ws_ = std::make_shared<network::WebSocketClient>(io_context_, "Bybit-Private-WS");

        ws_->set_message_callback([this](std::string_view msg, network::MessageType type) {
            on_message(msg);
        });

        ws_->set_connect_callback([this](bool success, const std::string& error) {
            if (success) {
                authenticate();
            }
        });

        ws_->connect(credentials_.ws_private_endpoint);
        return true;
    }

    void disconnect() {
        if (ws_) ws_->disconnect();
    }

    bool is_authenticated() const { return authenticated_.load(); }

    void set_order_callback(OrderCallback cb) { order_callback_ = std::move(cb); }

    /**
     * Place order via WebSocket (5-20ms latency vs 50-200ms REST)
     */
    void place_order(const std::string& symbol, Side side, double qty, bool reduce_only = false) {
        if (!authenticated_.load()) {
            Logger::error("[Bybit-WS] Not authenticated, cannot place order");
            return;
        }

        std::string req_id = utils::Crypto::generate_uuid();

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
        ss << R"("positionIdx":2)";  // Short position
        ss << R"(}]})";

        ws_->send(ss.str());
    }

private:
    void authenticate() {
        int64_t expires = utils::Crypto::timestamp_ms() + 10000;  // 10 seconds
        std::string val = "GET/realtime" + std::to_string(expires);
        std::string signature = utils::Crypto::hmac_sha256(credentials_.secret_key, val);

        std::ostringstream ss;
        ss << R"({"op":"auth","args":[")" << credentials_.api_key << R"(",)";
        ss << expires << R"(,")" << signature << R"("]})";

        ws_->send(ss.str());
    }

    void on_message(std::string_view msg) {
        try {
            simdjson::padded_string padded(msg);
            auto doc = json_parser_.iterate(padded);

            auto op = doc["op"];
            if (!op.error()) {
                std::string_view op_str = op.get_string().value();
                if (op_str == "auth") {
                    bool success = doc["success"].get_bool().value();
                    if (success) {
                        authenticated_ = true;
                        subscribe_orders();
                        Logger::info("[Bybit-WS] Authenticated successfully");
                    }
                }
            }

            auto topic = doc["topic"];
            if (!topic.error()) {
                std::string_view topic_str = topic.get_string().value();
                if (topic_str == "order") {
                    parse_order_update(doc);
                }
            }
        } catch (const simdjson::simdjson_error& e) {
            // Ignore parse errors for non-JSON messages
        }
    }

    void subscribe_orders() {
        ws_->send(R"({"op":"subscribe","args":["order"]})");
    }

    void parse_order_update(simdjson::ondemand::document& doc) {
        try {
            auto data = doc["data"].get_array();
            for (auto order : data) {
                std::string_view order_id = order["orderId"].get_string().value();
                std::string_view status = order["orderStatus"].get_string().value();
                std::string_view filled_str = order["cumExecQty"].get_string().value();
                std::string_view avg_str = order["avgPrice"].get_string().value();

                OrderStatus os = OrderStatus::New;
                if (status == "Filled") os = OrderStatus::Filled;
                else if (status == "PartiallyFilled") os = OrderStatus::PartiallyFilled;
                else if (status == "Cancelled") os = OrderStatus::Cancelled;
                else if (status == "Rejected") os = OrderStatus::Rejected;

                if (order_callback_) {
                    order_callback_(
                        std::hash<std::string_view>{}(order_id),
                        os,
                        std::stod(std::string(filled_str)),
                        std::stod(std::string(avg_str))
                    );
                }
            }
        } catch (...) {}
    }
};

} // namespace kimp::exchange::bybit
