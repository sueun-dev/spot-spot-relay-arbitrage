#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/simd_premium.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <fmt/format.h>

namespace kimp::strategy {

namespace {

uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double spread_pct(double bid, double ask) {
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) {
        return std::numeric_limits<double>::infinity();
    }
    double mid = (bid + ask) * 0.5;
    if (mid <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return ((ask - bid) / mid) * 100.0;
}

bool quote_is_fresh(const PriceCache::PriceData& price, uint64_t now_ms) {
    if (!price.valid || price.timestamp == 0) {
        return false;
    }
    if (now_ms < price.timestamp) {
        return false;
    }
    return (now_ms - price.timestamp) <= TradingConfig::MAX_QUOTE_AGE_MS;
}

bool quote_pair_is_usable(Exchange korean_ex,
                          Exchange foreign_ex,
                          const PriceCache::PriceData& korean_price,
                          const PriceCache::PriceData& foreign_price) {
    (void)korean_ex;
    (void)foreign_ex;

    const uint64_t now_ms = steady_now_ms();
    if (!quote_is_fresh(korean_price, now_ms) || !quote_is_fresh(foreign_price, now_ms)) {
        return false;
    }

    const uint64_t ts_diff = korean_price.timestamp >= foreign_price.timestamp
                                 ? korean_price.timestamp - foreign_price.timestamp
                                 : foreign_price.timestamp - korean_price.timestamp;
    if (ts_diff > TradingConfig::MAX_QUOTE_DESYNC_MS) {
        return false;
    }

    const double kr_spread = spread_pct(korean_price.bid, korean_price.ask);
    const double fr_spread = spread_pct(foreign_price.bid, foreign_price.ask);
    if (kr_spread > TradingConfig::MAX_KOREAN_SPREAD_PCT) {
        return false;
    }
    if (fr_spread > TradingConfig::MAX_FOREIGN_SPREAD_PCT) {
        return false;
    }

    return true;
}

} // namespace

// PriceCache is now fully inline in header (using std::unordered_map with string keys)
// PositionTracker is now fully inline in header (lock-free implementation)

// ArbitrageEngine implementation
ArbitrageEngine::~ArbitrageEngine() {
    stop();
    stop_async_exporter();
}

void ArbitrageEngine::set_exchange(Exchange ex, ExchangePtr exchange) {
    exchanges_[static_cast<size_t>(ex)] = std::move(exchange);
}

void ArbitrageEngine::add_symbol(const SymbolId& symbol) {
    // Check if already exists using O(1) lookup (collision-safe)
    if (korean_symbol_index_.count(symbol)) {
        return;  // Already added
    }

    // Add to vectors
    size_t idx = monitored_symbols_.size();
    monitored_symbols_.push_back(symbol);

    // Pre-compute foreign symbol (BTC/KRW -> BTC/USDT)
    SymbolId foreign_symbol(symbol.get_base(), "USDT");
    foreign_symbols_.push_back(foreign_symbol);

    // Populate O(1) lookup maps (SymbolId key — no hash collision risk)
    korean_symbol_index_[symbol] = idx;
    foreign_symbol_index_[foreign_symbol] = idx;
    // entry_cache_[idx] is pre-initialized (fixed array, default values)
}

void ArbitrageEngine::add_exchange_pair(Exchange korean, Exchange foreign) {
    exchange_pairs_.emplace_back(korean, foreign);
}

void ArbitrageEngine::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    Logger::info("ArbitrageEngine starting with {} symbols, {} exchange pairs",
                 monitored_symbols_.size(), exchange_pairs_.size());

    monitor_thread_ = std::thread(&ArbitrageEngine::monitor_loop, this);
}

void ArbitrageEngine::stop() {
    if (!running_.exchange(false)) {
        return;  // Not running
    }

    cv_.notify_all();
    update_cv_.notify_all();  // Wake up any wait_for_update() callers

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    Logger::info("ArbitrageEngine stopped");
}

