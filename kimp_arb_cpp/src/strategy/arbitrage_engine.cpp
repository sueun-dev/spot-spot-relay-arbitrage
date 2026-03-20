#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/latency_probe.hpp"
#include "kimp/strategy/entry_selection_bitmap.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/price_format.hpp"
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

bool quote_is_fresh_with_limit(const PriceCache::PriceData& price, uint64_t now_ms,
                               uint64_t max_age_ms) {
    if (!price.valid || price.timestamp == 0) return false;
    if (now_ms < price.timestamp) return false;
    return (now_ms - price.timestamp) <= max_age_ms;
}

bool quote_pair_is_usable(Exchange korean_ex,
                          Exchange foreign_ex,
                          const PriceCache::PriceData& korean_price,
                          const PriceCache::PriceData& foreign_price,
                          bool is_exit = false) {
    (void)korean_ex;
    (void)foreign_ex;

    const uint64_t max_age   = is_exit ? TradingConfig::MAX_QUOTE_AGE_MS_EXIT   : TradingConfig::MAX_QUOTE_AGE_MS;
    const uint64_t max_desync = is_exit ? TradingConfig::MAX_QUOTE_DESYNC_MS_EXIT : TradingConfig::MAX_QUOTE_DESYNC_MS;
    const double   max_kr_sp  = is_exit ? TradingConfig::MAX_KOREAN_SPREAD_PCT_EXIT  : TradingConfig::MAX_KOREAN_SPREAD_PCT;
    const double   max_fr_sp  = is_exit ? TradingConfig::MAX_FOREIGN_SPREAD_PCT_EXIT : TradingConfig::MAX_FOREIGN_SPREAD_PCT;

    const uint64_t now_ms = steady_now_ms();
    if (!quote_is_fresh_with_limit(korean_price, now_ms, max_age) ||
        !quote_is_fresh_with_limit(foreign_price, now_ms, max_age)) {
        return false;
    }

    const uint64_t ts_diff = korean_price.timestamp >= foreign_price.timestamp
                                 ? korean_price.timestamp - foreign_price.timestamp
                                 : foreign_price.timestamp - korean_price.timestamp;
    if (ts_diff > max_desync) {
        return false;
    }

    const double kr_spread = spread_pct(korean_price.bid, korean_price.ask);
    const double fr_spread = spread_pct(foreign_price.bid, foreign_price.ask);
    if (kr_spread > max_kr_sp) {
        return false;
    }
    if (fr_spread > max_fr_sp) {
        return false;
    }

    return true;
}

void write_premiums_json_file(
    const std::string& path,
    bool connected,
    const std::vector<kimp::strategy::ArbitrageEngine::PremiumInfo>& premiums) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string buffer;
    buffer.reserve(200 + premiums.size() * 500);

    fmt::format_to(std::back_inserter(buffer),
        "{{\n"
        "  \"status\": {{\n"
        "    \"connected\": {},\n"
        "    \"bithumbConnected\": true,\n"
        "    \"bybitConnected\": true,\n"
        "    \"symbolCount\": {},\n"
        "    \"lastUpdate\": {}\n"
        "  }},\n"
        "  \"premiums\": [\n",
        connected ? "true" : "false",
        premiums.size(),
        now_ms);

    for (size_t i = 0; i < premiums.size(); ++i) {
        const auto& p = premiums[i];
        const char* signal_str = p.entry_signal ? "\"ENTRY\"" : (p.exit_signal ? "\"EXIT\"" : "null");
        const std::string korean_bid = kimp::format::format_decimal_trimmed(p.korean_bid);
        const std::string korean_ask = kimp::format::format_decimal_trimmed(p.korean_ask);
        const std::string korean_price = kimp::format::format_decimal_trimmed(p.korean_price);

        fmt::format_to(std::back_inserter(buffer),
            "    {{\n"
            "      \"symbol\": \"{}/{}\",\n"
            "      \"koreanBid\": {},\n"
            "      \"koreanAsk\": {},\n"
            "      \"koreanBidQty\": {:.8f},\n"
            "      \"koreanAskQty\": {:.8f},\n"
            "      \"foreignBid\": {:.6f},\n"
            "      \"foreignAsk\": {:.6f},\n"
            "      \"foreignBidQty\": {:.8f},\n"
            "      \"foreignAskQty\": {:.8f},\n"
            "      \"koreanPrice\": {},\n"
            "      \"foreignPrice\": {:.4f},\n"
            "      \"usdtRate\": {:.2f},\n"
            "      \"entryPremium\": {:.4f},\n"
            "      \"exitPremium\": {:.4f},\n"
            "      \"premiumSpread\": {:.4f},\n"
            "      \"premium\": {:.4f},\n"
            "      \"matchQty\": {:.8f},\n"
            "      \"targetCoinQty\": {:.8f},\n"
            "      \"maxTradableUsdtAtBest\": {:.8f},\n"
            "      \"bithumbTopKrw\": {:.2f},\n"
            "      \"bithumbTopUsdt\": {:.8f},\n"
            "      \"bybitTopUsdt\": {:.8f},\n"
            "      \"bybitTopKrw\": {:.2f},\n"
            "      \"grossEdgePct\": {:.6f},\n"
            "      \"netEdgePct\": {:.6f},\n"
            "      \"bithumbTotalFeeKrw\": {:.2f},\n"
            "      \"bybitTotalFeeUsdt\": {:.8f},\n"
            "      \"bybitTotalFeeKrw\": {:.2f},\n"
            "      \"totalFeeKrw\": {:.2f},\n"
            "      \"netProfitKrw\": {:.2f},\n"
            "      \"bothCanFillTarget\": {},\n"
            "      \"signal\": {},\n"
            "      \"ageMs\": {},\n"
            "      \"timestamp\": {}\n"
            "    }}",
            p.symbol.get_base(), p.symbol.get_quote(),
            korean_bid, korean_ask,
            p.korean_bid_qty, p.korean_ask_qty,
            p.foreign_bid, p.foreign_ask,
            p.foreign_bid_qty, p.foreign_ask_qty,
            korean_price, p.foreign_price, p.usdt_rate,
            p.entry_premium, p.exit_premium, p.premium_spread, p.premium,
            p.match_qty, p.target_coin_qty, p.max_tradable_usdt_at_best,
            p.bithumb_top_krw, p.bithumb_top_usdt, p.bybit_top_usdt, p.bybit_top_krw,
            p.gross_edge_pct, p.net_edge_pct,
            p.bithumb_total_fee_krw, p.bybit_total_fee_usdt, p.bybit_total_fee_krw, p.total_fee_krw,
            p.net_profit_krw,
            p.both_can_fill_target ? "true" : "false",
            signal_str, p.age_ms, now_ms);

        if (i < premiums.size() - 1) {
            buffer += ",\n";
        } else {
            buffer += "\n";
        }
    }

    buffer += "  ]\n}\n";

    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        Logger::error("Failed to open file for export: {}", path);
        return;
    }
    file.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
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

    if (monitored_symbols_.size() >= MAX_CACHED_SYMBOLS) {
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            Logger::warn(
                "Symbol cache limit reached (MAX_CACHED_SYMBOLS={}). "
                "Additional symbols will be ignored for entry cache path.",
                MAX_CACHED_SYMBOLS
            );
        }
        return;
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
    exchange_pairs_.push_back({korean, foreign, true});
}

