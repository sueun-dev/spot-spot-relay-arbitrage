#pragma once

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/exchange/bybit/bybit_trade_ws.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <shared_mutex>
#include <memory>
#include <condition_variable>
#include <unordered_set>

namespace kimp::exchange::bybit {

/**
 * Bybit exchange implementation
 *
 * Features:
 * - HMAC-SHA256 authentication
 * - USDT spot markets
 * - Spot margin short execution (`isLeverage=1`)
 * - WebSocket for real-time data
 * - Borrow liability tracking via wallet balance
 */
class BybitExchange : public ForeignShortExchangeBase {
private:
    simdjson::ondemand::parser json_parser_;
    simdjson::padded_string json_buffer_{8192};
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
    std::atomic<bool> public_ws_parse_warned_{false};

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
    std::vector<SymbolId> subscribed_orderbooks_;
    std::mutex subscription_mutex_;
    std::atomic<bool> spot_margin_mode_ready_{false};
    std::mutex spot_margin_mutex_;

public:
    BybitExchange(net::io_context& ioc, ExchangeCredentials creds)
        : ForeignShortExchangeBase(Exchange::Bybit, MarketType::MarginSpot, "Bybit", ioc, std::move(creds)) {
    }

    bool connect() override;
    void disconnect() override;

    void subscribe_ticker(const std::vector<SymbolId>& symbols) override;
    void subscribe_orderbook(const std::vector<SymbolId>& symbols) override;

    std::vector<SymbolId> get_available_symbols() override;
    std::vector<Ticker> fetch_all_tickers() override;

    Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) override;
    bool cancel_order(uint64_t order_id) override;

    bool prepare_shorting(const SymbolId& symbol) override;
    std::vector<Position> get_short_positions() override;
    bool close_short_position(const SymbolId& symbol) override;
    Order open_short(const SymbolId& symbol, Quantity quantity) override;
    Order close_short(const SymbolId& symbol, Quantity quantity) override;

    double get_balance(const std::string& currency) override;

    // Public fill query for async/parallel execution from OrderManager
    bool query_order_fill(const std::string& order_id, Order& order);

    // Fetch deposit-enabled networks per coin.
    // Returns {coin → {normalized_network_set}}.
    // Calls GET /v5/asset/coin/query-info (requires auth).
    std::unordered_map<std::string, std::unordered_set<std::string>> fetch_deposit_networks();

protected:
    void on_ws_message(std::string_view message) override;
    void on_ws_connected() override;
    void on_ws_disconnected() override;

private:
    std::string generate_signature(int64_t timestamp, const std::string& params) const;
    std::string resolve_public_ws_endpoint() const;

    std::unordered_map<std::string, std::string> build_auth_headers(
        const std::string& params = "") const;

    bool ensure_spot_margin_mode();
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