void ArbitrageEngine::on_ticker_update(const Ticker& ticker) {
    const uint64_t ticker_ts_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ticker.timestamp.time_since_epoch()).count());
    price_cache_.update(ticker.exchange, ticker.symbol, ticker.bid, ticker.ask, ticker.last,
                        ticker_ts_ms);

    // Update funding rate for futures exchanges (cache even if 0)
    if (kimp::is_foreign_exchange(ticker.exchange)) {
        if (std::isfinite(ticker.funding_rate) || ticker.next_funding_time != 0) {
            price_cache_.update_funding(ticker.exchange, ticker.symbol,
                ticker.funding_rate, ticker.funding_interval_hours, ticker.next_funding_time);
        }
    }

    // Update USDT price if this is USDT/KRW (fast char-based check)
    if (ticker.symbol.is_usdt_krw()) {
        double usdt_price = ticker.last;
        if (ticker.bid > 0.0 && ticker.ask > 0.0 && ticker.ask >= ticker.bid) {
            usdt_price = (ticker.bid + ticker.ask) * 0.5;  // Prefer executable mid over last trade
        }
        on_usdt_update(ticker.exchange, usdt_price);

        // USDT rate affects ALL premiums → recompute everything
        update_all_entries();
        fire_entry_from_cache();
        check_exit_conditions();

        update_seq_.fetch_add(1, std::memory_order_release);
        update_cv_.notify_all();
        return;
    }

    // ── Incremental premium update ──
    // O(1): Recompute only the symbol that changed, then scan cache for best.
    bool is_korean = is_korean_exchange(ticker.exchange);
    size_t idx = SIZE_MAX;

    if (is_korean) {
        auto it = korean_symbol_index_.find(ticker.symbol);
        if (it != korean_symbol_index_.end()) idx = it->second;
    } else {
        auto it = foreign_symbol_index_.find(ticker.symbol);
        if (it != foreign_symbol_index_.end()) idx = it->second;
    }

    if (idx != SIZE_MAX) {
        // O(1) premium recompute for this symbol
        update_symbol_entry(idx);

        // O(N) lightweight scan of cache-hot atomics → fire entry signal
        fire_entry_from_cache();

        // O(1) exit check for this symbol only (if holding position)
        if (position_tracker_.has_position(monitored_symbols_[idx])) {
            check_symbol_exit(idx);
        }
    }

    update_seq_.fetch_add(1, std::memory_order_release);
    update_cv_.notify_all();
}

void ArbitrageEngine::on_usdt_update(Exchange ex, double price) {
    if (price <= 0.0) {
        return;
    }

    double prev = price_cache_.get_usdt_krw(ex);
    if (prev > 0.0) {
        double jump_pct = std::fabs(price - prev) / prev * 100.0;
        if (jump_pct > TradingConfig::MAX_USDT_JUMP_PCT) {
            Logger::warn("USDT/KRW outlier filtered from {}: prev={:.2f}, new={:.2f}, jump={:.2f}%",
                         ex == Exchange::Bithumb ? "Bithumb" : "Upbit",
                         prev, price, jump_pct);
            return;
        }
    }

    const auto idx = static_cast<size_t>(ex);
    const double last_logged = last_usdt_log_[idx].load(std::memory_order_relaxed);
    if (std::fabs(price - last_logged) >= 1.0) {
        Logger::debug("USDT/KRW update from {}: {:.2f}", ex == Exchange::Bithumb ? "Bithumb" : "Upbit", price);
        last_usdt_log_[idx].store(price, std::memory_order_relaxed);
    }
    price_cache_.update_usdt_krw(ex, price);
}

bool ArbitrageEngine::close_position(const SymbolId& symbol, Position& closed) {
    return position_tracker_.close_position(symbol, closed);
}

std::optional<ArbitrageSignal> ArbitrageEngine::get_entry_signal() {
    return entry_signals_.try_pop();
}

std::optional<ExitSignal> ArbitrageEngine::get_exit_signal() {
    return exit_signals_.try_pop();
}

void ArbitrageEngine::wait_for_update(uint64_t last_seq, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(update_mutex_);
    update_cv_.wait_for(lock, timeout, [this, last_seq] {
        return update_seq_.load(std::memory_order_acquire) != last_seq;
    });
}

