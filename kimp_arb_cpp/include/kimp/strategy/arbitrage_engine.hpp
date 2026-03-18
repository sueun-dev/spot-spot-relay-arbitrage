#pragma once

#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/memory/atomic_bitset.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace kimp::strategy {

/**
 * Thread-safe sharded price cache.
 *
 * Price data is partitioned into lock shards to reduce contention under high
 * ticker throughput. Readers/writers touching different symbols run in
 * parallel, while preserving correctness per symbol key.
 */
class PriceCache {
public:
    struct NetworkFee {
        std::string network;   // normalized: "BTC", "ERC20", "TRC20", etc.
        double fee_coins{0.0};
    };

    struct PriceData {
        double bid{0.0};
        double ask{0.0};
        double bid_qty{0.0};
        double ask_qty{0.0};
        double last{0.0};
        uint64_t timestamp{0};
        bool valid{false};
    };

private:
    struct PriceEntry {
        std::atomic<double> bid{0.0};
        std::atomic<double> ask{0.0};
        std::atomic<double> bid_qty{0.0};
        std::atomic<double> ask_qty{0.0};
        std::atomic<double> last{0.0};
        std::atomic<uint64_t> timestamp{0};
    };

    // Zero-allocation composite key (exchange + symbol)
    struct PriceKey {
        uint8_t exchange;
        SymbolId symbol;

        bool operator==(const PriceKey& other) const noexcept {
            return exchange == other.exchange && symbol == other.symbol;
        }
    };

    struct PriceKeyHash {
        size_t operator()(const PriceKey& k) const noexcept {
            // Mix exchange into symbol hash (golden ratio hash for exchange bits)
            return (static_cast<size_t>(k.exchange) * 2654435761u) ^ k.symbol.hash();
        }
    };

    static PriceKey make_key(Exchange ex, const SymbolId& symbol) noexcept {
        return {static_cast<uint8_t>(ex), symbol};
    }

    static constexpr size_t SHARD_COUNT = 64;  // Power-of-two for fast masking
    static_assert((SHARD_COUNT & (SHARD_COUNT - 1)) == 0, "SHARD_COUNT must be power-of-two");

    struct alignas(memory::CACHE_LINE_SIZE) PriceShard {
        mutable std::shared_mutex mutex;
        std::unordered_map<PriceKey, PriceEntry, PriceKeyHash> prices;
    };

    std::array<PriceShard, SHARD_COUNT> shards_;

    static size_t shard_index(const PriceKey& key) noexcept {
        return PriceKeyHash{}(key) & (SHARD_COUNT - 1);
    }

    PriceShard& shard_for(const PriceKey& key) noexcept {
        return shards_[shard_index(key)];
    }

    const PriceShard& shard_for(const PriceKey& key) const noexcept {
        return shards_[shard_index(key)];
    }

    // USDT/KRW price from Korean venues.
    std::atomic<double> usdt_krw_bithumb_{0.0};
    std::atomic<double> usdt_krw_upbit_{0.0};

    // ── Network-aware withdrawal fee storage ──
    // Korean exchange: per-coin, per-network withdrawal fees in coin units.
    // Foreign exchange: per-coin, deposit-enabled network set (normalized).
    // get_withdraw_fee(korean, foreign, coin) intersects and picks minimum.
    mutable std::shared_mutex withdraw_fee_mutex_;
    // withdraw_network_fees_[Bithumb]["BTC"] = [{"BTC", 0.0002}]
    std::unordered_map<Exchange, std::unordered_map<std::string, std::vector<NetworkFee>>> withdraw_network_fees_;
    // foreign_deposit_nets_[Bybit]["BTC"] = {"BTC"}
    std::unordered_map<Exchange, std::unordered_map<std::string, std::unordered_set<std::string>>> foreign_deposit_nets_;

    // Precomputed flat cache: (korean_ex, foreign_ex, coin) → fee.
    // Populated by finalize_withdraw_fees(), read lock-free after that.
    std::unordered_map<std::string, double> precomputed_fees_;

    static std::string make_fee_key(Exchange k, Exchange f, const std::string& coin) {
        // "K:F:COIN" — short, unique, no allocation for small coins
        std::string key;
        key.reserve(coin.size() + 4);
        key += static_cast<char>('0' + static_cast<int>(k));
        key += ':';
        key += static_cast<char>('0' + static_cast<int>(f));
        key += ':';
        key += coin;
        return key;
    }

