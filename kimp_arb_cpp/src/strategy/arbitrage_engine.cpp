#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <algorithm>
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
    size_t korean_hash = symbol.hash();

    // Check if already exists using O(1) lookup
    if (korean_symbol_index_.count(korean_hash)) {
        return;  // Already added
    }

    // Add to vectors
    size_t idx = monitored_symbols_.size();
    monitored_symbols_.push_back(symbol);

    // Pre-compute foreign symbol (BTC/KRW -> BTC/USDT)
    SymbolId foreign_symbol(symbol.get_base(), "USDT");
    foreign_symbols_.push_back(foreign_symbol);

    // Populate O(1) lookup maps
    korean_symbol_index_[korean_hash] = idx;
    foreign_symbol_index_[foreign_symbol.hash()] = idx;
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

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    Logger::info("ArbitrageEngine stopped");
}

void ArbitrageEngine::on_ticker_update(const Ticker& ticker) {
    price_cache_.update(ticker.exchange, ticker.symbol, ticker.bid, ticker.ask, ticker.last);

    // Update funding rate if available (futures exchanges)
    if (ticker.funding_rate != 0.0 || ticker.next_funding_time != 0) {
        price_cache_.update_funding(ticker.exchange, ticker.symbol,
            ticker.funding_rate, ticker.funding_interval_hours, ticker.next_funding_time);
    }

    // Update USDT price if this is USDT/KRW (fast char-based check)
    if (ticker.symbol.is_usdt_krw()) {
        on_usdt_update(ticker.exchange, ticker.last);
        // USDT rate change affects ALL symbols - check all
        check_entry_opportunities();
        check_exit_conditions();
        return;
    }

    // EVENT-DRIVEN: Immediately check premium when price updates (0ms latency)
    // Only check the specific symbol that was updated
    check_symbol_opportunity(ticker.exchange, ticker.symbol);
}

void ArbitrageEngine::on_usdt_update(Exchange ex, double price) {
    Logger::info("USDT/KRW update from {}: {:.2f}", ex == Exchange::Bithumb ? "Bithumb" : "Upbit", price);
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

    auto it = korean_symbol_index_.find(symbol.hash());
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
    if (!position_tracker_.can_open_position()) {
        return;
    }

    // Collect all candidates that meet entry conditions
    struct EntryCandidate {
        ArbitrageSignal signal;
        int funding_interval_hours;
    };
    std::vector<EntryCandidate> candidates;
    candidates.reserve(16);  // Pre-allocate for typical case

    // Use index-based iteration to access pre-computed foreign symbols
    for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
        const auto& symbol = monitored_symbols_[i];
        const auto& foreign_symbol = foreign_symbols_[i];  // Pre-computed, no allocation

        if (position_tracker_.has_position(symbol)) {
            continue;
        }

        for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
            // Get prices (batch - reduces lock contention)
            auto korean_price = price_cache_.get_price(korean_ex, symbol);
            if (!korean_price.valid || korean_price.ask <= 0) continue;

            auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
            if (!foreign_price.valid || foreign_price.bid <= 0) continue;

            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

            // Calculate premium
            double premium = PremiumCalculator::calculate_entry_premium(
                korean_price.ask, foreign_price.bid, usdt_rate);

            // Fast path: skip if premium too high
            if (premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) continue;

            // Get funding rate only when premium condition met
            auto* foreign_exchange = exchanges_[static_cast<size_t>(foreign_ex)].get();
            double funding_rate = foreign_exchange ? foreign_exchange->get_funding_rate(foreign_symbol) : 0.0;

            if (funding_rate < 0.0) continue;  // Skip negative funding

            // Add to candidates
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

            candidates.push_back({signal, foreign_price.funding_interval_hours});
        }
    }

    // No candidates found
    if (candidates.empty()) {
        return;
    }

    // Sort by funding interval (descending): 8h > 4h > 2h > 1h
    // For same interval, prefer lower (more negative) premium
    std::sort(candidates.begin(), candidates.end(),
        [](const EntryCandidate& a, const EntryCandidate& b) {
            if (a.funding_interval_hours != b.funding_interval_hours) {
                return a.funding_interval_hours > b.funding_interval_hours;  // Higher interval first
            }
            return a.signal.premium < b.signal.premium;  // Lower premium (more negative) first
        });

    // Select the best candidate (highest funding interval)
    const auto& best = candidates[0];
    Logger::info("Entry signal: {} premium={:.2f}% funding_interval={}h (selected from {} candidates)",
                 best.signal.symbol.to_string(), best.signal.premium,
                 best.funding_interval_hours, candidates.size());

    if (on_entry_signal_) {
        on_entry_signal_(best.signal);
    }
    entry_signals_.try_push(best.signal);
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

        auto it = korean_symbol_index_.find(pos.symbol.hash());
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

        // Fast path: skip if premium too low
        if (premium < TradingConfig::EXIT_PREMIUM_THRESHOLD) continue;

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

        Logger::info("Exit signal: {} premium={:.2f}%", pos.symbol.to_string(), premium);

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
        auto it = korean_symbol_index_.find(updated_symbol.hash());
        if (it != korean_symbol_index_.end()) {
            symbol_idx = it->second;
            korean_symbol = updated_symbol;
        }
    } else {
        // Foreign update: O(1) lookup in foreign_symbol_index_
        auto it = foreign_symbol_index_.find(updated_symbol.hash());
        if (it != foreign_symbol_index_.end()) {
            symbol_idx = it->second;
            korean_symbol = monitored_symbols_[symbol_idx];
        }
    }

    if (symbol_idx == SIZE_MAX) return;  // Symbol not monitored

    const auto& foreign_symbol = foreign_symbols_[symbol_idx];

    // Check ENTRY opportunity
    // NOTE: Only trigger immediate entry for 8h funding symbols (most stable)
    // Lower funding intervals (4h, 2h, 1h) are handled by check_entry_opportunities()
    // with priority sorting (8h > 4h > 2h > 1h)
    if (position_tracker_.can_open_position() && !position_tracker_.has_position(korean_symbol)) {
        for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
            auto korean_price = price_cache_.get_price(korean_ex, korean_symbol);
            if (!korean_price.valid || korean_price.ask <= 0) continue;

            auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
            if (!foreign_price.valid || foreign_price.bid <= 0) continue;

            // Only immediate entry for 8h funding (most stable)
            // Let check_entry_opportunities() handle 4h/2h/1h with priority sorting
            if (foreign_price.funding_interval_hours < 8) continue;

            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) usdt_rate = TradingConfig::DEFAULT_USDT_KRW;

            double premium = PremiumCalculator::calculate_entry_premium(
                korean_price.ask, foreign_price.bid, usdt_rate);

            if (premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) continue;

            auto* foreign_exchange = exchanges_[static_cast<size_t>(foreign_ex)].get();
            double funding_rate = foreign_exchange ? foreign_exchange->get_funding_rate(foreign_symbol) : 0.0;
            if (funding_rate < 0.0) continue;

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

            Logger::info("Entry signal (8h): {} premium={:.2f}%", korean_symbol.to_string(), premium);

            if (on_entry_signal_) {
                on_entry_signal_(signal);
            }
            entry_signals_.try_push(signal);
            return;
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

        if (premium < TradingConfig::EXIT_PREMIUM_THRESHOLD) return;

        ExitSignal signal;
        signal.symbol = korean_symbol;
        signal.korean_exchange = pos->korean_exchange;
        signal.foreign_exchange = pos->foreign_exchange;
        signal.premium = premium;
        signal.korean_bid = korean_price.bid;
        signal.foreign_ask = foreign_price.ask;
        signal.usdt_krw_rate = usdt_rate;
        signal.timestamp = std::chrono::steady_clock::now();

        Logger::info("Exit signal: {} premium={:.2f}%", korean_symbol.to_string(), premium);

        if (on_exit_signal_) {
            on_exit_signal_(signal);
        }
        exit_signals_.try_push(signal);
    }
}

