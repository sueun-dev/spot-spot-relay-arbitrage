#pragma once

#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
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
    struct PriceData {
        double bid{0.0};
        double ask{0.0};
        double last{0.0};
        double funding_rate{0.0};
        uint64_t next_funding_time{0};
        uint64_t timestamp{0};
        int funding_interval_hours{8};
        bool valid{false};
    };

private:
    struct PriceEntry {
        std::atomic<double> bid{0.0};
        std::atomic<double> ask{0.0};
        std::atomic<double> last{0.0};
        std::atomic<double> funding_rate{0.0};
        std::atomic<int> funding_interval_hours{8};
        std::atomic<uint64_t> next_funding_time{0};
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

    struct alignas(64) PriceShard {
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

    // USDT/KRW prices
    std::atomic<double> usdt_krw_upbit_{0.0};
    std::atomic<double> usdt_krw_bithumb_{0.0};

public:
    void update(Exchange ex, const SymbolId& symbol, double bid, double ask, double last,
                uint64_t timestamp_ms = 0) {
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
        entry.last.store(last, std::memory_order_relaxed);
        entry.timestamp.store(ts, std::memory_order_release);
    }

    void update_funding(Exchange ex, const SymbolId& symbol, double funding_rate,
                        int interval_hours, uint64_t next_funding_time) {
        PriceKey key = make_key(ex, symbol);
        auto& shard = shard_for(key);

        {
            std::shared_lock read_lock(shard.mutex);
            auto it = shard.prices.find(key);
            if (it != shard.prices.end()) {
                it->second.funding_rate.store(funding_rate, std::memory_order_relaxed);
                it->second.funding_interval_hours.store(interval_hours, std::memory_order_relaxed);
                it->second.next_funding_time.store(next_funding_time, std::memory_order_relaxed);
                return;
            }
        }

        std::unique_lock write_lock(shard.mutex);
        auto& entry = shard.prices[key];
        entry.funding_rate.store(funding_rate, std::memory_order_relaxed);
        entry.funding_interval_hours.store(interval_hours, std::memory_order_relaxed);
        entry.next_funding_time.store(next_funding_time, std::memory_order_relaxed);
    }

    void update_usdt_krw(Exchange ex, double price) {
        if (ex == Exchange::Upbit) {
            usdt_krw_upbit_.store(price, std::memory_order_release);
        } else if (ex == Exchange::Bithumb) {
            usdt_krw_bithumb_.store(price, std::memory_order_release);
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
        return {
            entry.bid.load(std::memory_order_acquire),
            entry.ask.load(std::memory_order_acquire),
            entry.last.load(std::memory_order_acquire),
            entry.funding_rate.load(std::memory_order_acquire),
            entry.next_funding_time.load(std::memory_order_acquire),
            entry.timestamp.load(std::memory_order_acquire),
            entry.funding_interval_hours.load(std::memory_order_acquire),
            true  // valid
        };
    }

    double get_usdt_krw(Exchange ex) const {
        if (ex == Exchange::Upbit) {
            return usdt_krw_upbit_.load(std::memory_order_acquire);
        } else if (ex == Exchange::Bithumb) {
            return usdt_krw_bithumb_.load(std::memory_order_acquire);
        }
        return 0.0;
    }

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

    struct alignas(64) PositionSlot {
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
};

/**
 * Capital tracker for compound growth
 * Tracks total capital and calculates dynamic position sizes
 */
class CapitalTracker {
private:
    std::atomic<double> initial_capital_{2000.0};     // Starting capital in USD
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
        // Example: $2000 / 1 / 2 = $1000, capped to $250 per side
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
        double foreign_bid{0.0};
        double foreign_ask{0.0};
        double korean_price{0.0};
        double foreign_price{0.0};
        double usdt_rate{0.0};
        double entry_premium{0.0};
        double exit_premium{0.0};
        double premium_spread{0.0};   // entry - exit
        double premium{0.0};
        double funding_rate{0.0};
        int funding_interval_hours{8};
        uint64_t next_funding_time{0};
        bool entry_signal{false};
        bool exit_signal{false};
    };

    ArbitrageEngine() = default;
    ~ArbitrageEngine();

    // Configuration
    void set_exchange(Exchange ex, ExchangePtr exchange);
    void add_symbol(const SymbolId& symbol);
    void add_exchange_pair(Exchange korean, Exchange foreign);

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

    std::vector<std::pair<Exchange, Exchange>> exchange_pairs_;

    // Price and position tracking
    PriceCache price_cache_;
    PositionTracker position_tracker_;
    CapitalTracker capital_tracker_{2000.0};  // 초기 자본 $2000

    // ── Incremental entry premium cache (lock-free, O(1) per-symbol update) ──
    // Every ticker update recomputes only the affected symbol's premium.
    // Entry detection scans this cache-hot array instead of doing PriceCache
    // lookups + mutex + hash per symbol.  Zero-miss, zero-delay.
    static constexpr size_t MAX_CACHED_SYMBOLS = 256;  // 256 * 64B = 16KB, fits L1
    struct alignas(64) CachedEntryPremium {
        std::atomic<double> entry_premium{100.0};  // High default = no signal
        std::atomic<double> korean_ask{0.0};
        std::atomic<double> foreign_bid{0.0};
        std::atomic<double> funding_rate{0.0};
        std::atomic<double> usdt_rate{0.0};
        std::atomic<int> funding_interval{0};
        std::atomic<bool> qualified{false};         // All entry filters passed
        std::atomic<bool> signal_fired{false};      // Dedup: reset when disqualified
    };
    std::array<CachedEntryPremium, MAX_CACHED_SYMBOLS> entry_cache_{};

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

    // Internal methods
    void monitor_loop();
    void check_exit_conditions();      // 각 포지션 개별 청산 체크

    // Incremental entry system
    void update_symbol_entry(size_t idx);          // O(1) per-symbol premium recompute
    void update_all_entries();                      // O(N) on USDT change (infrequent)
    void fire_entry_from_cache();                   // O(N) lightweight scan, fires signals
    void check_symbol_exit(size_t idx);             // O(1) per-symbol exit check

    static bool is_korean_exchange(Exchange ex) {
        return ex == Exchange::Upbit || ex == Exchange::Bithumb;
    }
};

} // namespace kimp::strategy