double ArbitrageEngine::calculate_premium(const SymbolId& symbol,
                                           Exchange korean_ex,
                                           Exchange foreign_ex) const {
    // Get Korean price (need ASK for entry calculation)
    auto korean_price = price_cache_.get_price(korean_ex, symbol);
    if (!korean_price.valid || korean_price.ask <= 0) {
        return 0.0;
    }

    // Get foreign price (need BID for entry calculation)
    // O(1) hash map lookup instead of linear search
    const SymbolId* foreign_symbol_ptr = nullptr;
    SymbolId fallback_symbol;

    auto it = korean_symbol_index_.find(symbol);
    if (it != korean_symbol_index_.end()) {
        foreign_symbol_ptr = &foreign_symbols_[it->second];
    } else {
        // Fallback: create temporary if symbol not in monitored list
        fallback_symbol = SymbolId(symbol.get_base(), "USDT");
        foreign_symbol_ptr = &fallback_symbol;
    }

    auto foreign_price = price_cache_.get_price(foreign_ex, *foreign_symbol_ptr);
    if (!foreign_price.valid || foreign_price.bid <= 0) {
        return 0.0;
    }
    if (!quote_pair_is_usable(korean_ex, foreign_ex, korean_price, foreign_price)) {
        return 0.0;
    }

    // Get USDT/KRW rate - must be real data
    double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
    if (usdt_rate <= 0) {
        return 0.0;  // No real USDT/KRW rate available, skip
    }

    return PremiumCalculator::calculate_entry_premium(
        korean_price.ask, foreign_price.bid, usdt_rate);
}

void ArbitrageEngine::monitor_loop() {
    // Apply CPU pinning and RT priority for strategy thread
    auto thread_config = opt::ThreadConfig::optimal();
    if (thread_config.strategy_core >= 0) {
        if (opt::pin_to_core(thread_config.strategy_core)) {
            Logger::info("Strategy thread pinned to core {}", thread_config.strategy_core);
        }
    }
    if (opt::set_realtime_priority()) {
        Logger::info("Strategy thread set to realtime priority");
    }

    Logger::info("ArbitrageEngine monitor loop started (exit backup only — entry is fully event-driven)");

    // Entry: FULLY event-driven via on_ticker_update() → update_symbol_entry() → fire_entry_from_cache()
    //        No backup needed. Every ticker fires an incremental premium update + cache scan.
    //        Zero-miss by design: premium can only change when a price changes, and every price
    //        change triggers a ticker → on_ticker_update.
    //
    // Exit:  Event-driven per-symbol via check_symbol_exit(), with 250ms backup safety net.
    //        Backup is kept for exit because positions are critical to close.
    constexpr auto loop_interval = std::chrono::milliseconds(50);
    constexpr auto exit_backup_interval = std::chrono::milliseconds(250);
    auto next_exit_backup = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();

        // Exit backup only when there are active positions.
        if (position_tracker_.has_any_position() && now >= next_exit_backup) {
            check_exit_conditions();
            next_exit_backup = now + exit_backup_interval;
        } else if (now >= next_exit_backup) {
            next_exit_backup = now + exit_backup_interval;
        }

        std::this_thread::sleep_for(loop_interval);
    }

    Logger::info("ArbitrageEngine monitor loop ended");
}

// ── Incremental entry system ──
// update_symbol_entry: O(1) recompute premium for one symbol
// update_all_entries:  O(N) recompute all (called on USDT change)
// fire_entry_from_cache: O(N) lightweight scan of cache-hot atomics

void ArbitrageEngine::update_symbol_entry(size_t idx) {
    if (idx >= monitored_symbols_.size()) return;
    auto& cache = entry_cache_[idx];

    if (exchange_pairs_.empty()) {
        cache.qualified.store(false, std::memory_order_release);
        return;
    }
    const auto& [korean_ex, foreign_ex] = exchange_pairs_[0];

    const auto& symbol = monitored_symbols_[idx];
    const auto& foreign_symbol = foreign_symbols_[idx];

    auto korean_price = price_cache_.get_price(korean_ex, symbol);
    if (!korean_price.valid || korean_price.ask <= 0) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
    if (!foreign_price.valid || foreign_price.bid <= 0) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    if (!quote_pair_is_usable(korean_ex, foreign_ex, korean_price, foreign_price)) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    // Funding filters
    if (foreign_price.funding_interval_hours != TradingConfig::MIN_FUNDING_INTERVAL_HOURS) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    if (TradingConfig::REQUIRE_POSITIVE_FUNDING && foreign_price.funding_rate < 0.0) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
    if (usdt_rate <= 0) {
        if (cache.qualified.load(std::memory_order_relaxed)) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        return;
    }

    double premium = PremiumCalculator::calculate_entry_premium(
        korean_price.ask, foreign_price.bid, usdt_rate);

    bool qualifies = (premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD);

    // Update cached values
    cache.entry_premium.store(premium, std::memory_order_relaxed);
    cache.korean_ask.store(korean_price.ask, std::memory_order_relaxed);
    cache.foreign_bid.store(foreign_price.bid, std::memory_order_relaxed);
    cache.funding_rate.store(foreign_price.funding_rate, std::memory_order_relaxed);
    cache.usdt_rate.store(usdt_rate, std::memory_order_relaxed);
    cache.funding_interval.store(foreign_price.funding_interval_hours, std::memory_order_relaxed);

    bool was_qualified = cache.qualified.load(std::memory_order_relaxed);
    cache.qualified.store(qualifies, std::memory_order_release);

    // Reset signal_fired when qualification state changes (re-enable signal)
    if (was_qualified != qualifies) {
        cache.signal_fired.store(false, std::memory_order_release);
    }
}