void ArbitrageEngine::set_exchange_pair_entry_enabled(Exchange korean, Exchange foreign, bool enabled) {
    for (auto& pair : exchange_pairs_) {
        if (pair.korean == korean && pair.foreign == foreign) {
            pair.entry_enabled = enabled;
        }
    }
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
                        ticker_ts_ms, ticker.bid_qty, ticker.ask_qty);

    // Update USDT price if this is USDT/KRW (fast char-based check)
    if (ticker.symbol.is_usdt_krw()) {
        double usdt_price = ticker.last;
        if (ticker.bid > 0.0 && ticker.ask > 0.0 && ticker.ask >= ticker.bid) {
            usdt_price = (ticker.bid + ticker.ask) * 0.5;  // Prefer executable mid over last trade
        }
        on_usdt_update(ticker.exchange, usdt_price);

        // USDT rate affects ALL premiums → recompute everything
        update_all_entries();
        const uint64_t now_ms = steady_now_ms();
        uint64_t next_due = next_usdt_scan_ms_.load(std::memory_order_relaxed);
        if (now_ms >= next_due) {
            const uint64_t next_target = now_ms + TradingConfig::USDT_FULL_SCAN_DEBOUNCE_MS;
            if (next_usdt_scan_ms_.compare_exchange_strong(next_due, next_target, std::memory_order_acq_rel)) {
                fire_entry_from_cache();
            }
        }
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

        // O(N) cache scan is throttled; bypass throttle when a fresh qualified signal is possible.
        bool should_scan = false;
        if (entry_cache_[idx].qualified.load(std::memory_order_relaxed) &&
            !entry_cache_[idx].signal_fired.load(std::memory_order_relaxed)) {
            should_scan = true;
        } else {
            const uint64_t now_ms = steady_now_ms();
            uint64_t next_due = next_entry_scan_ms_.load(std::memory_order_relaxed);
            if (now_ms >= next_due) {
                const uint64_t next_target = now_ms + TradingConfig::ENTRY_FAST_SCAN_COOLDOWN_MS;
                if (next_entry_scan_ms_.compare_exchange_strong(next_due, next_target, std::memory_order_acq_rel)) {
                    should_scan = true;
                }
            }
        }
        if (should_scan) {
            fire_entry_from_cache();
        }

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
                         exchange_name(ex),
                         prev, price, jump_pct);
            return;
        }
    }

    const auto idx = static_cast<size_t>(ex);
    const double last_logged = last_usdt_log_[idx].load(std::memory_order_relaxed);
    if (std::fabs(price - last_logged) >= 1.0) {
        Logger::debug("USDT/KRW update from {}: {:.2f}", exchange_name(ex), price);
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
    if (update_seq_.load(std::memory_order_acquire) != last_seq) {
        return;
    }

    // Fast path: most wakes happen immediately after an order, so spin briefly
    // before falling back to kernel-assisted sleep.
    constexpr uint32_t spin_iterations = 2048;
    for (uint32_t i = 0; i < spin_iterations; ++i) {
        if (update_seq_.load(std::memory_order_acquire) != last_seq) {
            return;
        }
        opt::cpu_pause();
    }

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
    SymbolId foreign_symbol;
    auto it = korean_symbol_index_.find(symbol);
    if (it != korean_symbol_index_.end()) {
        foreign_symbol = foreign_symbols_[it->second];
    } else {
        foreign_symbol = SymbolId(symbol.get_base(), "USDT");
    }

    // If multiple exchanges exist, pick the pair with the best projected net profit.
    double best_foreign_bid = 0.0;
    double usdt_rate = 0.0;
    double best_net_profit = -1e18;
    double best_net_edge = -1e18;
    const std::string base(symbol.get_base());
    for (const auto& pair : exchange_pairs_) {
        if (pair.korean != korean_ex) continue;
        if (!price_cache_.is_transfer_route_available(pair.korean, pair.foreign, base)) continue;
        auto fp = price_cache_.get_price(pair.foreign, foreign_symbol);
        if (!fp.valid || fp.bid <= 0) continue;
        if (!quote_pair_is_usable(pair.korean, pair.foreign, korean_price, fp)) continue;
        double rate = price_cache_.get_usdt_krw(pair.korean);
        if (rate <= 0) continue;
        double withdraw_fee = price_cache_.get_withdraw_fee(pair.korean, pair.foreign, base);
        auto relay_metrics = PremiumCalculator::calculate_relay_metrics(
            korean_price.ask,
            korean_price.ask_qty,
            fp.bid,
            fp.bid_qty,
            rate,
            TradingConfig::get_korean_fee_rate(pair.korean),
            TradingConfig::get_foreign_fee_rate(pair.foreign),
            withdraw_fee);
        if (relay_metrics.net_profit_krw > best_net_profit ||
            (relay_metrics.net_profit_krw == best_net_profit &&
             relay_metrics.net_edge_pct > best_net_edge)) {
            best_net_profit = relay_metrics.net_profit_krw;
            best_net_edge = relay_metrics.net_edge_pct;
            best_foreign_bid = fp.bid;
            usdt_rate = rate;
        }
    }

    // Fallback: try the explicitly requested exchange
    if (best_foreign_bid <= 0.0) {
        if (!price_cache_.is_transfer_route_available(korean_ex, foreign_ex, base)) return 0.0;
        auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
        if (!foreign_price.valid || foreign_price.bid <= 0) return 0.0;
        if (!quote_pair_is_usable(korean_ex, foreign_ex, korean_price, foreign_price)) return 0.0;
        best_foreign_bid = foreign_price.bid;
        usdt_rate = price_cache_.get_usdt_krw(korean_ex);
    }
    if (usdt_rate <= 0) {
        return 0.0;
    }

    return PremiumCalculator::calculate_entry_premium(
        korean_price.ask, best_foreign_bid, usdt_rate);
}

