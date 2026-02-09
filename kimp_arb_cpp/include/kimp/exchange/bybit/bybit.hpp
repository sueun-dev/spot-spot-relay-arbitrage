#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/exchange/bybit/bybit_trade_ws.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <shared_mutex>
#include <memory>
#include <condition_variable>

namespace kimp::exchange::bybit {

/**
 * Bybit exchange implementation
 *
 * Features:
 * - HMAC-SHA256 authentication
 * - USDT perpetual futures
 * - WebSocket for real-time data
 * - Position management with positionIdx
 */
class BybitExchange : public ForeignFuturesExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{8192};

    static constexpr int SHORT_POSITION_IDX = 2;

    // Cache funding intervals (symbol -> interval in hours) from instruments-info
    std::unordered_map<std::string, int> funding_interval_cache_;
    struct LotSize {
        double min_qty{0.0};
        double qty_step{0.0};
        double min_notional{0.0};
    };
    std::unordered_map<std::string, LotSize> lot_size_cache_;
    mutable std::shared_mutex metadata_mutex_;

    // WebSocket Trade API for low-latency order placement
    std::unique_ptr<BybitTradeWS> trade_ws_;

    // Private WS for real-time fill data (replaces REST fill query)
    std::shared_ptr<network::WebSocketClient> private_ws_;
    std::atomic<bool> private_ws_authenticated_{false};

    // Fill cache: orderId → {avgPrice, filledQty} populated by Private WS order stream
    struct FillInfo {
        double avg_price{0.0};
        double filled_qty{0.0};
    };
    std::mutex fill_cache_mutex_;
    std::condition_variable fill_cache_cv_;
    std::unordered_map<std::string, FillInfo> fill_cache_;

    // Store subscribed symbols for reconnection
    std::vector<SymbolId> subscribed_tickers_;
    std::mutex subscription_mutex_;

public:
    BybitExchange(net::io_context& ioc, ExchangeCredentials creds)
        : ForeignFuturesExchangeBase(Exchange::Bybit, MarketType::Perpetual, "Bybit", ioc, std::move(creds)) {
    }

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    double get_funding_rate(const SymbolId& symbol) override;
    std::vector<Ticker> fetch_all_tickers() override;

    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    bool cancel_order(uint64_t order_id) override;

    // Futures specific
    bool set_leverage(const SymbolId& symbol, int leverage) override;
    std::vector<Position> get_positions() override;
    bool close_position(const SymbolId& symbol) override;
    Order open_short(const SymbolId& symbol, Quantity quantity) override;
    Order close_short(const SymbolId& symbol, Quantity quantity) override;

    double get_balance(const std::string& currency) override;

    // Public fill query for async/parallel execution from OrderManager
    bool query_order_fill(const std::string& order_id, Order& order);

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    std::string generate_signature(int64_t timestamp, const std::string& params) const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& params = "") const;

    bool parse_ticker_message(std::string_view message, Ticker& ticker);
    bool parse_order_response(const std::string& response, Order& order, std::string* order_id_out = nullptr);
    double normalize_order_qty(const SymbolId& symbol, double qty, bool is_open) const;

    std::string symbol_to_bybit(const SymbolId& symbol) const {
        return std::string(symbol.get_base()) + std::string(symbol.get_quote());
    }

    // Private WS for fill data
    void authenticate_private_ws();
    void on_private_ws_message(std::string_view message);
};

} // namespace kimp::exchange::bybit