    // Normalize chain name: strip non-alnum, uppercase, then map known aliases
    // to a canonical form so cross-exchange intersection works.
    static std::string normalize_chain(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        }
        // Canonical alias table: different exchanges use different names
        // for the same blockchain network. Map them to one canonical form.
        static const std::unordered_map<std::string, std::string> aliases = {
            {"BITCOIN",         "BTC"},
            {"ETHEREUM",        "ETH"},
            {"POLYGON",         "MATIC"},
            {"POL",             "MATIC"},
            {"SOLANA",          "SOL"},
            {"TRON",            "TRX"},
            {"RIPPLE",          "XRP"},
            {"LITECOIN",        "LTC"},
            {"DOGECOIN",        "DOGE"},
            {"AVALANCHE",       "AVAXC"},
            {"AVALANCHECCHAIN", "AVAXC"},
            {"STELLAR",         "XLM"},
            {"COSMOS",          "ATOM"},
            {"POLKADOT",        "DOT"},
            {"CARDANO",         "ADA"},
            {"ALGORAND",        "ALGO"},
            {"NEAR",            "NEAR"},
            {"ARBITRUMONE",     "ARBONE"},
            {"BSC",             "BEP20"},
            {"BNBSMARTCHAIN",   "BEP20"},
            {"OPTIMISM",        "OP"},
        };
        auto it = aliases.find(out);
        if (it != aliases.end()) {
            return it->second;
        }
        return out;
    }

public:
    void update(Exchange ex, const SymbolId& symbol, double bid, double ask, double last,
                uint64_t timestamp_ms = 0, double bid_qty = 0.0, double ask_qty = 0.0) {
        PriceKey key = make_key(ex, symbol);
        auto& shard = shard_for(key);
        const uint64_t ts = (timestamp_ms != 0)
            ? timestamp_ms
            : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        {
            std::shared_lock read_lock(shard.mutex);
            auto it = shard.prices.find(key);
            if (it != shard.prices.end()) {
                // Fast path: entry exists, just update atomics
                it->second.bid.store(bid, std::memory_order_relaxed);
                it->second.ask.store(ask, std::memory_order_relaxed);
                if (bid_qty > 0.0) {
                    it->second.bid_qty.store(bid_qty, std::memory_order_relaxed);
                }
                if (ask_qty > 0.0) {
                    it->second.ask_qty.store(ask_qty, std::memory_order_relaxed);
                }
                it->second.last.store(last, std::memory_order_relaxed);
                it->second.timestamp.store(ts, std::memory_order_release);
                return;
            }
        }

        // Slow path: need to create entry (only at startup)
        std::unique_lock write_lock(shard.mutex);
        auto& entry = shard.prices[key];
        entry.bid.store(bid, std::memory_order_relaxed);
        entry.ask.store(ask, std::memory_order_relaxed);
        entry.bid_qty.store(bid_qty, std::memory_order_relaxed);
        entry.ask_qty.store(ask_qty, std::memory_order_relaxed);
        entry.last.store(last, std::memory_order_relaxed);
        entry.timestamp.store(ts, std::memory_order_release);
    }

    void update_usdt_krw(Exchange ex, double price) {
        if (ex == Exchange::Bithumb) {
            usdt_krw_bithumb_.store(price, std::memory_order_release);
        } else if (ex == Exchange::Upbit) {
            usdt_krw_upbit_.store(price, std::memory_order_release);
        }
    }

    PriceData get_price(Exchange ex, const SymbolId& symbol) const {
        PriceKey key = make_key(ex, symbol);
        const auto& shard = shard_for(key);

        std::shared_lock lock(shard.mutex);
        auto it = shard.prices.find(key);
        if (it == shard.prices.end()) {
            return {};  // Not found
        }

        const auto& entry = it->second;
        // Load timestamp with acquire first — pairs with release store in update(),
        // guaranteeing all preceding relaxed stores are visible on ARM64.
        auto ts = entry.timestamp.load(std::memory_order_acquire);
        return {
            entry.bid.load(std::memory_order_relaxed),
            entry.ask.load(std::memory_order_relaxed),
            entry.bid_qty.load(std::memory_order_relaxed),
            entry.ask_qty.load(std::memory_order_relaxed),
            entry.last.load(std::memory_order_relaxed),
            ts,
            true  // valid
        };
    }

    double get_usdt_krw(Exchange ex) const {
        if (ex == Exchange::Bithumb) {
            return usdt_krw_bithumb_.load(std::memory_order_acquire);
        } else if (ex == Exchange::Upbit) {
            return usdt_krw_upbit_.load(std::memory_order_acquire);
        }
        return 0.0;
    }

    // Store per-network withdrawal fees for a Korean exchange.
    // Store per-network withdrawal fees. Network names are re-normalized
    // through the canonical alias table on ingestion so that cross-exchange
    // intersection works regardless of the source exchange's naming convention.
    void set_withdraw_network_fees(Exchange ex, const std::string& base,
                                    std::vector<NetworkFee> fees) {
        for (auto& nf : fees) nf.network = normalize_chain(nf.network);
        std::unique_lock lock(withdraw_fee_mutex_);
        withdraw_network_fees_[ex][base] = std::move(fees);
    }

    // Store deposit-enabled networks. Same canonical normalization applied.
    void set_foreign_deposit_networks(Exchange ex, const std::string& base,
                                       std::unordered_set<std::string> nets) {
        std::unordered_set<std::string> canonical;
        for (auto& n : nets) canonical.insert(normalize_chain(n));
        std::unique_lock lock(withdraw_fee_mutex_);
        foreign_deposit_nets_[ex][base] = std::move(canonical);
    }

    // Hot-path lookup: lock-free read from precomputed flat cache.
    // Call finalize_withdraw_fees() after all data is loaded.
    double get_withdraw_fee(Exchange korean_ex, Exchange foreign_ex,
                            const std::string& base) const {
        // Fast path: precomputed cache (lock-free after finalize)
        auto key = make_fee_key(korean_ex, foreign_ex, base);
        auto it = precomputed_fees_.find(key);
        if (it != precomputed_fees_.end()) return it->second;
        return 0.0;
    }

    // Call once after all set_withdraw_network_fees / set_foreign_deposit_networks
    // calls are done. Precomputes the fee for every (korean_ex, foreign_ex, coin)
    // combination into a flat lock-free map for zero-overhead hot-path reads.
    void finalize_withdraw_fees() {
        std::shared_lock lock(withdraw_fee_mutex_);
        precomputed_fees_.clear();

        // Collect all coin names
        std::unordered_set<std::string> all_coins;
        for (const auto& [ex, coin_map] : withdraw_network_fees_) {
            for (const auto& [coin, _] : coin_map) all_coins.insert(coin);
        }

        // Collect all exchange pairs
        std::vector<Exchange> korean_exs, foreign_exs;
        for (const auto& [ex, _] : withdraw_network_fees_) korean_exs.push_back(ex);
        for (const auto& [ex, _] : foreign_deposit_nets_) foreign_exs.push_back(ex);
        // Also add exchanges that might not have deposit data yet
        if (foreign_exs.empty()) {
            foreign_exs = {Exchange::Bybit, Exchange::OKX};
        }

        for (const auto& coin : all_coins) {
            for (auto k_ex : korean_exs) {
                for (auto f_ex : foreign_exs) {
                    double fee = get_withdraw_fee_locked(k_ex, f_ex, coin);
                    if (fee > 0.0) {
                        precomputed_fees_[make_fee_key(k_ex, f_ex, coin)] = fee;
                    }
                }
            }
        }

    }

    size_t precomputed_fee_count() const { return precomputed_fees_.size(); }

    size_t withdraw_fee_count() const {
        std::shared_lock lock(withdraw_fee_mutex_);
        size_t total = 0;
        for (const auto& [_, m] : withdraw_network_fees_) total += m.size();
        return total;
    }

    size_t withdraw_fee_count(Exchange ex) const {
        std::shared_lock lock(withdraw_fee_mutex_);
        auto it = withdraw_network_fees_.find(ex);
        return (it != withdraw_network_fees_.end()) ? it->second.size() : 0;
    }