void ArbitrageEngine::refresh_entry_filters() {
    update_all_entries();
    update_seq_.fetch_add(1, std::memory_order_release);
    update_cv_.notify_all();
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
// fire_entry_from_cache: bitmap-guided scan of cache-hot atomics

void ArbitrageEngine::sync_entry_state_bits(size_t idx, bool qualifies, bool signal_fired) {
    if (idx >= MAX_CACHED_SYMBOLS) return;
    entry_candidate_bits_.set(idx, qualifies && !signal_fired);
    entry_signal_fired_bits_.set(idx, signal_fired);
}

void ArbitrageEngine::update_symbol_entry(size_t idx) {
    if (idx >= monitored_symbols_.size()) return;
    auto& cache = entry_cache_[idx];
    auto clear_state = [&]() {
        const bool was_qualified = cache.qualified.load(std::memory_order_relaxed);
        const bool was_fired = cache.signal_fired.load(std::memory_order_relaxed);
        if (was_qualified || was_fired) {
            cache.qualified.store(false, std::memory_order_release);
            cache.signal_fired.store(false, std::memory_order_release);
        }
        sync_entry_state_bits(idx, false, false);
    };

    if (exchange_pairs_.empty()) {
        clear_state();
        return;
    }

    const auto& symbol = monitored_symbols_[idx];
    const auto& foreign_symbol = foreign_symbols_[idx];
    const std::string base(symbol.get_base());

    // Pick the pair with the best projected net profit after fees and transfer cost.
    Exchange best_korean_ex = exchange_pairs_[0].korean;
    Exchange best_foreign_ex = exchange_pairs_[0].foreign;
    PriceCache::PriceData best_korean_price{};
    PriceCache::PriceData best_foreign_price{};
    double best_net_profit = -1e18;
    double best_net_edge = -1e18;
    double best_usdt_rate = 0.0;

    for (const auto& pair : exchange_pairs_) {
        if (!pair.entry_enabled) continue;
        if (!price_cache_.is_transfer_route_available(pair.korean, pair.foreign, base)) continue;

        auto korean_price = price_cache_.get_price(pair.korean, symbol);
        if (!korean_price.valid || korean_price.ask <= 0) continue;

        auto foreign_price = price_cache_.get_price(pair.foreign, foreign_symbol);
        if (!foreign_price.valid || foreign_price.bid <= 0) continue;

        if (!quote_pair_is_usable(pair.korean, pair.foreign, korean_price, foreign_price)) continue;

        double rate = price_cache_.get_usdt_krw(pair.korean);
        if (rate <= 0) continue;

        double withdraw_fee = price_cache_.get_withdraw_fee(pair.korean, pair.foreign, base);
        auto relay_metrics = PremiumCalculator::calculate_relay_metrics(
            korean_price.ask,
            korean_price.ask_qty,
            foreign_price.bid,
            foreign_price.bid_qty,
            rate,
            TradingConfig::get_korean_fee_rate(pair.korean),
            TradingConfig::get_foreign_fee_rate(pair.foreign),
            withdraw_fee);
        if (relay_metrics.net_profit_krw > best_net_profit ||
            (relay_metrics.net_profit_krw == best_net_profit &&
             relay_metrics.net_edge_pct > best_net_edge)) {
            best_net_profit = relay_metrics.net_profit_krw;
            best_net_edge = relay_metrics.net_edge_pct;
            best_korean_ex = pair.korean;
            best_foreign_ex = pair.foreign;
            best_korean_price = korean_price;
            best_foreign_price = foreign_price;
            best_usdt_rate = rate;
        }
    }

    if (best_usdt_rate <= 0.0) {
        clear_state();
        return;
    }

    double usdt_rate = best_usdt_rate;

    double premium = PremiumCalculator::calculate_entry_premium(
        best_korean_price.ask, best_foreign_price.bid, usdt_rate);
    double withdraw_fee = price_cache_.get_withdraw_fee(best_korean_ex, best_foreign_ex, std::string(symbol.get_base()));
    auto relay_metrics = PremiumCalculator::calculate_relay_metrics(
        best_korean_price.ask,
        best_korean_price.ask_qty,
        best_foreign_price.bid,
        best_foreign_price.bid_qty,
        usdt_rate,
        TradingConfig::get_korean_fee_rate(best_korean_ex),
        TradingConfig::get_foreign_fee_rate(best_foreign_ex),
        withdraw_fee);

    bool qualifies = TradingConfig::entry_gate_passes(
        relay_metrics.both_can_fill_target,
        relay_metrics.match_qty,
        relay_metrics.net_edge_pct,
        relay_metrics.net_profit_krw);

    // Update cached values
    cache.entry_premium.store(premium, std::memory_order_relaxed);
    cache.korean_ask.store(best_korean_price.ask, std::memory_order_relaxed);
    cache.korean_ask_qty.store(best_korean_price.ask_qty, std::memory_order_relaxed);
    cache.foreign_bid.store(best_foreign_price.bid, std::memory_order_relaxed);
    cache.foreign_bid_qty.store(best_foreign_price.bid_qty, std::memory_order_relaxed);
    cache.best_korean_exchange.store(static_cast<uint8_t>(best_korean_ex), std::memory_order_relaxed);
    cache.best_foreign_exchange.store(static_cast<uint8_t>(best_foreign_ex), std::memory_order_relaxed);
    cache.match_qty.store(relay_metrics.match_qty, std::memory_order_relaxed);
    cache.target_coin_qty.store(relay_metrics.target_coin_qty, std::memory_order_relaxed);
    cache.max_tradable_usdt_at_best.store(relay_metrics.max_tradable_usdt_at_best, std::memory_order_relaxed);
    cache.gross_edge_pct.store(relay_metrics.gross_edge_pct, std::memory_order_relaxed);
    cache.net_edge_pct.store(relay_metrics.net_edge_pct, std::memory_order_relaxed);
    cache.net_profit_krw.store(relay_metrics.net_profit_krw, std::memory_order_relaxed);
    cache.both_can_fill_target.store(relay_metrics.both_can_fill_target, std::memory_order_relaxed);
    cache.usdt_rate.store(usdt_rate, std::memory_order_relaxed);

    bool was_qualified = cache.qualified.load(std::memory_order_relaxed);
    cache.qualified.store(qualifies, std::memory_order_release);

    // Reset signal_fired when qualification state changes (re-enable signal)
    bool signal_fired = cache.signal_fired.load(std::memory_order_relaxed);
    if (was_qualified != qualifies) {
        cache.signal_fired.store(false, std::memory_order_release);
        signal_fired = false;
    }
    sync_entry_state_bits(idx, qualifies, signal_fired);
}

void ArbitrageEngine::update_all_entries() {
    for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
        update_symbol_entry(i);
    }
}

