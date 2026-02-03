#pragma once

#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <string>

namespace kimp::strategy {

/**
 * Thread-safe price cache using unordered_map
 *
 * Uses shared_mutex for read-heavy workload optimization:
 * - Multiple readers can access simultaneously
 * - Writers get exclusive access
 * - No hash collision issues - guaranteed correct symbol matching
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
    // Key: "EXCHANGE:BASE/QUOTE" (e.g., "Bybit:BTC/USDT")
    struct PriceEntry {
        std::atomic<double> bid{0.0};
        std::atomic<double> ask{0.0};
        std::atomic<double> last{0.0};
        std::atomic<double> funding_rate{0.0};
        std::atomic<int> funding_interval_hours{8};
        std::atomic<uint64_t> next_funding_time{0};
        std::atomic<uint64_t> timestamp{0};
    };

    // Use string key for guaranteed uniqueness (no hash collisions)
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, PriceEntry> prices_;

    // USDT/KRW prices
    std::atomic<double> usdt_krw_upbit_{0.0};
    std::atomic<double> usdt_krw_bithumb_{0.0};

    // Create unique key from exchange and symbol
    static std::string make_key(Exchange ex, const SymbolId& symbol) {
        std::string key;
        key.reserve(32);
        key += std::to_string(static_cast<int>(ex));
        key += ':';
        key += symbol.get_base();
        key += '/';
        key += symbol.get_quote();
        return key;
    }

public:
    void update(Exchange ex, const SymbolId& symbol, double bid, double ask, double last) {
        std::string key = make_key(ex, symbol);

        {
            std::shared_lock read_lock(mutex_);
            auto it = prices_.find(key);
            if (it != prices_.end()) {
                // Fast path: entry exists, just update atomics
                it->second.bid.store(bid, std::memory_order_relaxed);
                it->second.ask.store(ask, std::memory_order_relaxed);
                it->second.last.store(last, std::memory_order_relaxed);
                it->second.timestamp.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_release);
                return;
            }
        }

        // Slow path: need to create entry
        std::unique_lock write_lock(mutex_);
        auto& entry = prices_[key];
        entry.bid.store(bid, std::memory_order_relaxed);
        entry.ask.store(ask, std::memory_order_relaxed);
        entry.last.store(last, std::memory_order_relaxed);
        entry.timestamp.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_release);
    }

    void update_funding(Exchange ex, const SymbolId& symbol, double funding_rate,
                        int interval_hours, uint64_t next_funding_time) {
        std::string key = make_key(ex, symbol);

        {
            std::shared_lock read_lock(mutex_);
            auto it = prices_.find(key);
            if (it != prices_.end()) {
                it->second.funding_rate.store(funding_rate, std::memory_order_relaxed);
                it->second.funding_interval_hours.store(interval_hours, std::memory_order_relaxed);
                it->second.next_funding_time.store(next_funding_time, std::memory_order_relaxed);
                return;
            }
        }

        std::unique_lock write_lock(mutex_);
        auto& entry = prices_[key];
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
        std::string key = make_key(ex, symbol);

        std::shared_lock lock(mutex_);
        auto it = prices_.find(key);
        if (it == prices_.end()) {
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
        return TradingConfig::DEFAULT_USDT_KRW;
    }

    // Debug: get all stored symbols for an exchange
    std::vector<std::string> get_all_keys() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> keys;
        keys.reserve(prices_.size());
        for (const auto& [key, _] : prices_) {
            keys.push_back(key);
        }
        return keys;
    }

    size_t size() const {
        std::shared_lock lock(mutex_);
        return prices_.size();
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

    bool has_position(const SymbolId& symbol) const noexcept {
        uint64_t hash = symbol.hash();
        for (const auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire) &&
                slot.symbol_hash.load(std::memory_order_acquire) == hash) {
                return true;
            }
        }
        return false;
    }

    bool has_any_position() const noexcept {
        return position_count_.load(std::memory_order_acquire) > 0;
    }

    const Position* get_position(const SymbolId& symbol) const noexcept {
        uint64_t hash = symbol.hash();
        for (const auto& slot : positions_) {
            if (slot.active.load(std::memory_order_acquire) &&
                slot.symbol_hash.load(std::memory_order_acquire) == hash) {
                return &slot.position;
            }
        }
        return nullptr;
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
        double korean_price{0.0};
        double foreign_price{0.0};
        double usdt_rate{0.0};
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

    // Signals
    std::optional<ArbitrageSignal> get_entry_signal();
    std::optional<ExitSignal> get_exit_signal();

    // Analysis
    double calculate_premium(const SymbolId& symbol, Exchange korean_ex, Exchange foreign_ex) const;
    std::vector<PremiumInfo> get_all_premiums() const;
    const PriceCache& get_price_cache() const { return price_cache_; }
    PriceCache& get_price_cache() { return price_cache_; }

    // Callbacks
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

    // Symbol tracking with O(1) lookup
    std::vector<SymbolId> monitored_symbols_;
    std::vector<SymbolId> foreign_symbols_;  // Pre-computed foreign symbols
    std::unordered_map<size_t, size_t> korean_symbol_index_;   // hash -> index
    std::unordered_map<size_t, size_t> foreign_symbol_index_;  // hash -> index

    std::vector<std::pair<Exchange, Exchange>> exchange_pairs_;

    // Price and position tracking
    PriceCache price_cache_;
    PositionTracker position_tracker_;

    // Signal queues (lock-free)
    memory::SPSCRingBuffer<ArbitrageSignal, 64> entry_signals_;
    memory::SPSCRingBuffer<ExitSignal, 64> exit_signals_;

    // Callbacks
    EntryCallback on_entry_signal_;
    ExitCallback on_exit_signal_;

    // Thread control
    std::atomic<bool> running_{false};
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

    // Internal methods
    void monitor_loop();
    void check_entry_opportunities();
    void check_exit_conditions();
    void check_symbol_opportunity(Exchange updated_ex, const SymbolId& updated_symbol);

    static bool is_korean_exchange(Exchange ex) {
        return ex == Exchange::Upbit || ex == Exchange::Bithumb;
    }
};

} // namespace kimp::strategy