std::vector<ArbitrageEngine::PremiumInfo> ArbitrageEngine::get_all_premiums() const {
    std::vector<PremiumInfo> result;
    result.reserve(monitored_symbols_.size());

    // Use index-based iteration to access pre-computed foreign symbols
    for (size_t i = 0; i < monitored_symbols_.size(); ++i) {
        const auto& symbol = monitored_symbols_[i];
        const auto& foreign_symbol = foreign_symbols_[i];  // Pre-computed, no allocation

        for (const auto& [korean_ex, foreign_ex] : exchange_pairs_) {
            // Get Korean price
            auto korean_price = price_cache_.get_price(korean_ex, symbol);
            if (!korean_price.valid || korean_price.last <= 0) {
                continue;
            }

            // Get foreign price (using cached foreign symbol)
            auto foreign_price = price_cache_.get_price(foreign_ex, foreign_symbol);
            if (!foreign_price.valid || foreign_price.last <= 0) {
                continue;
            }

            // Get USDT rate
            double usdt_rate = price_cache_.get_usdt_krw(korean_ex);
            if (usdt_rate <= 0) {
                usdt_rate = TradingConfig::DEFAULT_USDT_KRW;
            }

            // Calculate DISPLAY premium using LAST prices (more accurate for monitoring)
            double foreign_krw = foreign_price.last * usdt_rate;
            double display_premium = ((korean_price.last - foreign_krw) / foreign_krw) * 100.0;

            // Check signals with DISPLAY premium (LAST prices - what user sees)
            bool entry_signal = display_premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD;
            bool exit_signal = display_premium >= TradingConfig::EXIT_PREMIUM_THRESHOLD;

            PremiumInfo info;
            info.symbol = symbol;
            info.korean_price = korean_price.last;   // Display LAST price
            info.foreign_price = foreign_price.last; // Display LAST price
            info.usdt_rate = usdt_rate;
            info.premium = display_premium;          // Display premium based on LAST prices
            info.funding_rate = foreign_price.funding_rate;
            info.funding_interval_hours = foreign_price.funding_interval_hours;
            info.next_funding_time = foreign_price.next_funding_time;
            info.entry_signal = entry_signal;
            info.exit_signal = exit_signal;

            result.push_back(info);
            break; // Only first exchange pair per symbol
        }
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
