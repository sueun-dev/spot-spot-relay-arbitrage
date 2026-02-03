#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <jwt-cpp/jwt.h>
#include <simdjson.h>

namespace kimp::exchange::upbit {

/**
 * Upbit exchange implementation
 *
 * Features:
 * - JWT authentication for REST API
 * - WebSocket for real-time ticker data
 * - KRW spot markets only
 */
class UpbitExchange : public KoreanExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{4096};

    // Cached USDT/KRW price
    std::atomic<double> usdt_krw_price_{0.0};

public:
    UpbitExchange(net::io_context& ioc, ExchangeCredentials creds)
        : KoreanExchangeBase(Exchange::Upbit, MarketType::Spot, "Upbit", ioc, std::move(creds)) {
    }

    // Connection
    bool connect() override;
    void disconnect() override;

    // Subscriptions
    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    // Market data
    std::vector<SymbolId> get_available_symbols() override;
    double get_funding_rate(const SymbolId& symbol) override { return 0.0; }  // N/A for spot
    double get_usdt_krw_price() override;

    // Orders
    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override;
    bool cancel_order(uint64_t order_id) override;

    // Balance
    double get_balance(const std::string& currency) override;

protected:
    // WebSocket handlers
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    // JWT authentication
    std::string generate_jwt_token() const;
    std::string generate_jwt_token_with_query(const std::string& query_string) const;

    // Build REST headers
    std::unordered_map<std::string, std::string> build_auth_headers() const;
    std::unordered_map<std::string, std::string> build_auth_headers(const std::string& query) const;

    // Parse responses
    bool parse_ticker_message(std::string_view message, Ticker& ticker);
    bool parse_order_response(const std::string& response, Order& order);
};

} // namespace kimp::exchange::upbit
