#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>

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

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    std::string generate_signature(int64_t timestamp, const std::string& params) const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& params = "") const;

    bool parse_ticker_message(std::string_view message, Ticker& ticker);
    bool parse_order_response(const std::string& response, Order& order);

    std::string symbol_to_bybit(const SymbolId& symbol) const {
        return std::string(symbol.get_base()) + std::string(symbol.get_quote());
    }
};

} // namespace kimp::exchange::bybit