void ArbitrageEngine::fire_entry_from_cache() {
    if (exchange_pairs_.empty()) return;
    if (entry_suppressed_.load(std::memory_order_acquire)) return;
    const size_t symbol_count = monitored_symbols_.size();
    if (symbol_count == 0) return;

    // 0: no position, 1: full position, 2: partial position (eligible for top-up)
    std::array<uint8_t, MAX_CACHED_SYMBOLS> position_state{};
    int active_positions = 0;
    position_tracker_.for_each_active_position([&](const Position& pos) {
        ++active_positions;
        auto it = korean_symbol_index_.find(pos.symbol);
        if (it == korean_symbol_index_.end()) return;
        const size_t idx = it->second;
        if (idx >= symbol_count || idx >= MAX_CACHED_SYMBOLS) return;

        const double actual_usd = pos.foreign_amount * pos.foreign_entry_price;
        const bool partial = actual_usd < pos.position_size_usd * 0.95;
        position_state[idx] = partial ? 2 : 1;
    });
    int pending_new_signals = 0;
    entry_signal_fired_bits_.for_each_set(symbol_count, [&](size_t idx) {
        if (position_state[idx] == 0) {
            ++pending_new_signals;
        }
    });

    auto emit_signal = [&](size_t idx) {
        auto& c = entry_cache_[idx];
        ArbitrageSignal signal;
        signal.trace_id = LatencyProbe::instance().next_trace_id();
        signal.trace_start_ns = LatencyProbe::instance().capture_now_ns();
        signal.trace_symbol = LatencyProbe::format_symbol_fast(monitored_symbols_[idx]);
        signal.symbol = monitored_symbols_[idx];
        signal.korean_exchange = static_cast<Exchange>(c.best_korean_exchange.load(std::memory_order_relaxed));
        signal.foreign_exchange = static_cast<Exchange>(c.best_foreign_exchange.load(std::memory_order_relaxed));
        signal.premium = c.entry_premium.load(std::memory_order_relaxed);
        signal.korean_ask = c.korean_ask.load(std::memory_order_relaxed);
        signal.korean_ask_qty = c.korean_ask_qty.load(std::memory_order_relaxed);
        signal.foreign_bid = c.foreign_bid.load(std::memory_order_relaxed);
        signal.foreign_bid_qty = c.foreign_bid_qty.load(std::memory_order_relaxed);
        signal.match_qty = c.match_qty.load(std::memory_order_relaxed);
        signal.target_coin_qty = c.target_coin_qty.load(std::memory_order_relaxed);
        signal.max_tradable_usdt_at_best = c.max_tradable_usdt_at_best.load(std::memory_order_relaxed);
        signal.gross_edge_pct = c.gross_edge_pct.load(std::memory_order_relaxed);
        signal.net_edge_pct = c.net_edge_pct.load(std::memory_order_relaxed);
        signal.net_profit_krw = c.net_profit_krw.load(std::memory_order_relaxed);
        signal.both_can_fill_target = c.both_can_fill_target.load(std::memory_order_relaxed);
        signal.usdt_krw_rate = c.usdt_rate.load(std::memory_order_relaxed);
        LatencyProbe::instance().record_at_ns(
            signal.trace_id,
            signal.trace_symbol,
            LatencyStage::SignalDetected,
            signal.trace_start_ns,
            signal.trace_start_ns,
            0,
            0,
            signal.net_edge_pct,
            signal.max_tradable_usdt_at_best);
        if (on_entry_signal_) on_entry_signal_(signal);
        entry_signals_.try_push(signal);
    };
    const auto selection = select_entry_candidates<MAX_CACHED_SYMBOLS>(
        symbol_count,
        TradingConfig::MAX_POSITIONS,
        entry_candidate_bits_,
        entry_signal_fired_bits_,
        position_state,
        [&](size_t idx) {
            return -entry_cache_[idx].net_edge_pct.load(std::memory_order_relaxed);
        },
        [&](size_t idx) {
            return entry_cache_[idx].qualified.load(std::memory_order_acquire);
        });

    for (size_t n = 0; n < selection.count; ++n) {
        const size_t idx = selection.indices[n];
        const bool partial = position_state[idx] == 2;

        if (!entry_cache_[idx].qualified.load(std::memory_order_acquire)) continue;
        if (!partial && entry_cache_[idx].signal_fired.load(std::memory_order_acquire)) continue;

        if (!partial) {
            entry_cache_[idx].signal_fired.store(true, std::memory_order_release);
            sync_entry_state_bits(idx, true, true);
        }
        emit_signal(idx);
    }
}

