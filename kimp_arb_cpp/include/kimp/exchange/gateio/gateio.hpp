#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>

namespace kimp::exchange::gateio {

/**
 * Gate.io exchange implementation
 *
 * Features:
 * - HMAC-SHA512 authentication
 * - USDT perpetual futures
 * - WebSocket for real-time data
 * - Contract size handling
 */
class GateIOExchange : public ForeignFuturesExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{8192};

    // Cache contract sizes
    std::unordered_map<std::string, double> contract_sizes_;
    mutable std::mutex contract_mutex_;

public:
    GateIOExchange(net::io_context& ioc, ExchangeCredentials creds)
        : ForeignFuturesExchangeBase(Exchange::GateIO, MarketType::Perpetual, "GateIO", ioc, std::move(creds)) {
    }

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    double get_funding_rate(const SymbolId& symbol) override;

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
    std::string generate_signature(const std::string& method,
                                    const std::string& url,
                                    const std::string& query_string,
                                    const std::string& body,
                                    int64_t timestamp) const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& method,
        const std::string& url,
        const std::string& query = "",
        const std::string& body = "") const;

    bool parse_ticker_message(std::string_view message, Ticker& ticker);

    std::string symbol_to_gateio(const SymbolId& symbol) const {
        return std::string(symbol.get_base()) + "_" + std::string(symbol.get_quote());
    }

    double get_contract_size(const SymbolId& symbol);
    int64_t coins_to_contracts(const SymbolId& symbol, double coins);
};

} // namespace kimp::exchange::gateio
