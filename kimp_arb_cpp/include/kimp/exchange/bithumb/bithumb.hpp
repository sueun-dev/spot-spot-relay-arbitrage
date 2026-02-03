#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>

namespace kimp::exchange::bithumb {

/**
 * Bithumb exchange implementation
 *
 * Features:
 * - HMAC-SHA512 authentication
 * - WebSocket for real-time data
 * - KRW spot markets
 */
class BithumbExchange : public KoreanExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{4096};

    std::atomic<double> usdt_krw_price_{0.0};

public:
    BithumbExchange(net::io_context& ioc, ExchangeCredentials creds)
        : KoreanExchangeBase(Exchange::Bithumb, MarketType::Spot, "Bithumb", ioc, std::move(creds)) {
    }

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    double get_funding_rate(const SymbolId& symbol) override { return 0.0; }
    std::vector<Ticker> fetch_all_tickers() override;
    double get_usdt_krw_price() override;

    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override;
    bool cancel_order(uint64_t order_id) override;

    double get_balance(const std::string& currency) override;

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    std::string generate_signature(const std::string& endpoint,
                                    const std::string& params,
                                    int64_t timestamp) const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& endpoint, const std::string& params = "") const;

    bool parse_ticker_message(std::string_view message, Ticker& ticker);
};

} // namespace kimp::exchange::bithumb