private:
    // Must be called with withdraw_fee_mutex_ held (shared or exclusive).
    double get_withdraw_fee_locked(Exchange korean_ex, Exchange foreign_ex,
                                   const std::string& base) const {
        // Get foreign deposit networks for this coin
        const std::unordered_set<std::string>* deposit_nets = nullptr;
        auto fd_it = foreign_deposit_nets_.find(foreign_ex);
        if (fd_it != foreign_deposit_nets_.end()) {
            auto coin_it = fd_it->second.find(base);
            if (coin_it != fd_it->second.end() && !coin_it->second.empty()) {
                deposit_nets = &coin_it->second;
            }
        }

        // Try requested Korean exchange first, then others
        auto try_korean = [&](Exchange ex) -> double {
            auto wf_it = withdraw_network_fees_.find(ex);
            if (wf_it == withdraw_network_fees_.end()) return -1.0;
            auto coin_it = wf_it->second.find(base);
            if (coin_it == wf_it->second.end() || coin_it->second.empty()) return -1.0;

            double min_fee = std::numeric_limits<double>::max();
            for (const auto& nf : coin_it->second) {
                if (deposit_nets) {
                    // Only consider networks the foreign exchange can receive
                    if (deposit_nets->count(nf.network) && nf.fee_coins < min_fee) {
                        min_fee = nf.fee_coins;
                    }
                } else {
                    // No foreign deposit data → fall back to raw minimum
                    if (nf.fee_coins < min_fee) min_fee = nf.fee_coins;
                }
            }
            return (min_fee < std::numeric_limits<double>::max()) ? min_fee : -1.0;
        };

        double fee = try_korean(korean_ex);
        if (fee >= 0.0) return fee;

        // Fallback: try other Korean exchanges
        for (const auto& [other_ex, _] : withdraw_network_fees_) {
            if (other_ex == korean_ex) continue;
            fee = try_korean(other_ex);
            if (fee >= 0.0) return fee;
        }

        return 0.0;
    }

