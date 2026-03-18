#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <map>
#include <condition_variable>
#include <thread>

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
    static constexpr std::size_t ORDERBOOK_SNAPSHOT_DEPTH = 5;
    static constexpr std::size_t ORDERBOOK_BBO_DEPTH = 1;
    static constexpr auto ORDERBOOK_RESYNC_INTERVAL = std::chrono::milliseconds(500);

    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{4096};

    std::atomic<double> usdt_krw_price_{0.0};
    std::shared_ptr<network::WebSocketClient> private_ws_;
    std::atomic<bool> private_ws_authenticated_{false};

    struct FillInfo {
        double avg_price{0.0};
        double filled_qty{0.0};
    };
    std::mutex fill_cache_mutex_;
    std::condition_variable fill_cache_cv_;
    std::unordered_map<std::string, FillInfo> fill_cache_;

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

    // Per-symbol orderbook levels (keyed by SymbolId — zero-alloc lookup)
    std::unordered_map<SymbolId, OrderbookState> orderbook_state_;
    std::mutex orderbook_mutex_;

    // Lock-free BBO cache for hot ticker path
    struct BBO {
        std::atomic<double> best_bid{0.0};
        std::atomic<double> best_ask{0.0};
        std::atomic<double> best_bid_qty{0.0};
        std::atomic<double> best_ask_qty{0.0};
    };
    std::unordered_map<SymbolId, BBO> orderbook_bbo_;
    std::atomic<bool> orderbook_ready_{false};
    std::atomic<bool> orderbook_resync_running_{false};
    std::thread orderbook_resync_thread_;

    // Cache last known ticker price per symbol for orderbookdepth-driven dispatch
    std::unordered_map<SymbolId, double> last_price_cache_;

public:
    BithumbExchange(net::io_context& ioc, ExchangeCredentials creds)
        : KoreanExchangeBase(Exchange::Bithumb, MarketType::Spot, "Bithumb", ioc, std::move(creds)) {
    }
    ~BithumbExchange() override;

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    std::vector<Ticker> fetch_all_tickers() override;
    double get_usdt_krw_price() override;

    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override;
    Order place_market_buy_quantity(const SymbolId& symbol, Quantity quantity);
    bool cancel_order(uint64_t order_id) override;

    double get_balance(const std::string& currency) override;

    // Public fill query for async/parallel execution from OrderManager
    bool query_order_detail(const std::string& order_id, const SymbolId& symbol, Order& order);

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;
    void on_private_ws_message(std::string_view message);

private:
    std::string generate_signature(const std::string& endpoint,
                                    const std::string& params,
                                    int64_t timestamp) const;
    std::string generate_v1_jwt_token() const;
    std::string generate_v1_jwt_token_with_query(const std::string& query_string) const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& endpoint, const std::string& params = "") const;
    std::unordered_map<std::string, std::string> build_v1_auth_headers() const;
    std::unordered_map<std::string, std::string> build_v1_auth_headers(
        const std::string& query_string) const;

    bool query_order_detail_v1(const std::string& order_id, Order& order);
    bool query_order_detail_legacy(const std::string& order_id,
                                   const SymbolId& symbol,
                                   Order& order);
    bool query_order_detail_ws(const std::string& order_id, Order& order);

    std::optional<Ticker> make_bbo_ticker(const SymbolId& symbol);
    bool parse_ticker_message(std::string_view message, Ticker& ticker);
    std::vector<SymbolId> parse_orderbookdepth_message(std::string_view message);
    void update_bbo(const SymbolId& symbol);
    void start_orderbook_resync_loop();
    void stop_orderbook_resync_loop();
    std::string resolve_private_ws_endpoint() const;
    void subscribe_private_myorder();

public:
    void fetch_all_orderbook_snapshots(const std::vector<SymbolId>& symbols,
                                       std::size_t depth_count = ORDERBOOK_SNAPSHOT_DEPTH);

    // Fetch per-coin withdrawal fees from /v2/fee/inout/ALL (public, no auth).
    // Returns map: uppercase base symbol → minimum withdrawal fee in coin units across all networks.
    // Returns per-coin per-network withdrawal fees.
    // {coin → [{network, fee_coins}]}
    struct NetworkFee { std::string network; double fee_coins{0.0}; };
    std::unordered_map<std::string, std::vector<NetworkFee>> fetch_withdrawal_fees();

    struct AssetStatus {
        bool deposit_enabled{false};
        bool withdraw_enabled{false};
    };
    std::unordered_map<std::string, AssetStatus> fetch_asset_statuses();
};

} // namespace kimp::exchange::bithumb