void ArbitrageEngine::update_all_entries() {
    for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
        update_symbol_entry(i);
    }
}

void ArbitrageEngine::fire_entry_from_cache() {
    if (!position_tracker_.can_open_position()) return;
    if (exchange_pairs_.empty()) return;

    const auto& [korean_ex, foreign_ex] = exchange_pairs_[0];

    if (TradingConfig::MAX_POSITIONS == 1) {
        // ── Single position mode: find best qualifying symbol ──
        double best_premium = 100.0;
        size_t best_idx = SIZE_MAX;

        for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
            if (!entry_cache_[i].qualified.load(std::memory_order_acquire)) continue;
            if (position_tracker_.has_position(monitored_symbols_[i])) continue;
            double pm = entry_cache_[i].entry_premium.load(std::memory_order_relaxed);
            if (pm < best_premium) {
                best_premium = pm;
                best_idx = i;
            }
        }

        if (best_idx == SIZE_MAX) return;

        // Dedup: don't re-fire if already signaled for this symbol
        if (entry_cache_[best_idx].signal_fired.load(std::memory_order_acquire)) return;
        entry_cache_[best_idx].signal_fired.store(true, std::memory_order_release);

        auto& c = entry_cache_[best_idx];
        ArbitrageSignal signal;
        signal.symbol = monitored_symbols_[best_idx];
        signal.korean_exchange = korean_ex;
        signal.foreign_exchange = foreign_ex;
        signal.premium = c.entry_premium.load(std::memory_order_relaxed);
        signal.korean_ask = c.korean_ask.load(std::memory_order_relaxed);
        signal.foreign_bid = c.foreign_bid.load(std::memory_order_relaxed);
        signal.funding_rate = c.funding_rate.load(std::memory_order_relaxed);
        signal.usdt_krw_rate = c.usdt_rate.load(std::memory_order_relaxed);
        signal.timestamp = std::chrono::steady_clock::now();

        if (on_entry_signal_) on_entry_signal_(signal);
        entry_signals_.try_push(signal);
    } else {
        // ── Multi-position mode: fire for each qualifying symbol ──
        for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
            if (!position_tracker_.can_open_position()) break;
            if (!entry_cache_[i].qualified.load(std::memory_order_acquire)) continue;
            if (position_tracker_.has_position(monitored_symbols_[i])) continue;
            if (entry_cache_[i].signal_fired.load(std::memory_order_acquire)) continue;
            entry_cache_[i].signal_fired.store(true, std::memory_order_release);

            auto& c = entry_cache_[i];
            ArbitrageSignal signal;
            signal.symbol = monitored_symbols_[i];
            signal.korean_exchange = korean_ex;
            signal.foreign_exchange = foreign_ex;
            signal.premium = c.entry_premium.load(std::memory_order_relaxed);
            signal.korean_ask = c.korean_ask.load(std::memory_order_relaxed);
            signal.foreign_bid = c.foreign_bid.load(std::memory_order_relaxed);
            signal.funding_rate = c.funding_rate.load(std::memory_order_relaxed);
            signal.usdt_krw_rate = c.usdt_rate.load(std::memory_order_relaxed);
            signal.timestamp = std::chrono::steady_clock::now();

            if (on_entry_signal_) on_entry_signal_(signal);
            entry_signals_.try_push(signal);
        }
    }
}