public:

    // Debug: get all stored symbols
    std::vector<std::string> get_all_keys() const {
        std::vector<std::string> keys;
        for (const auto& shard : shards_) {
            std::shared_lock lock(shard.mutex);
            keys.reserve(keys.size() + shard.prices.size());
            for (const auto& [key, _] : shard.prices) {
                keys.push_back(std::to_string(key.exchange) + ":" +
                               std::string(key.symbol.get_base()) + "/" +
                               std::string(key.symbol.get_quote()));
            }
        }
        return keys;
    }

    size_t size() const {
        size_t total = 0;
        for (const auto& shard : shards_) {
            std::shared_lock lock(shard.mutex);
            total += shard.prices.size();
        }
        return total;
    }
};

/**
 * Position tracker using atomic operations
 */
class PositionTracker {
private:
    static constexpr size_t MAX_POSITIONS = 16;

    struct alignas(memory::CACHE_LINE_SIZE) PositionSlot {
        std::atomic<bool> active{false};
        std::atomic<uint64_t> symbol_hash{0};
        Position position;
        mutable std::mutex mutex;  // For position data access
    };

    std::array<PositionSlot, MAX_POSITIONS> positions_{};
    std::atomic<int> position_count_{0};

public:
    bool can_open_position() const noexcept {
        return position_count_.load(std::memory_order_acquire) <
               static_cast<int>(TradingConfig::MAX_POSITIONS);
    }