void ArbitrageEngine::check_exit_conditions() {
    // Fast check: no position = no exit check needed
    if (!position_tracker_.has_any_position()) return;

    position_tracker_.for_each_active_position([this](const Position& pos) {
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
        if (!korean_price.valid || korean_price.bid <= 0) return;

        auto foreign_price = price_cache_.get_price(pos.foreign_exchange, *foreign_symbol_ptr);
        if (!foreign_price.valid || foreign_price.ask <= 0) return;
        if (!quote_pair_is_usable(pos.korean_exchange, pos.foreign_exchange, korean_price, foreign_price, /*is_exit=*/true)) return;

        double usdt_rate = price_cache_.get_usdt_krw(pos.korean_exchange);
        if (usdt_rate <= 0) return;  // No real USDT/KRW rate

        // Calculate exit premium
        double premium = PremiumCalculator::calculate_exit_premium(
            korean_price.bid, foreign_price.ask, usdt_rate);

        // Dynamic exit threshold with a hard floor (+0.10% by default)
        double dynamic_exit = std::max(
            pos.entry_premium + TradingConfig::DYNAMIC_EXIT_SPREAD,
            TradingConfig::EXIT_PREMIUM_THRESHOLD);
        if (premium < dynamic_exit) return;

        // Generate signal
        ExitSignal signal;
        signal.trace_id = LatencyProbe::instance().next_trace_id();
        signal.trace_start_ns = LatencyProbe::instance().capture_now_ns();
        signal.trace_symbol = LatencyProbe::format_symbol_fast(pos.symbol);
        signal.symbol = pos.symbol;
        signal.korean_exchange = pos.korean_exchange;
        signal.foreign_exchange = pos.foreign_exchange;
        signal.premium = premium;
        signal.korean_bid = korean_price.bid;
        signal.foreign_ask = foreign_price.ask;
        signal.usdt_krw_rate = usdt_rate;

        if (on_exit_signal_) {
            on_exit_signal_(signal);
        }
        exit_signals_.try_push(signal);
    });
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
    if (!quote_pair_is_usable(pos.korean_exchange, pos.foreign_exchange, korean_price, foreign_price, /*is_exit=*/true)) return;

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
    signal.trace_id = LatencyProbe::instance().next_trace_id();
    signal.trace_start_ns = LatencyProbe::instance().capture_now_ns();
    signal.trace_symbol = LatencyProbe::format_symbol_fast(korean_symbol);
    signal.symbol = korean_symbol;
    signal.korean_exchange = pos.korean_exchange;
    signal.foreign_exchange = pos.foreign_exchange;
    signal.premium = premium;
    signal.korean_bid = korean_price.bid;
    signal.foreign_ask = foreign_price.ask;
    signal.usdt_krw_rate = usdt_rate;

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
    std::vector<double> korean_ask_qtys;
    std::vector<double> foreign_bids;
    std::vector<double> foreign_bid_qtys;
    // Exit arrays (korean_bid / foreign_ask)
    std::vector<double> korean_bids;
    std::vector<double> korean_bid_qtys;
    std::vector<double> foreign_asks;
    std::vector<double> foreign_ask_qtys;

    std::vector<double> usdt_rates;
    std::vector<uint64_t> korean_timestamps;
    std::vector<uint64_t> foreign_timestamps;
    std::vector<size_t> symbol_indices;
    std::vector<Exchange> best_foreign_exchanges;

    korean_asks.reserve(n);
    korean_ask_qtys.reserve(n);
    foreign_bids.reserve(n);
    foreign_bid_qtys.reserve(n);
    korean_bids.reserve(n);
    korean_bid_qtys.reserve(n);
    foreign_asks.reserve(n);
    foreign_ask_qtys.reserve(n);
    usdt_rates.reserve(n);
    korean_timestamps.reserve(n);
    foreign_timestamps.reserve(n);
    symbol_indices.reserve(n);
    best_foreign_exchanges.reserve(n);

    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    std::vector<Exchange> best_korean_exchanges;
    best_korean_exchanges.reserve(n);

    // Phase 1: Collect valid prices directly into SoA arrays (single pass)
    // For each symbol, pick the pair with the best projected net profit across exchange pairs.
    for (size_t i = 0; i < n; ++i) {
        const auto& symbol = monitored_symbols_[i];
        const auto& foreign_symbol = foreign_symbols_[i];
        const std::string base(symbol.get_base());

        PriceCache::PriceData best_kr{};
        PriceCache::PriceData best_fr{};
        double best_rate = 0.0;
        double best_net_profit = -1e18;
        double best_net_edge = -1e18;
        Exchange best_k_ex = Exchange::Bithumb;
        Exchange best_f_ex = Exchange::Bybit;

        for (const auto& pair : exchange_pairs_) {
            if (!price_cache_.is_transfer_route_available(pair.korean, pair.foreign, base)) continue;
            auto korean_price = price_cache_.get_price(pair.korean, symbol);
            if (!korean_price.valid || korean_price.ask <= 0) continue;

            auto foreign_price = price_cache_.get_price(pair.foreign, foreign_symbol);
            if (!foreign_price.valid || foreign_price.bid <= 0) continue;

            double usdt_rate = price_cache_.get_usdt_krw(pair.korean);
            if (usdt_rate <= 0) continue;

            double withdraw_fee = price_cache_.get_withdraw_fee(pair.korean, pair.foreign, base);
            auto relay_metrics = PremiumCalculator::calculate_relay_metrics(
                korean_price.ask,
                korean_price.ask_qty,
                foreign_price.bid,
                foreign_price.bid_qty,
                usdt_rate,
                TradingConfig::get_korean_fee_rate(pair.korean),
                TradingConfig::get_foreign_fee_rate(pair.foreign),
                withdraw_fee);
            if (relay_metrics.net_profit_krw > best_net_profit ||
                (relay_metrics.net_profit_krw == best_net_profit &&
                 relay_metrics.net_edge_pct > best_net_edge)) {
                best_net_profit = relay_metrics.net_profit_krw;
                best_net_edge = relay_metrics.net_edge_pct;
                best_kr = korean_price;
                best_fr = foreign_price;
                best_rate = usdt_rate;
                best_k_ex = pair.korean;
                best_f_ex = pair.foreign;
            }
        }

        if (best_rate > 0.0) {
            korean_asks.push_back(best_kr.ask);
            korean_ask_qtys.push_back(best_kr.ask_qty);
            foreign_bids.push_back(best_fr.bid);
            foreign_bid_qtys.push_back(best_fr.bid_qty);
            korean_bids.push_back(best_kr.bid);
            korean_bid_qtys.push_back(best_kr.bid_qty);
            foreign_asks.push_back(best_fr.ask);
            foreign_ask_qtys.push_back(best_fr.ask_qty);
            usdt_rates.push_back(best_rate);
            korean_timestamps.push_back(best_kr.timestamp);
            foreign_timestamps.push_back(best_fr.timestamp);
            symbol_indices.push_back(i);
            best_korean_exchanges.push_back(best_k_ex);
            best_foreign_exchanges.push_back(best_f_ex);
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
        info.korean_bid_qty = korean_bid_qtys[i];
        info.korean_ask_qty = korean_ask_qtys[i];
        info.foreign_bid = foreign_bids[i];
        info.foreign_ask = foreign_asks[i];
        info.foreign_bid_qty = foreign_bid_qtys[i];
        info.foreign_ask_qty = foreign_ask_qtys[i];
        info.korean_price = korean_asks[i];
        info.foreign_price = foreign_bids[i];
        info.usdt_rate = usdt_rates[i];
        info.entry_premium = entry_premiums[i];
        info.exit_premium = exit_premiums[i];
        info.premium_spread = entry_premiums[i] - exit_premiums[i];
        info.premium = entry_premiums[i];  // Backward-compatible alias
        double withdraw_fee = price_cache_.get_withdraw_fee(
            best_korean_exchanges[i], best_foreign_exchanges[i],
            std::string(monitored_symbols_[symbol_indices[i]].get_base()));
        auto relay_metrics = PremiumCalculator::calculate_relay_metrics(
            korean_asks[i],
            korean_ask_qtys[i],
            foreign_bids[i],
            foreign_bid_qtys[i],
            usdt_rates[i],
            TradingConfig::get_korean_fee_rate(best_korean_exchanges[i]),
            TradingConfig::get_foreign_fee_rate(best_foreign_exchanges[i]),
            withdraw_fee);
        info.match_qty = relay_metrics.match_qty;
        info.target_coin_qty = relay_metrics.target_coin_qty;
        info.max_tradable_usdt_at_best = relay_metrics.max_tradable_usdt_at_best;
        info.bithumb_top_krw = relay_metrics.bithumb_top_krw;
        info.bithumb_top_usdt = relay_metrics.bithumb_top_usdt;
        info.bybit_top_usdt = relay_metrics.bybit_top_usdt;
        info.bybit_top_krw = relay_metrics.bybit_top_krw;
        info.gross_edge_pct = relay_metrics.gross_edge_pct;
        info.net_edge_pct = relay_metrics.net_edge_pct;
        info.bithumb_total_fee_krw = relay_metrics.bithumb_total_fee_krw;
        info.bybit_total_fee_usdt = relay_metrics.bybit_total_fee_usdt;
        info.bybit_total_fee_krw = relay_metrics.bybit_total_fee_krw;
        info.withdraw_fee_coins = relay_metrics.withdraw_fee_coins;
        info.withdraw_fee_krw = relay_metrics.withdraw_fee_krw;
        info.total_fee_krw = relay_metrics.total_fee_krw;
        info.net_profit_krw = relay_metrics.net_profit_krw;
        info.both_can_fill_target = relay_metrics.both_can_fill_target;
        const uint64_t newest_ts = std::max(korean_timestamps[i], foreign_timestamps[i]);
        info.age_ms = newest_ts > 0 && now_ms > newest_ts ? (now_ms - newest_ts) : 0;
        PriceCache::PriceData best_korean_price{};
        best_korean_price.valid = true;
        best_korean_price.bid = korean_bids[i];
        best_korean_price.ask = korean_asks[i];
        best_korean_price.bid_qty = korean_bid_qtys[i];
        best_korean_price.ask_qty = korean_ask_qtys[i];
        best_korean_price.timestamp = korean_timestamps[i];
        PriceCache::PriceData best_foreign_price{};
        best_foreign_price.valid = true;
        best_foreign_price.bid = foreign_bids[i];
        best_foreign_price.ask = foreign_asks[i];
        best_foreign_price.bid_qty = foreign_bid_qtys[i];
        best_foreign_price.ask_qty = foreign_ask_qtys[i];
        best_foreign_price.timestamp = foreign_timestamps[i];
        info.quote_usable = quote_pair_is_usable(
            best_korean_exchanges[i],
            best_foreign_exchanges[i],
            best_korean_price,
            best_foreign_price);
        info.entry_signal = TradingConfig::entry_gate_passes(
            info.quote_usable && relay_metrics.both_can_fill_target,
            relay_metrics.match_qty,
            relay_metrics.net_edge_pct,
            relay_metrics.net_profit_krw);
        info.exit_signal = exit_premiums[i] >= TradingConfig::EXIT_PREMIUM_THRESHOLD;
        info.best_korean_exchange = best_korean_exchanges[i];
        info.best_foreign_exchange = best_foreign_exchanges[i];
        result.push_back(info);
    }

    return result;
}

std::vector<ArbitrageEngine::TransferBlockInfo> ArbitrageEngine::get_transfer_blocked_symbols() const {
    std::vector<TransferBlockInfo> result;
    result.reserve(monitored_symbols_.size());

    for (const auto& symbol : monitored_symbols_) {
        const std::string base(symbol.get_base());
        bool any_available = false;

        size_t missing_count = 0;
        size_t withdraw_blocked_count = 0;
        size_t deposit_blocked_count = 0;
        size_t no_shared_count = 0;

        std::optional<TransferBlockInfo> missing_info;
        std::optional<TransferBlockInfo> withdraw_info;
        std::optional<TransferBlockInfo> deposit_info;
        std::optional<TransferBlockInfo> no_shared_info;

        for (const auto& pair : exchange_pairs_) {
            auto route = price_cache_.get_transfer_route(pair.korean, pair.foreign, base);
            if (!route.has_value()) {
                ++missing_count;
                if (!missing_info.has_value()) {
                    missing_info = TransferBlockInfo{symbol, pair.korean, pair.foreign, "route data missing"};
                }
                continue;
            }

            if (route->available) {
                any_available = true;
                break;
            }

            if (!route->withdraw_open) {
                ++withdraw_blocked_count;
                if (!withdraw_info.has_value()) {
                    withdraw_info = TransferBlockInfo{symbol, pair.korean, pair.foreign, "KR withdraw off"};
                }
                continue;
            }

            if (!route->deposit_open) {
                ++deposit_blocked_count;
                if (!deposit_info.has_value()) {
                    deposit_info = TransferBlockInfo{symbol, pair.korean, pair.foreign, "Foreign deposit off"};
                }
                continue;
            }

            ++no_shared_count;
            if (!no_shared_info.has_value()) {
                no_shared_info = TransferBlockInfo{symbol, pair.korean, pair.foreign, "no shared network"};
            }
        }

        if (any_available) {
            continue;
        }

        const size_t pair_count = exchange_pairs_.size();
        if (pair_count == 0) {
            continue;
        }

        if (withdraw_blocked_count == pair_count && withdraw_info.has_value()) {
            result.push_back(*withdraw_info);
        } else if (deposit_blocked_count == pair_count && deposit_info.has_value()) {
            result.push_back(*deposit_info);
        } else if (no_shared_count == pair_count && no_shared_info.has_value()) {
            result.push_back(*no_shared_info);
        } else if (missing_count == pair_count && missing_info.has_value()) {
            result.push_back(*missing_info);
        } else if (withdraw_info.has_value()) {
            TransferBlockInfo info = *withdraw_info;
            info.reason = "no live route (KR withdraw off mixed)";
            result.push_back(std::move(info));
        } else if (deposit_info.has_value()) {
            TransferBlockInfo info = *deposit_info;
            info.reason = "no live route (Foreign deposit off mixed)";
            result.push_back(std::move(info));
        } else if (no_shared_info.has_value()) {
            TransferBlockInfo info = *no_shared_info;
            info.reason = "no live route (shared net none)";
            result.push_back(std::move(info));
        } else if (missing_info.has_value()) {
            result.push_back(*missing_info);
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.symbol.to_string() < b.symbol.to_string();
    });
    return result;
}

void ArbitrageEngine::export_to_json(const std::string& path) const {
    auto premiums = get_all_premiums();
    write_premiums_json_file(path, running_.load(), premiums);
}

// Async JSON Export Implementation
void ArbitrageEngine::export_to_json_async(const std::string& path) {
    // Snapshot data before launching detached writer to avoid touching `this`
    // after ArbitrageEngine lifetime ends.
    auto premiums = get_all_premiums();
    const bool connected = running_.load();
    std::thread([path, connected, premiums = std::move(premiums)]() mutable {
        write_premiums_json_file(path, connected, premiums);
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