void ArbitrageEngine::check_exit_conditions() {
    // Fast check: no position = no exit check needed
    if (!position_tracker_.has_any_position()) return;

    auto positions = position_tracker_.get_active_positions();
    if (positions.empty()) return;

    for (const auto& pos : positions) {
        // O(1) hash map lookup instead of linear search
        const SymbolId* foreign_symbol_ptr = nullptr;
        SymbolId fallback_symbol;

        auto it = korean_symbol_index_.find(pos.symbol);
        if (it != korean_symbol_index_.end()) {
            foreign_symbol_ptr = &foreign_symbols_[it->second];
        } else {
            fallback_symbol = SymbolId(pos.symbol.get_base(), "USDT");
            foreign_symbol_ptr = &fallback_symbol;
        }

        // Get prices
        auto korean_price = price_cache_.get_price(pos.korean_exchange, pos.symbol);
        if (!korean_price.valid || korean_price.bid <= 0) continue;

        auto foreign_price = price_cache_.get_price(pos.foreign_exchange, *foreign_symbol_ptr);
        if (!foreign_price.valid || foreign_price.ask <= 0) continue;
        if (!quote_pair_is_usable(pos.korean_exchange, pos.foreign_exchange, korean_price, foreign_price)) continue;

        double usdt_rate = price_cache_.get_usdt_krw(pos.korean_exchange);
        if (usdt_rate <= 0) continue;  // No real USDT/KRW rate

        // Calculate exit premium
        double premium = PremiumCalculator::calculate_exit_premium(
            korean_price.bid, foreign_price.ask, usdt_rate);

        // Dynamic exit threshold with a hard floor (+0.10% by default)
        double dynamic_exit = std::max(
            pos.entry_premium + TradingConfig::DYNAMIC_EXIT_SPREAD,
            TradingConfig::EXIT_PREMIUM_THRESHOLD);
        if (premium < dynamic_exit) continue;

        // Generate signal
        ExitSignal signal;
        signal.symbol = pos.symbol;
        signal.korean_exchange = pos.korean_exchange;
        signal.foreign_exchange = pos.foreign_exchange;
        signal.premium = premium;
        signal.korean_bid = korean_price.bid;
        signal.foreign_ask = foreign_price.ask;
        signal.usdt_krw_rate = usdt_rate;
        signal.timestamp = std::chrono::steady_clock::now();

        if (on_exit_signal_) {
            on_exit_signal_(signal);
        }
        exit_signals_.try_push(signal);
    }
}

void ArbitrageEngine::check_symbol_exit(size_t idx) {
    if (idx >= monitored_symbols_.size()) return;

    const auto& korean_symbol = monitored_symbols_[idx];
    const auto& foreign_symbol = foreign_symbols_[idx];

    auto pos_opt = position_tracker_.get_position(korean_symbol);
    if (!pos_opt) return;
    const auto& pos = *pos_opt;

    auto korean_price = price_cache_.get_price(pos.korean_exchange, korean_symbol);
    if (!korean_price.valid || korean_price.bid <= 0) return;

    auto foreign_price = price_cache_.get_price(pos.foreign_exchange, foreign_symbol);
    if (!foreign_price.valid || foreign_price.ask <= 0) return;
    if (!quote_pair_is_usable(pos.korean_exchange, pos.foreign_exchange, korean_price, foreign_price)) return;

    double usdt_rate = price_cache_.get_usdt_krw(pos.korean_exchange);
    if (usdt_rate <= 0) return;

    double premium = PremiumCalculator::calculate_exit_premium(
        korean_price.bid, foreign_price.ask, usdt_rate);

    // Dynamic exit threshold with a hard floor
    double dynamic_exit = std::max(
        pos.entry_premium + TradingConfig::DYNAMIC_EXIT_SPREAD,
        TradingConfig::EXIT_PREMIUM_THRESHOLD);
    if (premium < dynamic_exit) return;

    ExitSignal signal;
    signal.symbol = korean_symbol;
    signal.korean_exchange = pos.korean_exchange;
    signal.foreign_exchange = pos.foreign_exchange;
    signal.premium = premium;
    signal.korean_bid = korean_price.bid;
    signal.foreign_ask = foreign_price.ask;
    signal.usdt_krw_rate = usdt_rate;
    signal.timestamp = std::chrono::steady_clock::now();

    if (on_exit_signal_) {
        on_exit_signal_(signal);
    }
    exit_signals_.try_push(signal);
}