    bool has_position(const SymbolId& symbol) const {
        uint64_t hash = symbol.hash();
        for (const auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire) &&
                slot.symbol_hash.load(std::memory_order_acquire) == hash) {
                std::lock_guard lock(slot.mutex);
                if (slot.active.load(std::memory_order_relaxed) &&
                    slot.position.symbol == symbol) {
                    return true;
                }
            }
        }
        return false;
    }

    bool has_any_position() const noexcept {
        return position_count_.load(std::memory_order_acquire) > 0;
    }

    std::optional<Position> get_position(const SymbolId& symbol) const {
        uint64_t hash = symbol.hash();
        for (const auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire) &&
                slot.symbol_hash.load(std::memory_order_acquire) == hash) {
                std::lock_guard lock(slot.mutex);
                if (slot.active.load(std::memory_order_relaxed) &&
                    slot.position.symbol == symbol) {
                    return slot.position;
                }
            }
        }
        return std::nullopt;
    }

    bool open_position(const Position& pos) {
        for (auto& slot : positions_) {
            bool expected = false;
            if (slot.active.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                std::lock_guard lock(slot.mutex);
                slot.symbol_hash.store(pos.symbol.hash(), std::memory_order_release);
                slot.position = pos;
                position_count_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    bool close_position(const SymbolId& symbol, Position& closed) {
        uint64_t hash = symbol.hash();
        for (auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire) &&
                slot.symbol_hash.load(std::memory_order_acquire) == hash) {
                std::lock_guard lock(slot.mutex);
                if (!slot.active.load(std::memory_order_relaxed) ||
                    slot.position.symbol != symbol) {
                    continue;
                }
                closed = slot.position;
                slot.active.store(false, std::memory_order_release);
                position_count_.fetch_sub(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    int get_position_count() const noexcept {
        return position_count_.load(std::memory_order_acquire);
    }

    std::vector<Position> get_active_positions() const {
        std::vector<Position> result;
        for (const auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire)) {
                std::lock_guard lock(slot.mutex);
                result.push_back(slot.position);
            }
        }
        return result;
    }

    template <typename Fn>
    void for_each_active_position(Fn&& fn) const {
        for (const auto& slot : positions_) {
            if (!slot.active.load(std::memory_order_acquire)) {
                continue;
            }
            std::lock_guard lock(slot.mutex);
            if (slot.active.load(std::memory_order_relaxed)) {
                fn(slot.position);
            }
        }
    }
};

/**
 * Capital tracker for compound growth
 * Tracks total capital and calculates dynamic position sizes
 */
class CapitalTracker {
private:
    std::atomic<double> initial_capital_{TradingConfig::TOTAL_CAPITAL_USD};  // Starting capital in USD (both venues combined)
    std::atomic<double> realized_pnl_usd_{0.0};       // Accumulated P&L in USD
    std::atomic<int> total_trades_{0};                 // Total trades closed
    std::atomic<int> winning_trades_{0};               // Winning trades count
    mutable std::mutex history_mutex_;
    std::vector<double> pnl_history_;                  // P&L per trade for stats

public:
    CapitalTracker() = default;
    explicit CapitalTracker(double initial_capital)
        : initial_capital_(initial_capital), realized_pnl_usd_(0.0) {}

    void set_initial_capital(double capital) {
        initial_capital_.store(capital, std::memory_order_release);
    }

    double get_initial_capital() const {
        return initial_capital_.load(std::memory_order_acquire);
    }

    double get_current_capital() const {
        return initial_capital_.load(std::memory_order_acquire) +
               realized_pnl_usd_.load(std::memory_order_acquire);
    }

    double get_realized_pnl() const {
        return realized_pnl_usd_.load(std::memory_order_acquire);
    }

    // Add P&L from closed position (in USD)
    void add_realized_pnl(double pnl_usd) {
        // Update atomic P&L
        double current = realized_pnl_usd_.load(std::memory_order_relaxed);
        while (!realized_pnl_usd_.compare_exchange_weak(
            current, current + pnl_usd, std::memory_order_release, std::memory_order_relaxed));

        // Update trade stats
        total_trades_.fetch_add(1, std::memory_order_relaxed);
        if (pnl_usd > 0) {
            winning_trades_.fetch_add(1, std::memory_order_relaxed);
        }

        // Store in history for stats
        {
            std::lock_guard lock(history_mutex_);
            pnl_history_.push_back(pnl_usd);
        }
    }

    // Dynamic position size: capped by POSITION_SIZE_USD (per side)
    double get_position_size_usd() const {
        double current = get_current_capital();
        // Position size = min(capital/max_positions/2, POSITION_SIZE_USD)
        // Example: $6000 / 1 / 2 = $3000 per side budget, capped to the configured side limit.
        double dynamic = current / static_cast<double>(TradingConfig::MAX_POSITIONS) / 2.0;
        return std::min(dynamic, TradingConfig::POSITION_SIZE_USD);
    }

    // Get total order value (both sides)
    double get_total_position_value() const {
        return get_position_size_usd() * 2.0;
    }

    // Statistics
    int get_total_trades() const {
        return total_trades_.load(std::memory_order_acquire);
    }

    int get_winning_trades() const {
        return winning_trades_.load(std::memory_order_acquire);
    }

    double get_win_rate() const {
        int total = get_total_trades();
        if (total == 0) return 0.0;
        return static_cast<double>(get_winning_trades()) / total * 100.0;
    }

    double get_return_percent() const {
        double initial = get_initial_capital();
        if (initial <= 0) return 0.0;
        return (get_realized_pnl() / initial) * 100.0;
    }

    // Get P&L history for analysis
    std::vector<double> get_pnl_history() const {
        std::lock_guard lock(history_mutex_);
        return pnl_history_;
    }

    // Reset for new session (keeps initial capital)
    void reset_session() {
        realized_pnl_usd_.store(0.0, std::memory_order_release);
        total_trades_.store(0, std::memory_order_release);
        winning_trades_.store(0, std::memory_order_release);
        {
            std::lock_guard lock(history_mutex_);
            pnl_history_.clear();
        }
    }
};

/**
 * Premium calculation utilities
 */
class PremiumCalculator {
public:
    struct RelayMetrics {
        double match_qty{0.0};
        double bithumb_top_krw{0.0};
        double bithumb_top_usdt{0.0};
        double bybit_top_usdt{0.0};
        double bybit_top_krw{0.0};
        double match_buy_krw{0.0};
        double match_sell_usdt{0.0};
        double match_sell_krw{0.0};
        double max_tradable_usdt_at_best{0.0};
        double target_coin_qty{0.0};
        bool bithumb_can_fill_target{false};
        bool bybit_can_fill_target{false};
        bool both_can_fill_target{false};
        double bithumb_total_fee_krw{0.0};
        double bybit_total_fee_usdt{0.0};
        double bybit_total_fee_krw{0.0};
        double withdraw_fee_coins{0.0};   // Korean exchange withdrawal fee in coin units
        double withdraw_fee_krw{0.0};     // Network transfer fee (Korean → Foreign) in KRW
        double total_fee_krw{0.0};
        double gross_spread_krw{0.0};
        double gross_edge_pct{0.0};
        double net_profit_krw{0.0}; // Uses the immediately executable entry size, capped at TARGET_ENTRY_USDT
        double net_basis_krw{0.0};
        double net_edge_pct{0.0};
    };

    static double calculate_entry_premium(double korean_ask,
                                           double foreign_bid,
                                           double usdt_krw_rate) {
        if (foreign_bid <= 0 || usdt_krw_rate <= 0) return 0.0;
        double foreign_krw = foreign_bid * usdt_krw_rate;
        return ((korean_ask - foreign_krw) / foreign_krw) * 100.0;
    }

    static double calculate_exit_premium(double korean_bid,
                                          double foreign_ask,
                                          double usdt_krw_rate) {
        if (foreign_ask <= 0 || usdt_krw_rate <= 0) return 0.0;
        double foreign_krw = foreign_ask * usdt_krw_rate;
        return ((korean_bid - foreign_krw) / foreign_krw) * 100.0;
    }

    static bool should_enter(double premium) {
        return premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD;
    }

    static bool should_exit(double premium) {
        return premium >= TradingConfig::EXIT_PREMIUM_THRESHOLD;
    }

    static RelayMetrics calculate_relay_metrics(double korean_ask,
                                                double korean_ask_qty,
                                                double foreign_bid,
                                                double foreign_bid_qty,
                                                double usdt_krw_rate,
                                                double korean_fee_rate = TradingConfig::BITHUMB_FEE_RATE,
                                                double foreign_fee_rate = TradingConfig::BYBIT_FEE_RATE,
                                                double withdraw_fee_coins = 0.0) {
        RelayMetrics metrics;
        if (korean_ask <= 0.0 || korean_ask_qty <= 0.0 ||
            foreign_bid <= 0.0 || foreign_bid_qty <= 0.0 ||
            usdt_krw_rate <= 0.0) {
            return metrics;
        }

        metrics.match_qty = std::min(korean_ask_qty, foreign_bid_qty);
        metrics.bithumb_top_krw = korean_ask * korean_ask_qty;
        metrics.bithumb_top_usdt = metrics.bithumb_top_krw / usdt_krw_rate;
        metrics.bybit_top_usdt = foreign_bid * foreign_bid_qty;
        metrics.bybit_top_krw = metrics.bybit_top_usdt * usdt_krw_rate;
        metrics.max_tradable_usdt_at_best = std::min(metrics.bithumb_top_usdt, metrics.bybit_top_usdt);
        metrics.target_coin_qty = TradingConfig::TARGET_ENTRY_USDT / foreign_bid;
        metrics.bithumb_can_fill_target = metrics.bithumb_top_usdt >= TradingConfig::TARGET_ENTRY_USDT;
        metrics.bybit_can_fill_target = metrics.bybit_top_usdt >= TradingConfig::TARGET_ENTRY_USDT;
        metrics.both_can_fill_target = metrics.bithumb_can_fill_target && metrics.bybit_can_fill_target;

        // Price the entry on the immediately executable size, capped at the live target notional.
        const double effective_entry_qty = std::min(metrics.match_qty, metrics.target_coin_qty);
        metrics.match_buy_krw = korean_ask * effective_entry_qty;
        metrics.match_sell_usdt = foreign_bid * effective_entry_qty;
        metrics.match_sell_krw = metrics.match_sell_usdt * usdt_krw_rate;

        const double korean_fee_per_trade_krw = metrics.match_buy_krw * korean_fee_rate;
        const double foreign_fee_per_trade_usdt = metrics.match_sell_usdt * foreign_fee_rate;
        const double foreign_fee_per_trade_krw = metrics.match_sell_krw * foreign_fee_rate;
        metrics.bithumb_total_fee_krw = korean_fee_per_trade_krw * TradingConfig::KOREAN_FEE_EVENTS;
        metrics.bybit_total_fee_usdt = foreign_fee_per_trade_usdt * TradingConfig::FOREIGN_FEE_EVENTS;
        metrics.bybit_total_fee_krw = foreign_fee_per_trade_krw * TradingConfig::FOREIGN_FEE_EVENTS;
        metrics.withdraw_fee_coins = withdraw_fee_coins;
        metrics.withdraw_fee_krw = withdraw_fee_coins * korean_ask;  // Fee in coins × KRW price
        metrics.total_fee_krw = metrics.bithumb_total_fee_krw + metrics.bybit_total_fee_krw + metrics.withdraw_fee_krw;

        metrics.gross_spread_krw = metrics.match_sell_krw - metrics.match_buy_krw;
        if (metrics.match_buy_krw > 0.0) {
            metrics.gross_edge_pct = (metrics.gross_spread_krw / metrics.match_buy_krw) * 100.0;
        }
        metrics.net_profit_krw = metrics.gross_spread_krw - metrics.total_fee_krw;
        metrics.net_basis_krw = metrics.match_buy_krw + metrics.total_fee_krw;
        if (metrics.net_basis_krw > 0.0) {
            metrics.net_edge_pct = (metrics.net_profit_krw / metrics.net_basis_krw) * 100.0;
        }

        return metrics;
    }
};

/**
 * Main arbitrage engine
 */
class ArbitrageEngine {
public:
    using ExchangePtr = std::shared_ptr<exchange::ExchangeBase>;

    struct PremiumInfo {
        SymbolId symbol;
        double korean_bid{0.0};
        double korean_ask{0.0};
        double korean_bid_qty{0.0};
        double korean_ask_qty{0.0};
        double foreign_bid{0.0};
        double foreign_ask{0.0};
        double foreign_bid_qty{0.0};
        double foreign_ask_qty{0.0};
        double korean_price{0.0};
        double foreign_price{0.0};
        double usdt_rate{0.0};
        double entry_premium{0.0};
        double exit_premium{0.0};
        double premium_spread{0.0};   // entry - exit
        double premium{0.0};
        double match_qty{0.0};
        double target_coin_qty{0.0};
        double max_tradable_usdt_at_best{0.0};
        double bithumb_top_krw{0.0};
        double bithumb_top_usdt{0.0};
        double bybit_top_usdt{0.0};
        double bybit_top_krw{0.0};
        double gross_edge_pct{0.0};
        double net_edge_pct{0.0};
        double bithumb_total_fee_krw{0.0};
        double bybit_total_fee_usdt{0.0};
        double bybit_total_fee_krw{0.0};
        double withdraw_fee_coins{0.0};
        double withdraw_fee_krw{0.0};
        double total_fee_krw{0.0};
        double net_profit_krw{0.0};
        bool both_can_fill_target{false};
        uint64_t age_ms{0};
        bool entry_signal{false};
        bool exit_signal{false};
        Exchange best_korean_exchange{Exchange::Bithumb};
        Exchange best_foreign_exchange{Exchange::Bybit};
    };

    ArbitrageEngine() = default;
    ~ArbitrageEngine();

    // Configuration
    void set_exchange(Exchange ex, ExchangePtr exchange);
    void add_symbol(const SymbolId& symbol);
    void add_exchange_pair(Exchange korean, Exchange foreign);
    void set_exchange_pair_entry_enabled(Exchange korean, Exchange foreign, bool enabled);

    // Lifecycle
    void start();
    void stop();

    // Data updates
    void on_ticker_update(const Ticker& ticker);
    void on_usdt_update(Exchange ex, double price);

    // Position management
    void open_position(const Position& pos) { position_tracker_.open_position(pos); }
    bool close_position(const SymbolId& symbol, Position& closed);
    int get_position_count() const { return position_tracker_.get_position_count(); }
    std::optional<Position> get_position(const SymbolId& symbol) const {
        return position_tracker_.get_position(symbol);
    }
    bool update_position(const Position& pos) {
        Position closed;
        if (position_tracker_.close_position(pos.symbol, closed)) {
            return position_tracker_.open_position(pos);
        }
        return false;
    }
    PositionTracker& get_position_tracker() { return position_tracker_; }

    // Entry suppression: when lifecycle loop is active, skip fire_entry_from_cache()
    void set_entry_suppressed(bool suppressed) { entry_suppressed_.store(suppressed, std::memory_order_release); }
    bool is_entry_suppressed() const { return entry_suppressed_.load(std::memory_order_acquire); }

    // Capital management (복리 성장)
    void set_initial_capital(double capital_usd) { capital_tracker_.set_initial_capital(capital_usd); }
    double get_current_capital() const { return capital_tracker_.get_current_capital(); }
    double get_position_size_usd() const { return capital_tracker_.get_position_size_usd(); }
    void add_realized_pnl(double pnl_usd) { capital_tracker_.add_realized_pnl(pnl_usd); }
    const CapitalTracker& get_capital_tracker() const { return capital_tracker_; }

    // Signals
    std::optional<ArbitrageSignal> get_entry_signal();
    std::optional<ExitSignal> get_exit_signal();

    // Analysis
    double calculate_premium(const SymbolId& symbol, Exchange korean_ex, Exchange foreign_ex) const;
    std::vector<PremiumInfo> get_all_premiums() const;
    const PriceCache& get_price_cache() const { return price_cache_; }
    PriceCache& get_price_cache() { return price_cache_; }

    // Market data update signaling (for event-driven waits)
    uint64_t get_update_seq() const { return update_seq_.load(std::memory_order_acquire); }
    void wait_for_update(uint64_t last_seq, std::chrono::milliseconds timeout) const;

    // Callbacks (병렬 포지션 - 각 코인별 진입/청산)
    using EntryCallback = std::function<void(const ArbitrageSignal&)>;
    using ExitCallback = std::function<void(const ExitSignal&)>;
    void set_entry_callback(EntryCallback cb) { on_entry_signal_ = std::move(cb); }
    void set_exit_callback(ExitCallback cb) { on_exit_signal_ = std::move(cb); }

    // JSON Export
    void export_to_json(const std::string& path) const;
    void export_to_json_async(const std::string& path);
    void start_async_exporter(const std::string& path, std::chrono::milliseconds interval);
    void stop_async_exporter();

private:
    // Exchanges
    std::array<ExchangePtr, static_cast<size_t>(Exchange::Count)> exchanges_{};

    // Symbol tracking with O(1) lookup (SymbolId key — collision-safe)
    struct SymbolIdHash {
        size_t operator()(const SymbolId& s) const noexcept { return s.hash(); }
    };

    std::vector<SymbolId> monitored_symbols_;
    std::vector<SymbolId> foreign_symbols_;  // Pre-computed foreign symbols
    std::unordered_map<SymbolId, size_t, SymbolIdHash> korean_symbol_index_;
    std::unordered_map<SymbolId, size_t, SymbolIdHash> foreign_symbol_index_;

    struct ExchangePairConfig {
        Exchange korean{Exchange::Bithumb};
        Exchange foreign{Exchange::Bybit};
        bool entry_enabled{true};
    };

    std::vector<ExchangePairConfig> exchange_pairs_;

    // Price and position tracking
    PriceCache price_cache_;
    PositionTracker position_tracker_;
    CapitalTracker capital_tracker_{TradingConfig::TOTAL_CAPITAL_USD};  // 초기 자본: 거래소별 $3000, 총 $6000

    // ── Incremental entry premium cache (lock-free, O(1) per-symbol update) ──
    // Every ticker update recomputes only the affected symbol's premium.
    // Entry detection scans this cache-hot array instead of doing PriceCache
    // lookups + mutex + hash per symbol.  Zero-miss, zero-delay.
    // Keep this comfortably above live common-symbol counts to avoid cache index overflow.
    static constexpr size_t MAX_CACHED_SYMBOLS = 1024;
    struct alignas(memory::CACHE_LINE_SIZE) CachedEntryPremium {
        std::atomic<double> entry_premium{100.0};  // High default = no signal
        std::atomic<double> korean_ask{0.0};
        std::atomic<double> korean_ask_qty{0.0};
        std::atomic<double> foreign_bid{0.0};
        std::atomic<double> foreign_bid_qty{0.0};
        std::atomic<double> match_qty{0.0};
        std::atomic<double> target_coin_qty{0.0};
        std::atomic<double> max_tradable_usdt_at_best{0.0};
        std::atomic<double> gross_edge_pct{0.0};
        std::atomic<double> net_edge_pct{0.0};
        std::atomic<double> net_profit_krw{0.0};
        std::atomic<double> usdt_rate{0.0};
        std::atomic<bool> both_can_fill_target{false};
        std::atomic<uint8_t> best_korean_exchange{0};  // Exchange enum of best Korean venue
        std::atomic<uint8_t> best_foreign_exchange{0}; // Exchange enum of best foreign venue
        std::atomic<bool> qualified{false};         // All entry filters passed
        std::atomic<bool> signal_fired{false};      // Dedup: reset when disqualified
    };
    std::array<CachedEntryPremium, MAX_CACHED_SYMBOLS> entry_cache_{};
    memory::AtomicBitset<MAX_CACHED_SYMBOLS> entry_candidate_bits_;
    memory::AtomicBitset<MAX_CACHED_SYMBOLS> entry_signal_fired_bits_;

    // Signal queues (lock-free, multi-producer safe)
    memory::MPMCRingBuffer<ArbitrageSignal, 256> entry_signals_;
    memory::MPMCRingBuffer<ExitSignal, 256> exit_signals_;

    // Callbacks
    EntryCallback on_entry_signal_;
    ExitCallback on_exit_signal_;

    // Thread control
    std::atomic<bool> running_{false};
    std::atomic<bool> entry_suppressed_{false};
    std::thread monitor_thread_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    // Async exporter
    std::atomic<bool> exporter_running_{false};
    std::thread exporter_thread_;
    std::string export_path_;
    std::chrono::milliseconds export_interval_{1000};
    std::mutex exporter_mutex_;
    std::condition_variable exporter_cv_;

    // Update notification for order execution waits
    mutable std::mutex update_mutex_;
    mutable std::condition_variable update_cv_;
    std::atomic<uint64_t> update_seq_{0};
    std::array<std::atomic<double>, static_cast<size_t>(Exchange::Count)> last_usdt_log_{};
    std::atomic<uint64_t> next_entry_scan_ms_{0};     // Throttle O(N) entry cache scans
    std::atomic<uint64_t> next_usdt_scan_ms_{0};      // Debounce bursty USDT-triggered full scans

    // Internal methods
    void monitor_loop();
    void check_exit_conditions();      // 각 포지션 개별 청산 체크

    // Incremental entry system
    void update_symbol_entry(size_t idx);          // O(1) per-symbol premium recompute
    void update_all_entries();                      // O(N) on USDT change (infrequent)
    void fire_entry_from_cache();                   // O(N) lightweight scan, fires signals
    void check_symbol_exit(size_t idx);             // O(1) per-symbol exit check
    void sync_entry_state_bits(size_t idx, bool qualifies, bool signal_fired);

    static bool is_korean_exchange(Exchange ex) {
        return ex == Exchange::Bithumb || ex == Exchange::Upbit;
    }
};

} // namespace kimp::strategy
