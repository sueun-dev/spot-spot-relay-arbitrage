#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/simd_premium.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <fmt/format.h>

namespace kimp::strategy {

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
    price_cache_.update(ticker.exchange, ticker.symbol, ticker.bid, ticker.ask, ticker.last);

    // Update funding rate for futures exchanges (cache even if 0)
    if (kimp::is_foreign_exchange(ticker.exchange)) {
        if (std::isfinite(ticker.funding_rate) || ticker.next_funding_time != 0) {
            price_cache_.update_funding(ticker.exchange, ticker.symbol,
                ticker.funding_rate, ticker.funding_interval_hours, ticker.next_funding_time);
        }
    }

    // Update USDT price if this is USDT/KRW (fast char-based check)
    if (ticker.symbol.is_usdt_krw()) {
        on_usdt_update(ticker.exchange, ticker.last);
        // USDT rate change affects ALL symbols - check all
        check_entry_opportunities();
        check_exit_conditions();
        update_seq_.fetch_add(1, std::memory_order_release);
        update_cv_.notify_all();
        return;
    }

    // EVENT-DRIVEN: Immediately check premium when price updates (0ms latency)
    // Only check the specific symbol that was updated
    check_symbol_opportunity(ticker.exchange, ticker.symbol);

    update_seq_.fetch_add(1, std::memory_order_release);
    update_cv_.notify_all();
}