std::vector<ArbitrageEngine::PremiumInfo> ArbitrageEngine::get_all_premiums() const {
    const size_t n = monitored_symbols_.size();
    if (n == 0) return {};

    // SoA arrays for SIMD - collect directly (no intermediate struct copy)
    // Entry arrays (korean_ask / foreign_bid)
    std::vector<double> korean_asks;
    std::vector<double> foreign_bids;
    // Exit arrays (korean_bid / foreign_ask)
    std::vector<double> korean_bids;
    std::vector<double> foreign_asks;

    std::vector<double> usdt_rates;
    std::vector<double> funding_rates;
    std::vector<int> funding_intervals;
    std::vector<uint64_t> next_funding_times;
    std::vector<size_t> symbol_indices;

    korean_asks.reserve(n);
    foreign_bids.reserve(n);
    korean_bids.reserve(n);
    foreign_asks.reserve(n);
    usdt_rates.reserve(n);
    funding_rates.reserve(n);
    funding_intervals.reserve(n);
    next_funding_times.reserve(n);
    symbol_indices.reserve(n);

    // Phase 1: Collect valid prices directly into SoA arrays (single pass)
    for (size_t i = 0; i < n; ++i) {
        const auto& symbol = monitored_symbols_[i];
        const auto& foreign_symbol = foreign_symbols_[i];

        for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
            auto korean_price = price_cache_.get_price(korean_ex, symbol);
            if (!korean_price.valid || korean_price.ask <= 0) continue;

            auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
            if (!foreign_price.valid || foreign_price.bid <= 0) continue;
            if (!quote_pair_is_usable(korean_ex, foreign_ex, korean_price, foreign_price)) continue;

            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) continue;  // No real USDT/KRW rate

            korean_asks.push_back(korean_price.ask);
            foreign_bids.push_back(foreign_price.bid);
            korean_bids.push_back(korean_price.bid);
            foreign_asks.push_back(foreign_price.ask);
            usdt_rates.push_back(usdt_rate);
            funding_rates.push_back(foreign_price.funding_rate);
            funding_intervals.push_back(foreign_price.funding_interval_hours);
            next_funding_times.push_back(foreign_price.next_funding_time);
            symbol_indices.push_back(i);
            break; // Only first exchange pair per symbol
        }
    }

    const size_t count = korean_asks.size();
    if (count == 0) return {};

    // Phase 2: SIMD batch premium calculation
    // Entry premium: (korean_ask - foreign_bid * usdt) / (foreign_bid * usdt) * 100
    std::vector<double> entry_premiums(count);
    SIMDPremiumCalculator::calculate_batch(
        korean_asks.data(),
        foreign_bids.data(),
        usdt_rates.data(),
        entry_premiums.data(),
        count
    );
    // Exit premium: (korean_bid - foreign_ask * usdt) / (foreign_ask * usdt) * 100
    std::vector<double> exit_premiums(count);
    SIMDPremiumCalculator::calculate_batch(
        korean_bids.data(),
        foreign_asks.data(),
        usdt_rates.data(),
        exit_premiums.data(),
        count
    );

    // Phase 3: Build result
    std::vector<PremiumInfo> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        PremiumInfo info;
        info.symbol = monitored_symbols_[symbol_indices[i]];
        info.korean_bid = korean_bids[i];
        info.korean_ask = korean_asks[i];
        info.foreign_bid = foreign_bids[i];
        info.foreign_ask = foreign_asks[i];
        info.korean_price = korean_asks[i];
        info.foreign_price = foreign_bids[i];
        info.usdt_rate = usdt_rates[i];
        info.entry_premium = entry_premiums[i];
        info.exit_premium = exit_premiums[i];
        info.premium_spread = entry_premiums[i] - exit_premiums[i];
        info.premium = entry_premiums[i];  // Backward-compatible alias
        info.funding_rate = funding_rates[i];
        info.funding_interval_hours = funding_intervals[i];
        info.next_funding_time = next_funding_times[i];
        info.entry_signal = (entry_premiums[i] <= TradingConfig::ENTRY_PREMIUM_THRESHOLD) &&
                            (funding_intervals[i] == TradingConfig::MIN_FUNDING_INTERVAL_HOURS) &&
                            (!TradingConfig::REQUIRE_POSITIVE_FUNDING || funding_rates[i] >= 0.0);
        info.exit_signal = exit_premiums[i] >= TradingConfig::EXIT_PREMIUM_THRESHOLD;
        result.push_back(info);
    }

    return result;
}

