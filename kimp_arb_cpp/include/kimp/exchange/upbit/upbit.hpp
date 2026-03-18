#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/core/optimization.hpp"

#include <simdjson.h>
#include <map>
#include <condition_variable>

namespace kimp::exchange::upbit {

/**
 * Upbit exchange implementation
 *
 * Features:
 * - KRW spot markets (Korean exchange)
 * - WebSocket for real-time orderbook data (gzip-compressed binary frames)
 * - REST API for available markets (/v1/market/all)
 * - Symbol format: "KRW-BTC" (quote-base)
 */
class UpbitExchange : public KoreanExchangeBase {
private:
    std::atomic<double> usdt_krw_price_{0.0};

    // Store subscribed symbols for reconnection
    std::vector<SymbolId> subscribed_tickers_;
    std::vector<SymbolId> subscribed_orderbooks_;
    std::mutex subscription_mutex_;

    // Lock-free BBO cache for hot ticker path
    struct BBO {
        std::atomic<double> best_bid{0.0};
        std::atomic<double> best_ask{0.0};
        std::atomic<double> best_bid_qty{0.0};
        std::atomic<double> best_ask_qty{0.0};
    };
    std::unordered_map<SymbolId, BBO> orderbook_bbo_;

    // Cache last known ticker price per symbol
    std::unordered_map<SymbolId, double> last_price_cache_;
    std::mutex last_price_mutex_;

    // Decompression buffer for gzip WebSocket frames
    std::vector<char> decompress_buf_;

public:
    UpbitExchange(net::io_context& ioc, ExchangeCredentials creds)
        : KoreanExchangeBase(Exchange::Upbit, MarketType::Spot, "Upbit", ioc, std::move(creds)) {
        decompress_buf_.resize(65536);
    }

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    std::vector<Ticker> fetch_all_tickers() override;
    double get_usdt_krw_price() override;

    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override;
    bool cancel_order(uint64_t order_id) override;

    double get_balance(const std::string& currency) override;
    bool query_order_detail(const std::string& order_id, Order& order);

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    // Decompress gzip data from WebSocket binary frames
    bool decompress_gzip(const char* data, size_t len, std::string& output);

    // Fast parser for Upbit orderbook WS message (already decompressed JSON)
    bool parse_orderbook_message(std::string_view message);

    // Fast parser for Upbit ticker WS message
    bool parse_ticker_message(std::string_view message);

    std::string symbol_to_upbit(const SymbolId& symbol) const {
        return std::string(symbol.get_quote()) + "-" + std::string(symbol.get_base());
    }

    // JWT authentication for Upbit API (HS512)
    std::string generate_jwt_token() const;
    std::string generate_jwt_token_with_query(const std::string& query_string) const;

public:
    // Fetch per-network withdrawal fees for given coins.
    // Step 1: GET /v1/status/wallet → discover net_types per coin.
    // Step 2: GET /v1/withdraws/chance per (coin, net_type) → extract fee.
    // Returns {coin → [{network, fee_coins}]}.
    struct NetworkFee { std::string network; double fee_coins{0.0}; };
    std::unordered_map<std::string, std::vector<NetworkFee>> fetch_withdrawal_fees(
        const std::vector<std::string>& coins);
};

} // namespace kimp::exchange::upbit