void ArbitrageEngine::on_usdt_update(Exchange ex, double price) {
    const auto idx = static_cast<size_t>(ex);
    const double last_logged = last_usdt_log_[idx].load(std::memory_order_relaxed);
    if (std::fabs(price - last_logged) >= 1.0) {
        Logger::info("USDT/KRW update from {}: {:.2f}", ex == Exchange::Bithumb ? "Bithumb" : "Upbit", price);
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

    // Get USDT/KRW rate
    double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
    if (usdt_rate <= 0) {
        usdt_rate = TradingConfig::DEFAULT_USDT_KRW;
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

    Logger::info("ArbitrageEngine monitor loop started (backup mode - main logic is event-driven)");

    // BACKUP ONLY: Primary checking happens in on_ticker_update() (event-driven, 0ms latency)
    // This loop serves as a safety net in case events are missed
    constexpr auto backup_interval = std::chrono::milliseconds(100);

    while (running_.load(std::memory_order_relaxed)) {
        // Backup check - should rarely trigger if event-driven is working
        check_entry_opportunities();
        check_exit_conditions();

        std::this_thread::sleep_for(backup_interval);
    }

    Logger::info("ArbitrageEngine monitor loop ended");
}

void ArbitrageEngine::check_entry_opportunities() {
    // ==========================================================================
    // 병렬 포지션 관리: 조건 만족하는 모든 코인에 진입
    // - 8시간 펀딩 간격
    // - 펀딩비 양수
    // - 프리미엄 <= -0.75%
    // ==========================================================================

    if (!position_tracker_.can_open_position()) {
        return;  // 최대 포지션 수 도달
    }

    if (TradingConfig::MAX_POSITIONS == 1) {
        bool found = false;
        double best_premium = 0.0;
        ArbitrageSignal best_signal;

        for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
            const auto& symbol = monitored_symbols_[i];
            const auto& foreign_symbol = foreign_symbols_[i];

            // 이미 이 심볼에 포지션 있으면 스킵
            if (position_tracker_.has_position(symbol)) {
                continue;
            }

            for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
                auto korean_price = price_cache_.get_price(korean_ex, symbol);
                if (!korean_price.valid || korean_price.ask <= 0) continue;

                auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
                if (!foreign_price.valid || foreign_price.bid <= 0) continue;

                // FILTER 1: 8시간 펀딩 간격만
                if (foreign_price.funding_interval_hours < TradingConfig::MIN_FUNDING_INTERVAL_HOURS) {
                    continue;
                }

                // FILTER 2: 펀딩비 양수만
                double funding_rate = foreign_price.funding_rate;
                if (TradingConfig::REQUIRE_POSITIVE_FUNDING && funding_rate < 0.0) continue;

                double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
                if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

                // FILTER 3: 프리미엄 -0.75% 이하 (역프)
                double premium = PremiumCalculator::calculate_entry_premium(
                    korean_price.ask, foreign_price.bid, usdt_rate);
                if (premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) continue;

                if (!found || premium < best_premium) {
                    found = true;
                    best_premium = premium;

                    best_signal.symbol = symbol;
                    best_signal.korean_exchange = korean_ex;
                    best_signal.foreign_exchange = foreign_ex;
                    best_signal.premium = premium;
                    best_signal.korean_ask = korean_price.ask;
                    best_signal.foreign_bid = foreign_price.bid;
                    best_signal.funding_rate = funding_rate;
                    best_signal.usdt_krw_rate = usdt_rate;
                    best_signal.timestamp = std::chrono::steady_clock::now();
                }
            }
        }

        if (found) {
            if (on_entry_signal_) {
                on_entry_signal_(best_signal);
            }
            entry_signals_.try_push(best_signal);
        }
        return;
    }

    for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
        // 포지션 슬롯 여유 확인
        if (!position_tracker_.can_open_position()) {
            break;
        }

        const auto& symbol = monitored_symbols_[i];
        const auto& foreign_symbol = foreign_symbols_[i];

        // 이미 이 심볼에 포지션 있으면 스킵
        if (position_tracker_.has_position(symbol)) {
            continue;
        }

        for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
            auto korean_price = price_cache_.get_price(korean_ex, symbol);
            if (!korean_price.valid || korean_price.ask <= 0) continue;

            auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
            if (!foreign_price.valid || foreign_price.bid <= 0) continue;

            // FILTER 1: 8시간 펀딩 간격만
            if (foreign_price.funding_interval_hours < TradingConfig::MIN_FUNDING_INTERVAL_HOURS) {
                continue;
            }

            // FILTER 2: 펀딩비 양수만
            double funding_rate = foreign_price.funding_rate;
            if (TradingConfig::REQUIRE_POSITIVE_FUNDING && funding_rate < 0.0) continue;

            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

            // FILTER 3: 프리미엄 -0.75% 이하 (역프)
            double premium = PremiumCalculator::calculate_entry_premium(
                korean_price.ask, foreign_price.bid, usdt_rate);
            if (premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) continue;

            // 모든 조건 충족 → 진입 시그널 발생
            ArbitrageSignal signal;
            signal.symbol = symbol;
            signal.korean_exchange = korean_ex;
            signal.foreign_exchange = foreign_ex;
            signal.premium = premium;
            signal.korean_ask = korean_price.ask;
            signal.foreign_bid = foreign_price.bid;
            signal.funding_rate = funding_rate;
            signal.usdt_krw_rate = usdt_rate;
            signal.timestamp = std::chrono::steady_clock::now();

            if (on_entry_signal_) {
                on_entry_signal_(signal);
            }
            entry_signals_.try_push(signal);
            break;  // 이 심볼은 처리 완료, 다음 심볼로
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

        double usdt_rate = price_cache_.get_usdt_krw(pos.korean_exchange);
        if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

        // Calculate exit premium
        double premium = PremiumCalculator::calculate_exit_premium(
            korean_price.bid, foreign_price.ask, usdt_rate);

        // Dynamic exit threshold based on position's entry premium + fees + min profit
        double dynamic_exit = pos.entry_premium + TradingConfig::DYNAMIC_EXIT_SPREAD;
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

void ArbitrageEngine::check_symbol_opportunity(Exchange updated_ex, const SymbolId& updated_symbol) {
    // O(1) hash map lookup instead of linear search
    size_t symbol_idx = SIZE_MAX;
    SymbolId korean_symbol;

    // Check if this is a Korean exchange update (BTC/KRW) or foreign (BTC/USDT)
    bool is_korean = is_korean_exchange(updated_ex);

    if (is_korean) {
        // Korean update: O(1) lookup in korean_symbol_index_
        auto it = korean_symbol_index_.find(updated_symbol);
        if (it != korean_symbol_index_.end()) {
            symbol_idx = it->second;
            korean_symbol = updated_symbol;
        }
    } else {
        // Foreign update: O(1) lookup in foreign_symbol_index_
        auto it = foreign_symbol_index_.find(updated_symbol);
        if (it != foreign_symbol_index_.end()) {
            symbol_idx = it->second;
            korean_symbol = monitored_symbols_[symbol_idx];
        }
    }

    if (symbol_idx == SIZE_MAX) return;  // Symbol not monitored

    const auto& foreign_symbol = foreign_symbols_[symbol_idx];

    // Check ENTRY opportunity (immediate entry only when multi-position is allowed)
    // NOTE: Only trigger immediate entry for 8h funding symbols (most stable)
    if (TradingConfig::MAX_POSITIONS > 1) {
        if (position_tracker_.can_open_position() && !position_tracker_.has_position(korean_symbol)) {
            for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
                auto korean_price = price_cache_.get_price(korean_ex, korean_symbol);
                if (!korean_price.valid || korean_price.ask <= 0) continue;

                auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
                if (!foreign_price.valid || foreign_price.bid <= 0) continue;

                // Only immediate entry for coins meeting funding interval filter
                if (foreign_price.funding_interval_hours < TradingConfig::MIN_FUNDING_INTERVAL_HOURS) continue;

                double funding_rate = foreign_price.funding_rate;
                if (TradingConfig::REQUIRE_POSITIVE_FUNDING && funding_rate < 0.0) continue;

                double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
                if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

                double premium = PremiumCalculator::calculate_entry_premium(
                    korean_price.ask, foreign_price.bid, usdt_rate);

                if (premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) continue;

                ArbitrageSignal signal;
                signal.symbol = korean_symbol;
                signal.korean_exchange = korean_ex;
                signal.foreign_exchange = foreign_ex;
                signal.premium = premium;
                signal.korean_ask = korean_price.ask;
                signal.foreign_bid = foreign_price.bid;
                signal.funding_rate = funding_rate;
                signal.usdt_krw_rate = usdt_rate;
                signal.timestamp = std::chrono::steady_clock::now();

                if (on_entry_signal_) {
                    on_entry_signal_(signal);
                }
                entry_signals_.try_push(signal);
                return;
            }
        }
    }

    // Check EXIT opportunity (only if we have a position for this symbol)
    if (position_tracker_.has_position(korean_symbol)) {
        const Position* pos = position_tracker_.get_position(korean_symbol);
        if (!pos) return;

        auto korean_price = price_cache_.get_price(pos->korean_exchange, korean_symbol);
        if (!korean_price.valid || korean_price.bid <= 0) return;

        auto foreign_price = price_cache_.get_price(pos->foreign_exchange, foreign_symbol);
        if (!foreign_price.valid || foreign_price.ask <= 0) return;

        double usdt_rate = price_cache_.get_usdt_krw(pos->korean_exchange);
        if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

        double premium = PremiumCalculator::calculate_exit_premium(
            korean_price.bid, foreign_price.ask, usdt_rate);

        // Dynamic exit threshold based on position's entry premium
        double dynamic_exit = pos->entry_premium + TradingConfig::DYNAMIC_EXIT_SPREAD;
        if (premium < dynamic_exit) return;

        ExitSignal signal;
        signal.symbol = korean_symbol;
        signal.korean_exchange = pos->korean_exchange;
        signal.foreign_exchange = pos->foreign_exchange;
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

            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

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
        info.korean_price = korean_asks[i];
        info.foreign_price = foreign_bids[i];
        info.usdt_rate = usdt_rates[i];
        info.premium = entry_premiums[i];
        info.funding_rate = funding_rates[i];
        info.funding_interval_hours = funding_intervals[i];
        info.next_funding_time = next_funding_times[i];
        info.entry_signal = entry_premiums[i] <= TradingConfig::ENTRY_PREMIUM_THRESHOLD;
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
            "      \"koreanPrice\": {:.2f},\n"
            "      \"foreignPrice\": {:.4f},\n"
            "      \"usdtRate\": {:.2f},\n"
            "      \"premium\": {:.4f},\n"
            "      \"fundingRate\": {:.6f},\n"
            "      \"fundingIntervalHours\": {},\n"
            "      \"nextFundingTime\": {},\n"
            "      \"signal\": {},\n"
            "      \"timestamp\": {}\n"
            "    }}",
            p.symbol.get_base(), p.symbol.get_quote(),
            p.korean_price, p.foreign_price, p.usdt_rate, p.premium,
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
