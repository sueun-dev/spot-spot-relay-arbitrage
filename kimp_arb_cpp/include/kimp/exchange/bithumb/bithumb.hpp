#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <map>

namespace kimp::exchange::bithumb {

/**
 * Bithumb exchange implementation
 *
 * Features:
 * - HMAC-SHA512 authentication
 * - WebSocket for real-time data
 * - KRW spot markets
 * - Real orderbook bid/ask via orderbookdepth WebSocket
 */
class BithumbExchange : public KoreanExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{4096};

    std::atomic<double> usdt_krw_price_{0.0};

    // Store subscribed symbols for reconnection
    std::vector<SymbolId> subscribed_tickers_;
    std::vector<SymbolId> subscribed_orderbooks_;
    std::mutex subscription_mutex_;

    // --- Orderbook state for real bid/ask ---
    struct OrderbookState {
        std::map<double, double, std::greater<>> bids;  // price→qty, descending (best bid = begin)
        std::map<double, double> asks;                   // price→qty, ascending (best ask = begin)
        bool initialized{false};
    };

    // Per-symbol orderbook levels (keyed by "BTC_KRW" format)
    std::unordered_map<std::string, OrderbookState> orderbook_state_;
    std::mutex orderbook_mutex_;

    // Lock-free BBO cache for hot ticker path
    struct BBO {
        std::atomic<double> best_bid{0.0};
        std::atomic<double> best_ask{0.0};
    };
    std::unordered_map<std::string, BBO> orderbook_bbo_;
    std::atomic<bool> orderbook_ready_{false};

    // Cache last known ticker price per symbol for orderbookdepth-driven dispatch
    std::unordered_map<std::string, double> last_price_cache_;

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
    Order place_market_buy_quantity(const SymbolId& symbol, Quantity quantity);
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
    std::vector<std::string> parse_orderbookdepth_message(std::string_view message);
    void update_bbo(const std::string& symbol_key);
    bool query_order_detail(const std::string& order_id, const SymbolId& symbol, Order& order);

public:
    void fetch_all_orderbook_snapshots(const std::vector<SymbolId>& symbols);
};

} // namespace kimp::exchange::bithumb