void ArbitrageEngine::export_to_json(const std::string& path) const {
    auto premiums = get_all_premiums();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Pre-allocate buffer (estimated ~500 bytes per symbol + 200 header)
    std::string buffer;
    buffer.reserve(200 + premiums.size() * 500);

    // Build JSON in memory first (faster than multiple file writes)
    fmt::format_to(std::back_inserter(buffer),
        "{{\n"
        "  \"status\": {{\n"
        "    \"connected\": {},\n"
        "    \"upbitConnected\": true,\n"
        "    \"bybitConnected\": true,\n"
        "    \"symbolCount\": {},\n"
        "    \"lastUpdate\": {}\n"
        "  }},\n"
        "  \"premiums\": [\n",
        running_.load() ? "true" : "false",
        premiums.size(),
        now_ms);

    for (size_t i = 0; i < premiums.size(); ++i) {
        const auto& p = premiums[i];
        const char* signal_str = p.entry_signal ? "\"ENTRY\"" : (p.exit_signal ? "\"EXIT\"" : "null");

        fmt::format_to(std::back_inserter(buffer),
            "    {{\n"
            "      \"symbol\": \"{}/{}\",\n"
            "      \"koreanBid\": {:.2f},\n"
            "      \"koreanAsk\": {:.2f},\n"
            "      \"foreignBid\": {:.6f},\n"
            "      \"foreignAsk\": {:.6f},\n"
            "      \"koreanPrice\": {:.2f},\n"
            "      \"foreignPrice\": {:.4f},\n"
            "      \"usdtRate\": {:.2f},\n"
            "      \"entryPremium\": {:.4f},\n"
            "      \"exitPremium\": {:.4f},\n"
            "      \"premiumSpread\": {:.4f},\n"
            "      \"premium\": {:.4f},\n"
            "      \"fundingRate\": {:.6f},\n"
            "      \"fundingIntervalHours\": {},\n"
            "      \"nextFundingTime\": {},\n"
            "      \"signal\": {},\n"
            "      \"timestamp\": {}\n"
            "    }}",
            p.symbol.get_base(), p.symbol.get_quote(),
            p.korean_bid, p.korean_ask,
            p.foreign_bid, p.foreign_ask,
            p.korean_price, p.foreign_price, p.usdt_rate,
            p.entry_premium, p.exit_premium, p.premium_spread, p.premium,
            p.funding_rate, p.funding_interval_hours, p.next_funding_time,
            signal_str, now_ms);

        if (i < premiums.size() - 1) {
            buffer += ",\n";
        } else {
            buffer += "\n";
        }
    }

    buffer += "  ]\n}\n";

    // Single write to file
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        Logger::error("Failed to open file for export: {}", path);
        return;
    }
    file.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}

// Async JSON Export Implementation
void ArbitrageEngine::export_to_json_async(const std::string& path) {
    // Fire and forget - export runs in background
    std::thread([this, path]() {
        export_to_json(path);
    }).detach();
}

void ArbitrageEngine::start_async_exporter(const std::string& path, std::chrono::milliseconds interval) {
    if (exporter_running_.exchange(true)) {
        return;  // Already running
    }

    export_path_ = path;
    export_interval_ = interval;

    exporter_thread_ = std::thread([this]() {
        // Exporter thread: lower priority (execution_core) since it's not latency-critical
        auto thread_config = opt::ThreadConfig::optimal();
        if (thread_config.execution_core >= 0) {
            opt::pin_to_core(thread_config.execution_core);
        }
        // Note: Not setting RT priority for exporter (it's I/O bound, not latency-critical)

        Logger::info("Async JSON exporter started (interval: {}ms)", export_interval_.count());

        while (exporter_running_.load()) {
            // Export JSON (file I/O happens in this background thread, not main thread)
            export_to_json(export_path_);

            // Wait for next interval or shutdown
            std::unique_lock lock(exporter_mutex_);
            exporter_cv_.wait_for(lock, export_interval_, [this] {
                return !exporter_running_.load();
            });
        }

        Logger::info("Async JSON exporter stopped");
    });
}

void ArbitrageEngine::stop_async_exporter() {
    if (!exporter_running_.exchange(false)) {
        return;  // Not running
    }

    exporter_cv_.notify_all();

    if (exporter_thread_.joinable()) {
        exporter_thread_.join();
    }
}

} // namespace kimp::strategy
