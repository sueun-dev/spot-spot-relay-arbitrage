#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/network/websocket_client.hpp"
#include "kimp/utils/crypto.hpp"
#include "kimp/core/logger.hpp"

#include <simdjson.h>
#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace kimp::exchange::bybit {

/**
 * Bybit WebSocket Trade API for ultra-low latency order placement
 *
 * Connects to wss://stream.bybit.com/v5/trade (separate from public/private WS)
 * Provides ~5-20ms order placement vs ~150-300ms REST
 * Returns ACK only (orderId) — fill data must be queried via REST
 */
class BybitTradeWS {
public:
    struct PlaceOrderResult {
        bool success{false};
        std::string order_id;
        std::string error_msg;
        int ret_code{-1};
    };

private:
    net::io_context& io_context_;
    std::shared_ptr<network::WebSocketClient> ws_;
    ExchangeCredentials credentials_;
    simdjson::ondemand::parser json_parser_;

    // Pending order requests: reqId -> promise
    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::promise<PlaceOrderResult>> pending_orders_;

    std::atomic<bool> authenticated_{false};
    std::atomic<bool> connected_{false};

    // Heartbeat timer
    std::unique_ptr<net::steady_timer> heartbeat_timer_;
    static constexpr int HEARTBEAT_INTERVAL_S = 20;
    static constexpr int ORDER_TIMEOUT_MS = 3000;

public:
    BybitTradeWS(net::io_context& ioc, ExchangeCredentials creds)
        : io_context_(ioc), credentials_(std::move(creds)) {}

    bool connect();
    void disconnect();

    bool is_connected() const noexcept { return connected_.load() && authenticated_.load(); }

    /**
     * Place order synchronously via WebSocket Trade API
     * Blocks until ACK received or timeout (3s)
     * Returns Order with status Filled (on ACK success) or Rejected (on failure/timeout)
     */
    Order place_order_sync(const std::string& symbol, Side side, double qty,
                           bool reduce_only, int position_idx);

private:
    void authenticate();
    void on_message(std::string_view msg);
    void schedule_heartbeat();
    void on_heartbeat_timer(boost::system::error_code ec);
    void resolve_pending(const std::string& req_id, PlaceOrderResult result);
};

} // namespace kimp::exchange::bybit
