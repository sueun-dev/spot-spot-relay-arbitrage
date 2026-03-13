#include "kimp/execution/order_manager.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <ctime>
#include <cstdio>

namespace {

// ============================================================================
// Async CSV Trade Logger — zero-copy queue, background file I/O
// Hot path only: lock → push struct → notify (~1μs vs 10-15ms sync I/O)
// ============================================================================
class AsyncCsvWriter {
public:
    static AsyncCsvWriter& instance() {
        static AsyncCsvWriter inst;
        return inst;
    }

    struct EntryLog {
        std::chrono::system_clock::time_point time;
        std::string symbol;
        double split_qty;
        double korean_buy_price;
        double foreign_short_price;
        double usdt_rate;
        double held_value_usd;
        double position_target_usd;
        double premium;
    };

    struct ExitLog {
        std::chrono::system_clock::time_point time;
        std::string symbol;
        double split_qty;
        double korean_entry_price;
        double korean_exit_price;
        double foreign_entry_price;
        double foreign_exit_price;
        double pnl_krw;
        double usdt_rate;
        bool final_split;
    };

    void push_entry(EntryLog&& log) {
        {
            std::lock_guard lock(mutex_);
            entry_queue_.push_back(std::move(log));
        }
        cv_.notify_one();
    }

    void push_exit(ExitLog&& log) {
        {
            std::lock_guard lock(mutex_);
            exit_queue_.push_back(std::move(log));
        }
        cv_.notify_one();
    }

    ~AsyncCsvWriter() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

private:
    AsyncCsvWriter() : writer_thread_([this] { run(); }) {}

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<EntryLog> entry_queue_;
    std::deque<ExitLog> exit_queue_;
    bool stop_{false};
    bool dirs_created_{false};
    bool entry_header_written_{false};
    bool exit_header_written_{false};
    std::thread writer_thread_;

    static std::string format_time(std::chrono::system_clock::time_point tp) {
        auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      tp.time_since_epoch()) % 1000;
        char out[40];
        std::snprintf(out, sizeof(out), "%s.%03lld", buf,
                      static_cast<long long>(ms.count()));
        return std::string(out);
    }

    void ensure_dirs() {
        if (!dirs_created_) {
            std::filesystem::create_directories("trade_logs");
            dirs_created_ = true;
        }
    }

    void flush_entries(std::deque<EntryLog>& batch) {
        if (batch.empty()) return;
        ensure_dirs();
        const std::string path = "trade_logs/entry_splits.csv";

        if (!entry_header_written_) {
            bool need = !std::filesystem::exists(path) ||
                        std::filesystem::file_size(path) == 0;
            entry_header_written_ = !need;  // if file has content, skip header
        }

        std::ofstream out(path, std::ios::app);
        if (!out.is_open()) { batch.clear(); return; }

        if (!entry_header_written_) {
            out << "entry_time,symbol,split_qty,korean_buy_price,foreign_short_price,"
                   "usdt_rate,held_value_usd,position_target_usd,premium\n";
            entry_header_written_ = true;
        }

        for (const auto& e : batch) {
            out << format_time(e.time) << ','
                << e.symbol << ','
                << std::fixed << std::setprecision(8) << e.split_qty << ','
                << std::fixed << std::setprecision(2) << e.korean_buy_price << ','
                << std::fixed << std::setprecision(8) << e.foreign_short_price << ','
                << std::fixed << std::setprecision(2) << e.usdt_rate << ','
                << std::fixed << std::setprecision(2) << e.held_value_usd << ','
                << std::fixed << std::setprecision(2) << e.position_target_usd << ','
                << std::fixed << std::setprecision(4) << e.premium
                << '\n';
        }
        batch.clear();
    }

    void flush_exits(std::deque<ExitLog>& batch) {
        if (batch.empty()) return;
        ensure_dirs();
        const std::string path = "trade_logs/exit_splits.csv";

        if (!exit_header_written_) {
            bool need = !std::filesystem::exists(path) ||
                        std::filesystem::file_size(path) == 0;
            exit_header_written_ = !need;
        }

        std::ofstream out(path, std::ios::app);
        if (!out.is_open()) { batch.clear(); return; }

        if (!exit_header_written_) {
            out << "exit_time,symbol,split_qty,korean_entry_price,korean_exit_price,"
                   "foreign_entry_price,foreign_exit_price,pnl_krw,pnl_usd,usdt_rate,final_split\n";
            exit_header_written_ = true;
        }

        for (const auto& x : batch) {
            double pnl_usd = x.usdt_rate > 0.0 ? (x.pnl_krw / x.usdt_rate) : 0.0;
            out << format_time(x.time) << ','
                << x.symbol << ','
                << std::fixed << std::setprecision(8) << x.split_qty << ','
                << std::fixed << std::setprecision(8) << x.korean_entry_price << ','
                << std::fixed << std::setprecision(8) << x.korean_exit_price << ','
                << std::fixed << std::setprecision(6) << x.foreign_entry_price << ','
                << std::fixed << std::setprecision(6) << x.foreign_exit_price << ','
                << std::fixed << std::setprecision(2) << x.pnl_krw << ','
                << std::fixed << std::setprecision(2) << pnl_usd << ','
                << std::fixed << std::setprecision(2) << x.usdt_rate << ','
                << (x.final_split ? "1" : "0")
                << '\n';
        }
        batch.clear();
    }

    void run() {
        std::deque<EntryLog> entry_batch;
        std::deque<ExitLog> exit_batch;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_ || !entry_queue_.empty() || !exit_queue_.empty();
                });
                entry_batch.swap(entry_queue_);
                exit_batch.swap(exit_queue_);
                if (stop_ && entry_batch.empty() && exit_batch.empty()) return;
            }
            flush_entries(entry_batch);
            flush_exits(exit_batch);
        }
    }
};

void append_entry_split_log(const kimp::SymbolId& symbol,
                            double split_qty,
                            double korean_buy_price,
                            double foreign_short_price,
                            double usdt_rate,
                            double held_value_usd,
                            double position_target_usd,
                            double premium) {
    AsyncCsvWriter::instance().push_entry({
        std::chrono::system_clock::now(),
        symbol.to_string(),
        split_qty, korean_buy_price, foreign_short_price,
        usdt_rate, held_value_usd, position_target_usd, premium
    });
}

void append_exit_split_log(const kimp::Position& pos,
                           double split_qty,
                           double korean_exit_price,
                           double foreign_exit_price,
                           double pnl_krw,
                           double usdt_rate,
                           bool final_split) {
    AsyncCsvWriter::instance().push_exit({
        std::chrono::system_clock::now(),
        pos.symbol.to_string(),
        split_qty, pos.korean_entry_price, korean_exit_price,
        pos.foreign_entry_price, foreign_exit_price,
        pnl_krw, usdt_rate, final_split
    });
}

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

bool quote_pair_is_usable(const kimp::strategy::PriceCache::PriceData& korean_price,
                          const kimp::strategy::PriceCache::PriceData& foreign_price,
                          bool is_exit = false) {
    uint64_t max_age    = is_exit ? kimp::TradingConfig::MAX_QUOTE_AGE_MS_EXIT    : kimp::TradingConfig::MAX_QUOTE_AGE_MS;
    uint64_t max_desync = is_exit ? kimp::TradingConfig::MAX_QUOTE_DESYNC_MS_EXIT : kimp::TradingConfig::MAX_QUOTE_DESYNC_MS;
    double   max_kr_sp  = is_exit ? kimp::TradingConfig::MAX_KOREAN_SPREAD_PCT_EXIT  : kimp::TradingConfig::MAX_KOREAN_SPREAD_PCT;
    double   max_fr_sp  = is_exit ? kimp::TradingConfig::MAX_FOREIGN_SPREAD_PCT_EXIT : kimp::TradingConfig::MAX_FOREIGN_SPREAD_PCT;

    uint64_t now_ms = steady_now_ms();
    if (!korean_price.valid || korean_price.timestamp == 0 ||
        !foreign_price.valid || foreign_price.timestamp == 0) return false;
    if ((now_ms - korean_price.timestamp) > max_age || (now_ms - foreign_price.timestamp) > max_age) return false;

    uint64_t ts_diff = korean_price.timestamp >= foreign_price.timestamp
                           ? korean_price.timestamp - foreign_price.timestamp
                           : foreign_price.timestamp - korean_price.timestamp;
    if (ts_diff > max_desync) return false;
    if (spread_pct(korean_price.bid, korean_price.ask) > max_kr_sp) return false;
    if (spread_pct(foreign_price.bid, foreign_price.ask) > max_fr_sp) return false;
    return true;
}

} // namespace

namespace kimp::execution {

OrderManager::OrderManager()
    : fill_query_executor_([this](FillQueryTask&& task, std::size_t worker_index) {
          handle_fill_query(std::move(task), worker_index);
      }) {}

OrderManager::~OrderManager() {
    running_.store(false, std::memory_order_release);
    fill_query_executor_.stop();
}

void OrderManager::set_exchange(Exchange ex, ExchangePtr exchange) {
    exchanges_[static_cast<size_t>(ex)] = exchange;
    if (ex == Exchange::Bithumb) {
        bithumb_exchange_ = std::dynamic_pointer_cast<exchange::bithumb::BithumbExchange>(exchange);
    } else if (ex == Exchange::Bybit) {
        bybit_exchange_ = std::dynamic_pointer_cast<exchange::bybit::BybitExchange>(exchange);
    }
}

OrderManager::KoreanExchangePtr OrderManager::get_korean_exchange(Exchange ex) {
    if (ex == Exchange::Bithumb) {
        return bithumb_exchange_;
    }
    return nullptr;
}

OrderManager::BybitExchangePtr OrderManager::get_bybit_exchange() {
    return bybit_exchange_;
}

ExecutionResult OrderManager::execute_spot_relay_entry(
        const ArbitrageSignal& signal,
        const std::optional<Position>& initial_position) {
    ExecutionResult result;
    result.position.symbol = signal.symbol;
    result.position.korean_exchange = signal.korean_exchange;
    result.position.foreign_exchange = signal.foreign_exchange;
    result.position.entry_time = initial_position
        ? initial_position->entry_time : std::chrono::system_clock::now();
    result.position.entry_premium = initial_position
        ? initial_position->entry_premium : signal.premium;

    // Dynamic position size based on current capital (복리 성장)
    double position_size_usd = engine_ ? engine_->get_position_size_usd() : TradingConfig::POSITION_SIZE_USD;
    result.position.position_size_usd = position_size_usd;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_bybit_exchange();

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Entry failed: {}", result.error_message);
        return result;
    }

    // SAFETY CHECK: Don't trade if we have existing positions outside bot tracking
    if (!is_safe_to_trade(signal.symbol, signal.korean_exchange, signal.foreign_exchange)) {
        result.error_message = "Existing position detected - skipping to prevent hedge break";
        Logger::warn("Entry blocked: {}", result.error_message);
        return result;
    }

    SymbolId foreign_symbol(signal.symbol.get_base(), "USDT");
    auto& probe = LatencyProbe::instance();
    const uint64_t trace_id = signal.trace_id != 0 ? signal.trace_id : probe.next_trace_id();
    const LatencySymbol trace_symbol =
        signal.trace_symbol[0] != '\0' ? signal.trace_symbol : LatencyProbe::format_symbol_fast(signal.symbol);
    const uint64_t trace_start_ns =
        signal.trace_start_ns != 0 ? signal.trace_start_ns : probe.capture_now_ns();
    auto record_latency = [&](LatencyStage stage,
                              int64_t aux0 = 0,
                              int64_t aux1 = 0,
                              double value0 = 0.0,
                              double value1 = 0.0) {
        probe.record(trace_id, trace_symbol, stage, trace_start_ns, aux0, aux1, value0, value1);
    };
    record_latency(LatencyStage::EntryLoopStart, 0, 0, signal.net_edge_pct, signal.max_tradable_usdt_at_best);

    // =========================================================================
    // SPOT RELAY LIFECYCLE
    // - entry: net edge positive and both venues can fill the target size
    // - exit: cover/sell when exit premium clears the dynamic threshold
    // - between: wait for the next fresh market update
    // =========================================================================
    double held_amount = initial_position ? initial_position->korean_amount : 0.0;
    double total_korean_cost = initial_position
        ? initial_position->korean_entry_price * initial_position->korean_amount : 0.0;
    double total_foreign_value = initial_position
        ? initial_position->foreign_entry_price * initial_position->foreign_amount : 0.0;
    double realized_pnl_krw = initial_position ? initial_position->realized_pnl_krw : 0.0;
    double last_usdt_rate = signal.usdt_krw_rate;

    if (initial_position) {
        Logger::info("[TOPUP] Resuming entry for {} from {:.8f} coins (${:.2f}/{:.2f})",
                     signal.symbol.to_string(), held_amount,
                     total_foreign_value, position_size_usd);
    }

    auto calculate_effective_entry_pm = [&](double usdt_rate) {
        if (held_amount <= 0.0 || total_foreign_value <= 0.0 || usdt_rate <= 0.0) {
            return signal.premium;
        }
        double avg_foreign_entry = total_foreign_value / held_amount;
        double avg_korean_entry = total_korean_cost / held_amount;
        double foreign_krw = avg_foreign_entry * usdt_rate;
        if (foreign_krw <= 0.0) {
            return signal.premium;
        }
        return ((avg_korean_entry - foreign_krw) / foreign_krw) * 100.0;
    };

    auto min_executable_usd = [&](double foreign_bid, double korean_ask) {
        constexpr double MIN_BYBIT_NOTIONAL_USD = 1.0;  // Conservative fallback when venue filters are unavailable here
        if (foreign_bid <= 0.0 || korean_ask <= 0.0) {
            return std::max(TradingConfig::MIN_ORDER_KRW / 1500.0, MIN_BYBIT_NOTIONAL_USD);
        }
        // Convert Bithumb KRW minimum to equivalent USD notional for this pair
        double min_by_krw = TradingConfig::MIN_ORDER_KRW * (foreign_bid / korean_ask);
        return std::max(min_by_krw, MIN_BYBIT_NOTIONAL_USD);
    };

    auto persist_snapshot = [&](const Position& snap) {
        if (engine_) {
            auto& tracker = engine_->get_position_tracker();
            Position existing;
            tracker.close_position(snap.symbol, existing);
            if (snap.is_active) {
                tracker.open_position(snap);
            }
        }

        if (on_position_update_) {
            if (snap.is_active) {
                on_position_update_(&snap);
            } else {
                on_position_update_(nullptr);
            }
        }
    };

    auto trigger_critical_stop = [&](const std::string& reason, const Position& snap) {
        Logger::error("[RISK-STOP] {}", reason);
        running_.store(false, std::memory_order_release);
        if (engine_) {
            engine_->set_entry_suppressed(true);
        }
        persist_snapshot(snap);
        result.position = snap;
        result.success = false;
        result.position_managed = true;
        result.error_message = reason;
    };

    while (running_.load(std::memory_order_acquire)) {
        // Get fresh prices from cache
        double current_foreign_bid = signal.foreign_bid;
        double current_foreign_ask = 0.0;
        double current_foreign_bid_qty = signal.foreign_bid_qty;
        double current_korean_ask = signal.korean_ask;
        double current_korean_bid = 0.0;
        double current_korean_ask_qty = signal.korean_ask_qty;
        double usdt_rate = signal.usdt_krw_rate;  // From signal (guaranteed real)
        bool quote_pair_ok = !engine_;
        if (engine_) {
            auto& cache = engine_->get_price_cache();
            auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
            auto korean_price = cache.get_price(signal.korean_exchange, signal.symbol);

            // EXIT path: skip freshness checks — just use latest cached prices.
            // We execute at market price anyway; cached price is only for premium calc.
            // ENTRY path: require fresh, tight-spread quotes.
            bool has_prices = (foreign_price.bid > 0 && korean_price.ask > 0);
            if (held_amount > 0) {
                // Exit mode: any non-zero cached price is good enough
                if (has_prices) {
                    current_foreign_bid = foreign_price.bid;
                    current_foreign_ask = foreign_price.ask;
                    current_foreign_bid_qty = foreign_price.bid_qty;
                    current_korean_ask = korean_price.ask;
                    current_korean_bid = korean_price.bid;
                    current_korean_ask_qty = korean_price.ask_qty;
                    quote_pair_ok = true;
                }
            } else {
                // Entry mode: strict freshness & spread checks
                if (has_prices && quote_pair_is_usable(korean_price, foreign_price)) {
                    current_foreign_bid = foreign_price.bid;
                    current_foreign_ask = foreign_price.ask;
                    current_foreign_bid_qty = foreign_price.bid_qty;
                    current_korean_ask = korean_price.ask;
                    current_korean_bid = korean_price.bid;
                    current_korean_ask_qty = korean_price.ask_qty;
                    quote_pair_ok = true;
                }
            }
            double bithumb_usdt = cache.get_usdt_krw(Exchange::Bithumb);
            if (bithumb_usdt > 0) usdt_rate = bithumb_usdt;
        }
        last_usdt_rate = usdt_rate;

        if (!quote_pair_ok) {
            if (engine_) {
                uint64_t seq = engine_->get_update_seq();
                engine_->wait_for_update(seq, std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            }
            continue;
        }

        // Calculate ENTRY premium: buy Korean (ask), short foreign (bid)
        double entry_premium = 0.0;
        if (current_foreign_bid > 0 && usdt_rate > 0) {
            double foreign_krw = current_foreign_bid * usdt_rate;
            entry_premium = ((current_korean_ask - foreign_krw) / foreign_krw) * 100.0;
        }

        // Calculate EXIT premium: sell Korean (bid), cover foreign (ask)
        double exit_premium = 0.0;
        if (current_foreign_ask > 0 && usdt_rate > 0 && current_korean_bid > 0) {
            double foreign_krw = current_foreign_ask * usdt_rate;
            exit_premium = ((current_korean_bid - foreign_krw) / foreign_krw) * 100.0;
        }

        const double open_entry_notional_usd = total_foreign_value;
        double remaining_position_usd = std::max(0.0, position_size_usd - open_entry_notional_usd);
        const double next_order_usd = std::min(TradingConfig::ORDER_SIZE_USD, remaining_position_usd);
        double next_order_coin_amount = current_foreign_bid > 0.0
            ? (next_order_usd / current_foreign_bid)
            : 0.0;
        bool korean_can_fill_required = next_order_coin_amount > 0.0 &&
                                        current_korean_ask_qty >= next_order_coin_amount;
        bool foreign_can_fill_required = next_order_coin_amount > 0.0 &&
                                         current_foreign_bid_qty >= next_order_coin_amount;
        auto relay_metrics = strategy::PremiumCalculator::calculate_relay_metrics(
            current_korean_ask,
            current_korean_ask_qty,
            current_foreign_bid,
            current_foreign_bid_qty,
            usdt_rate);

        // Dynamic exit threshold with a hard floor (+0.10% by default)
        double dynamic_exit_threshold = TradingConfig::EXIT_PREMIUM_THRESHOLD;
        if (held_amount > 0 && total_foreign_value > 0 && usdt_rate > 0) {
            double effective_entry_pm = calculate_effective_entry_pm(usdt_rate);
            dynamic_exit_threshold = std::max(
                effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD,
                TradingConfig::EXIT_PREMIUM_THRESHOLD);
        }

        double min_split_usd = min_executable_usd(current_foreign_bid, current_korean_ask);

        const uint64_t update_seq_before_trade = engine_ ? engine_->get_update_seq() : 0;
        auto split_start = std::chrono::steady_clock::now();

        if (relay_metrics.net_edge_pct > TradingConfig::MIN_NET_EDGE_PCT &&
            korean_can_fill_required &&
            foreign_can_fill_required &&
            next_order_usd >= min_split_usd) {
            // ==================== ENTRY ====================
            double order_size_usd = next_order_usd;
            double coin_amount = next_order_coin_amount;

            // Open Bybit spot-margin short first (no fill query — deferred to parallel)
            record_latency(LatencyStage::EntryForeignSubmitStart, 0, 0, coin_amount, order_size_usd);
            Order foreign_order = execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);
            record_latency(LatencyStage::EntryForeignSubmitAck,
                           static_cast<int64_t>(foreign_order.status),
                           0,
                           foreign_order.quantity,
                           foreign_order.average_price);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[RELAY-ENTRY] Bybit spot-margin short failed, retrying next cycle");
                record_latency(LatencyStage::Error, static_cast<int64_t>(foreign_order.status));
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            // Use lot-size normalized quantity (known pre-fill-query)
            double actual_filled = foreign_order.quantity;
            double krw_amount = actual_filled * current_korean_ask;

            if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
                Logger::warn("[RELAY-ENTRY] Order too small ({:.0f} KRW), rolling back Bybit spot-margin short", krw_amount);
                Order rollback = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                if (rollback.status != OrderStatus::Filled) {
                    Position mismatch;
                    mismatch.symbol = signal.symbol;
                    mismatch.korean_exchange = signal.korean_exchange;
                    mismatch.foreign_exchange = signal.foreign_exchange;
                    mismatch.entry_time = result.position.entry_time;
                    mismatch.entry_premium = result.position.entry_premium;
                    mismatch.position_size_usd = position_size_usd;
                    mismatch.korean_amount = held_amount;
                    mismatch.foreign_amount = held_amount + actual_filled;
                    mismatch.korean_entry_price = held_amount > 0 ? (total_korean_cost / held_amount) : 0.0;
                    double short_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_bid;
                    double foreign_value = total_foreign_value + (actual_filled * short_price);
                    mismatch.foreign_entry_price = mismatch.foreign_amount > 0 ? (foreign_value / mismatch.foreign_amount) : 0.0;
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[RELAY-ENTRY] Bybit rollback failed after min-order guard (manual intervention required)",
                        mismatch
                    );
                    return result;
                }
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            // Reuse dedicated fill workers instead of spawning per-order async threads.
            std::latch fill_done(2);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Foreign,
                signal.foreign_exchange,
                SymbolId{},
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::EntryForeignFillQueryStart,
                LatencyStage::EntryForeignFillWorkerStart,
                LatencyStage::EntryForeignFillDone,
                &foreign_order,
                &fill_done,
            });

            record_latency(LatencyStage::EntryKoreanSubmitStart, 0, 0, actual_filled, krw_amount);
            Order korean_order = execute_korean_buy(signal.korean_exchange, signal.symbol, actual_filled, krw_amount);
            record_latency(LatencyStage::EntryKoreanSubmitAck,
                           static_cast<int64_t>(korean_order.status),
                           0,
                           korean_order.quantity,
                           korean_order.average_price);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Korean,
                signal.korean_exchange,
                signal.symbol,
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::EntryKoreanFillQueryStart,
                LatencyStage::EntryKoreanFillWorkerStart,
                LatencyStage::EntryKoreanFillDone,
                &korean_order,
                &fill_done,
            });
            fill_done.wait();

            if (korean_order.status == OrderStatus::Filled) {
                double short_price = foreign_order.average_price;
                if (short_price <= 0) {
                    short_price = current_foreign_bid;
                    Logger::warn("[ADAPTIVE-ENTRY] No fill price from Bybit, using cache {:.8f}", short_price);
                }
                double buy_price = korean_order.average_price;
                if (buy_price <= 0) {
                    buy_price = current_korean_ask;
                    Logger::warn("[ADAPTIVE-ENTRY] No fill price from Bithumb, using cache {:.2f}", buy_price);
                }
                // Reconcile filled_quantity if available (should match order.quantity for $25 market orders)
                if (foreign_order.filled_quantity > 0) actual_filled = foreign_order.filled_quantity;

                double actual_korean_cost = actual_filled * buy_price;

                held_amount += actual_filled;
                total_korean_cost += actual_korean_cost;
                total_foreign_value += actual_filled * short_price;

                double new_open_notional_usd = total_foreign_value;
                double effective_entry_pm = calculate_effective_entry_pm(usdt_rate);
                double target_exit_pm = std::max(
                    effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD,
                    TradingConfig::EXIT_PREMIUM_THRESHOLD);

                auto split_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - split_start).count();
                Logger::info("[RELAY-ENTRY] {} +{:.8f} coins (${:.2f}), held: ${:.2f}/${:.2f}, "
                             "net_edge_now: {:.4f}%, entry_pm_now: {:.4f}%, eff_entry_pm: {:.4f}%, "
                             "target_exit_pm: {:.4f}%, buy_price: {:.2f}, exec_ms: {}",
                             signal.symbol.to_string(), actual_filled, order_size_usd,
                             new_open_notional_usd, position_size_usd, relay_metrics.net_edge_pct,
                             entry_premium, effective_entry_pm, target_exit_pm, buy_price, split_elapsed);

                // Log entry split to CSV (actual fill price)
                append_entry_split_log(signal.symbol, actual_filled, buy_price,
                                       short_price, usdt_rate, new_open_notional_usd,
                                       position_size_usd, effective_entry_pm);

                // Build current position snapshot
                Position snap;
                snap.symbol = signal.symbol;
                snap.korean_exchange = signal.korean_exchange;
                snap.foreign_exchange = signal.foreign_exchange;
                snap.entry_time = result.position.entry_time;
                snap.entry_premium = effective_entry_pm;
                snap.position_size_usd = position_size_usd;
                snap.korean_amount = held_amount;
                snap.foreign_amount = held_amount;
                snap.korean_entry_price = held_amount > 0 ? total_korean_cost / held_amount : 0.0;
                snap.foreign_entry_price = held_amount > 0 ? total_foreign_value / held_amount : 0.0;
                snap.realized_pnl_krw = realized_pnl_krw;
                snap.is_active = true;
                persist_snapshot(snap);

                if (new_open_notional_usd >= position_size_usd) {
                    Logger::info("[ADAPTIVE] {} fully entered: ${:.2f}, monitoring for exit",
                                 signal.symbol.to_string(), new_open_notional_usd);
                }
                record_latency(LatencyStage::EntryCompleted, 0, 0, actual_filled, new_open_notional_usd);
            } else {
                Logger::error("[RELAY-ENTRY] Korean BUY failed, rolling back Bybit spot-margin short");
                record_latency(LatencyStage::Error, static_cast<int64_t>(korean_order.status));
                Order rollback = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                if (rollback.status != OrderStatus::Filled) {
                    Position mismatch;
                    mismatch.symbol = signal.symbol;
                    mismatch.korean_exchange = signal.korean_exchange;
                    mismatch.foreign_exchange = signal.foreign_exchange;
                    mismatch.entry_time = result.position.entry_time;
                    mismatch.entry_premium = result.position.entry_premium;
                    mismatch.position_size_usd = position_size_usd;
                    mismatch.korean_amount = held_amount;
                    mismatch.foreign_amount = held_amount + actual_filled;
                    mismatch.korean_entry_price = held_amount > 0 ? (total_korean_cost / held_amount) : 0.0;
                    double short_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_bid;
                    double foreign_value = total_foreign_value + (actual_filled * short_price);
                    mismatch.foreign_entry_price = mismatch.foreign_amount > 0 ? (foreign_value / mismatch.foreign_amount) : 0.0;
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[RELAY-ENTRY] Bybit rollback failed after Korean BUY rejection (manual intervention required)",
                        mismatch
                    );
                    return result;
                }
            }

        } else if (exit_premium >= dynamic_exit_threshold && held_amount > 0) {
            // ==================== EXIT SPLIT ====================
            record_latency(LatencyStage::ExitLoopStart, 0, 0, exit_premium, dynamic_exit_threshold);
            double remaining_value_usd = held_amount * current_foreign_ask;
            double exit_coin_amount;

            if (remaining_value_usd < 50.0) {
                exit_coin_amount = held_amount;
            } else {
                exit_coin_amount = TradingConfig::ORDER_SIZE_USD / current_foreign_ask;
                exit_coin_amount = std::min(exit_coin_amount, held_amount);
            }

            Logger::info("[ADAPTIVE-EXIT] {} exit_pm: {:.4f}% >= dynamic_threshold: {:.4f}%, exiting",
                         signal.symbol.to_string(), exit_premium, dynamic_exit_threshold);

            // Cover Bybit spot-margin short first (no fill query — deferred to parallel)
            record_latency(LatencyStage::ExitForeignSubmitStart, 0, 0, exit_coin_amount, remaining_value_usd);
            Order foreign_order = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, exit_coin_amount);
            record_latency(LatencyStage::ExitForeignSubmitAck,
                           static_cast<int64_t>(foreign_order.status),
                           0,
                           foreign_order.quantity,
                           foreign_order.average_price);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-EXIT] Bybit spot-margin cover failed during entry, retrying");
                record_latency(LatencyStage::Error, static_cast<int64_t>(foreign_order.status));
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            double actual_covered = foreign_order.quantity;

            std::latch fill_done(2);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Foreign,
                signal.foreign_exchange,
                SymbolId{},
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ExitForeignFillQueryStart,
                LatencyStage::ExitForeignFillWorkerStart,
                LatencyStage::ExitForeignFillDone,
                &foreign_order,
                &fill_done,
            });

            record_latency(LatencyStage::ExitKoreanSubmitStart, 0, 0, actual_covered, current_korean_bid);
            Order korean_order = execute_korean_sell(signal.korean_exchange, signal.symbol, actual_covered);
            record_latency(LatencyStage::ExitKoreanSubmitAck,
                           static_cast<int64_t>(korean_order.status),
                           0,
                           korean_order.quantity,
                           korean_order.average_price);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Korean,
                signal.korean_exchange,
                signal.symbol,
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ExitKoreanFillQueryStart,
                LatencyStage::ExitKoreanFillWorkerStart,
                LatencyStage::ExitKoreanFillDone,
                &korean_order,
                &fill_done,
            });
            fill_done.wait();

            if (korean_order.status == OrderStatus::Filled) {
                // Reconcile filled_quantity if available
                if (foreign_order.filled_quantity > 0) actual_covered = foreign_order.filled_quantity;

                double cover_price = foreign_order.average_price;
                if (cover_price <= 0) {
                    cover_price = current_foreign_ask;
                    Logger::warn("[ADAPTIVE-EXIT] No cover fill price from Bybit, using cache {:.8f}", cover_price);
                }
                double sell_price = korean_order.average_price;
                if (sell_price <= 0) {
                    sell_price = current_korean_bid;
                    Logger::warn("[ADAPTIVE-EXIT] No sell fill price from Bithumb, using cache {:.2f}", sell_price);
                }

                // P&L using average entry prices
                double avg_korean_entry = held_amount > 0 ? total_korean_cost / held_amount : 0.0;
                double avg_foreign_entry = held_amount > 0 ? total_foreign_value / held_amount : 0.0;
                double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                realized_pnl_krw += split_pnl_krw;

                // Reduce costs proportionally
                double exit_ratio = held_amount > 0 ? actual_covered / held_amount : 1.0;
                total_korean_cost *= (1.0 - exit_ratio);
                total_foreign_value *= (1.0 - exit_ratio);
                held_amount -= actual_covered;

                // Log exit split
                Position temp_pos = result.position;
                temp_pos.korean_entry_price = avg_korean_entry;
                temp_pos.foreign_entry_price = avg_foreign_entry;
                append_exit_split_log(temp_pos, actual_covered, sell_price, cover_price,
                                      split_pnl_krw, usdt_rate, held_amount <= 0);

                Logger::info("[ADAPTIVE-EXIT] {} -{:.8f} coins, P&L: {:.0f} KRW, remaining: {:.8f}, exit_pm: {:.4f}%",
                             signal.symbol.to_string(), actual_covered, split_pnl_krw, held_amount, exit_premium);
                if (held_amount <= 0) {
                    record_latency(LatencyStage::ExitCompleted, 0, 0, actual_covered, realized_pnl_krw);
                }

                // Update position file after exit split
                if (on_position_update_) {
                    if (held_amount > 0) {
                        Position snap;
                        snap.symbol = signal.symbol;
                        snap.korean_exchange = signal.korean_exchange;
                        snap.foreign_exchange = signal.foreign_exchange;
                        snap.entry_time = result.position.entry_time;
                        snap.entry_premium = calculate_effective_entry_pm(usdt_rate);
                        snap.position_size_usd = position_size_usd;
                        snap.korean_amount = held_amount;
                        snap.foreign_amount = held_amount;
                        snap.korean_entry_price = held_amount > 0 ? total_korean_cost / held_amount : 0.0;
                        snap.foreign_entry_price = held_amount > 0 ? total_foreign_value / held_amount : 0.0;
                        snap.realized_pnl_krw = realized_pnl_krw;
                        snap.is_active = true;
                        on_position_update_(&snap);
                    } else {
                        on_position_update_(nullptr);
                    }
                }

                if (held_amount <= 0) {
                    // Fully exited — complete trade cycle, then continue for re-entry
                    Logger::info("[ADAPTIVE] {} fully exited. Cycle P&L: {:.0f} KRW. Continuing for re-entry...",
                                 signal.symbol.to_string(), realized_pnl_krw);

                    // Close position from engine tracker
                    if (engine_) {
                        Position closed;
                        engine_->get_position_tracker().close_position(signal.symbol, closed);
                    }

                    // Fire trade complete callback (P&L logging)
                    if (on_trade_complete_) {
                        Position completed_pos;
                        completed_pos.symbol = signal.symbol;
                        completed_pos.korean_exchange = signal.korean_exchange;
                        completed_pos.foreign_exchange = signal.foreign_exchange;
                        completed_pos.entry_time = result.position.entry_time;
                        completed_pos.exit_time = std::chrono::system_clock::now();
                        completed_pos.entry_premium = result.position.entry_premium;
                        completed_pos.exit_premium = exit_premium;
                        completed_pos.position_size_usd = position_size_usd;
                        completed_pos.realized_pnl_krw = realized_pnl_krw;
                        completed_pos.is_active = false;
                        on_trade_complete_(completed_pos, realized_pnl_krw, usdt_rate);
                    }

                    // Reset state for next entry cycle
                    total_korean_cost = 0.0;
                    total_foreign_value = 0.0;
                    realized_pnl_krw = 0.0;
                    result.position.entry_time = std::chrono::system_clock::now();
                    result.position.entry_premium = 0.0;
                }
            } else {
                // SELL failed after COVER — retry to avoid unhedged state
                // Resolve fill prices for P&L on retry path
                double cover_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_ask;

                Logger::error("[ADAPTIVE-EXIT] Korean SELL failed after COVER, retrying... Unhedged: {:.8f} coins", actual_covered);
                record_latency(LatencyStage::Error, static_cast<int64_t>(korean_order.status));
                for (int retry = 1; retry <= 5; ++retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300 * retry));
                    Logger::warn("[ADAPTIVE-EXIT] SELL retry {}/5 for {:.8f} coins", retry, actual_covered);
                    korean_order = execute_korean_sell(signal.korean_exchange, signal.symbol, actual_covered);
                    if (korean_order.status == OrderStatus::Filled) {
                        query_korean_fill(signal.korean_exchange, signal.symbol, korean_order);
                        double sell_price = korean_order.average_price;
                        if (sell_price <= 0) sell_price = current_korean_bid;

                        double avg_korean_entry = held_amount > 0 ? total_korean_cost / held_amount : 0.0;
                        double avg_foreign_entry = held_amount > 0 ? total_foreign_value / held_amount : 0.0;
                        double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                        double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                        double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                        double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                        realized_pnl_krw += split_pnl_krw;

                        double exit_ratio = held_amount > 0 ? actual_covered / held_amount : 1.0;
                        total_korean_cost *= (1.0 - exit_ratio);
                        total_foreign_value *= (1.0 - exit_ratio);
                        held_amount -= actual_covered;
                        if (held_amount <= 0) {
                            record_latency(LatencyStage::ExitCompleted, 0, 0, actual_covered, realized_pnl_krw);
                        }

                        Logger::info("[ADAPTIVE-EXIT] SELL retry succeeded: {:.8f} coins @ {:.2f}, P&L: {:.0f} KRW",
                                     actual_covered, sell_price, split_pnl_krw);
                        break;
                    }
                }
                if (korean_order.status != OrderStatus::Filled) {
                    Logger::error("[ADAPTIVE-EXIT] CRITICAL: Korean SELL failed after 5 retries! "
                                  "UNHEDGED {:.8f} coins — MANUAL INTERVENTION REQUIRED", actual_covered);
                    Position mismatch;
                    mismatch.symbol = signal.symbol;
                    mismatch.korean_exchange = signal.korean_exchange;
                    mismatch.foreign_exchange = signal.foreign_exchange;
                    mismatch.entry_time = result.position.entry_time;
                    mismatch.entry_premium = result.position.entry_premium;
                    mismatch.position_size_usd = position_size_usd;
                    mismatch.korean_amount = held_amount;
                    mismatch.foreign_amount = std::max(0.0, held_amount - actual_covered);
                    mismatch.korean_entry_price = held_amount > 0 ? (total_korean_cost / held_amount) : 0.0;
                    mismatch.foreign_entry_price = held_amount > 0 ? (total_foreign_value / held_amount) : 0.0;
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[ADAPTIVE-EXIT] Korean SELL failed after successful Bybit spot-margin cover; bot stopped for manual recovery",
                        mismatch
                    );
                    return result;
                }
            }

        } else {
            // ==================== WAIT ====================
            if (engine_) {
                uint64_t seq = engine_->get_update_seq();
                engine_->wait_for_update(seq, std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            }
            continue;
        }

        // Event-driven post-trade wait: continue immediately on the next WS update.
        wait_for_next_market_update(update_seq_before_trade);
    }

    // Loop exited = shutdown. Return current state.
    result.position_managed = true;  // Position already managed inside the loop
    if (held_amount > 0) {
        Logger::warn("[ADAPTIVE] Shutdown with active position: {} {:.8f} coins",
                     signal.symbol.to_string(), held_amount);
        result.success = true;
        result.position.korean_amount = held_amount;
        result.position.foreign_amount = held_amount;
        result.position.korean_entry_price = held_amount > 0 ? total_korean_cost / held_amount : 0.0;
        result.position.foreign_entry_price = held_amount > 0 ? total_foreign_value / held_amount : 0.0;
        result.position.entry_premium = calculate_effective_entry_pm(last_usdt_rate);
        result.position.is_active = true;
        result.position.realized_pnl_krw = realized_pnl_krw;
    } else {
        result.success = false;
        result.position.is_active = false;
    }
    return result;
}

ExecutionResult OrderManager::execute_spot_relay_exit(const ExitSignal& signal, const Position& position) {
    ExecutionResult result;
    result.position = position;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_bybit_exchange();

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Exit failed: {}", result.error_message);
        return result;
    }

    SymbolId foreign_symbol(position.symbol.get_base(), "USDT");
    auto& probe = LatencyProbe::instance();
    const uint64_t trace_id = signal.trace_id != 0 ? signal.trace_id : probe.next_trace_id();
    const LatencySymbol trace_symbol =
        signal.trace_symbol[0] != '\0' ? signal.trace_symbol : LatencyProbe::format_symbol_fast(position.symbol);
    const uint64_t trace_start_ns =
        signal.trace_start_ns != 0 ? signal.trace_start_ns : probe.capture_now_ns();
    auto record_latency = [&](LatencyStage stage,
                              int64_t aux0 = 0,
                              int64_t aux1 = 0,
                              double value0 = 0.0,
                              double value1 = 0.0) {
        probe.record(trace_id, trace_symbol, stage, trace_start_ns, aux0, aux1, value0, value1);
    };

    // =========================================================================
    // ADAPTIVE SPLIT EXECUTION: 청산/재진입 유기적 전환
    // - exit_pm >= dynamic threshold: exit split ($25 cover + sell, <$50 close all)
    // - entry_pm <= -0.99% && partially exited: re-entry split ($25 short + buy)
    // - between: wait for market update
    // Loop ends when: fully exited (0 coins remaining)
    // =========================================================================
    double remaining_amount = position.foreign_amount;
    double original_amount = position.foreign_amount;
    double total_korean_cost = position.korean_entry_price * position.korean_amount;
    double total_foreign_value = position.foreign_entry_price * position.foreign_amount;
    double realized_pnl_krw = 0.0;
    double total_korean_proceeds = 0.0;
    double total_foreign_cost = 0.0;
    double total_exited_amount = 0.0;
    double last_usdt_rate = signal.usdt_krw_rate;

    auto calculate_effective_entry_pm = [&](double usdt_rate) {
        if (remaining_amount <= 0.0 || total_foreign_value <= 0.0 || usdt_rate <= 0.0) {
            return position.entry_premium;
        }
        double avg_foreign_entry = total_foreign_value / remaining_amount;
        double avg_korean_entry = total_korean_cost / remaining_amount;
        double foreign_krw = avg_foreign_entry * usdt_rate;
        if (foreign_krw <= 0.0) {
            return position.entry_premium;
        }
        return ((avg_korean_entry - foreign_krw) / foreign_krw) * 100.0;
    };

    auto persist_snapshot = [&](const Position& snap) {
        if (engine_) {
            auto& tracker = engine_->get_position_tracker();
            Position existing;
            tracker.close_position(snap.symbol, existing);
            if (snap.is_active) {
                tracker.open_position(snap);
            }
        }

        if (on_position_update_) {
            if (snap.is_active) {
                on_position_update_(&snap);
            } else {
                on_position_update_(nullptr);
            }
        }
    };

    auto trigger_critical_stop = [&](const std::string& reason, const Position& snap) {
        Logger::error("[RISK-STOP] {}", reason);
        running_.store(false, std::memory_order_release);
        if (engine_) {
            engine_->set_entry_suppressed(true);
        }
        persist_snapshot(snap);
        result.position = snap;
        result.success = false;
        result.position_managed = true;
        result.error_message = reason;
    };

    while (remaining_amount > 0 && running_.load(std::memory_order_acquire)) {
        // Get fresh prices from cache
        double current_foreign_bid = 0.0;
        double current_foreign_ask = signal.foreign_ask;
        double current_korean_ask = 0.0;
        double current_korean_bid = signal.korean_bid;
        double usdt_rate = signal.usdt_krw_rate;  // From signal (guaranteed real)
        bool quote_pair_ok = !engine_;
        if (engine_) {
            auto& cache = engine_->get_price_cache();
            auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
            auto korean_price = cache.get_price(signal.korean_exchange, position.symbol);
            if (quote_pair_is_usable(korean_price, foreign_price)) {
                current_foreign_bid = foreign_price.bid;
                current_foreign_ask = foreign_price.ask;
                current_korean_ask = korean_price.ask;
                current_korean_bid = korean_price.bid;
                quote_pair_ok = true;
            }
            double bithumb_usdt = cache.get_usdt_krw(Exchange::Bithumb);
            if (bithumb_usdt > 0) usdt_rate = bithumb_usdt;
        }
        last_usdt_rate = usdt_rate;

        if (!quote_pair_ok) {
            if (engine_) {
                uint64_t seq = engine_->get_update_seq();
                engine_->wait_for_update(seq, std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            }
            continue;
        }

        // Calculate EXIT premium: sell Korean (bid), cover foreign (ask)
        double exit_premium = 0.0;
        if (current_foreign_ask > 0 && usdt_rate > 0 && current_korean_bid > 0) {
            double foreign_krw = current_foreign_ask * usdt_rate;
            exit_premium = ((current_korean_bid - foreign_krw) / foreign_krw) * 100.0;
        }

        // Calculate ENTRY premium: buy Korean (ask), short foreign (bid)
        double entry_premium = 0.0;
        if (current_foreign_bid > 0 && usdt_rate > 0 && current_korean_ask > 0) {
            double foreign_krw = current_foreign_bid * usdt_rate;
            entry_premium = ((current_korean_ask - foreign_krw) / foreign_krw) * 100.0;
        }

        // Dynamic exit threshold with a hard floor (+0.10% by default)
        double dynamic_exit_threshold = TradingConfig::EXIT_PREMIUM_THRESHOLD;
        if (remaining_amount > 0 && total_foreign_value > 0 && usdt_rate > 0) {
            double effective_entry_pm = calculate_effective_entry_pm(usdt_rate);
            dynamic_exit_threshold = std::max(
                effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD,
                TradingConfig::EXIT_PREMIUM_THRESHOLD);
        }

        const uint64_t update_seq_before_trade = engine_ ? engine_->get_update_seq() : 0;

        if (exit_premium >= dynamic_exit_threshold) {
            // ==================== EXIT SPLIT ====================
            record_latency(LatencyStage::ExitLoopStart, 0, 0, exit_premium, dynamic_exit_threshold);
            double remaining_value_usd = remaining_amount * current_foreign_ask;
            double exit_coin_amount;

            Logger::info("[ADAPTIVE-EXIT] {} exit_pm: {:.4f}% >= dynamic_threshold: {:.4f}%, exiting",
                         position.symbol.to_string(), exit_premium, dynamic_exit_threshold);

            if (remaining_value_usd < 50.0) {
                exit_coin_amount = remaining_amount;
            } else {
                exit_coin_amount = TradingConfig::ORDER_SIZE_USD / current_foreign_ask;
                exit_coin_amount = std::min(exit_coin_amount, remaining_amount);
            }

            // Cover Bybit spot-margin short first (no fill query — deferred to parallel)
            record_latency(LatencyStage::ExitForeignSubmitStart, 0, 0, exit_coin_amount, remaining_value_usd);
            Order foreign_order = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, exit_coin_amount);
            record_latency(LatencyStage::ExitForeignSubmitAck,
                           static_cast<int64_t>(foreign_order.status),
                           0,
                           foreign_order.quantity,
                           foreign_order.average_price);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-EXIT] Bybit spot-margin cover failed, retrying");
                record_latency(LatencyStage::Error, static_cast<int64_t>(foreign_order.status));
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            double actual_covered = foreign_order.quantity;

            std::latch fill_done(2);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Foreign,
                signal.foreign_exchange,
                SymbolId{},
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ExitForeignFillQueryStart,
                LatencyStage::ExitForeignFillWorkerStart,
                LatencyStage::ExitForeignFillDone,
                &foreign_order,
                &fill_done,
            });

            record_latency(LatencyStage::ExitKoreanSubmitStart, 0, 0, actual_covered, current_korean_bid);
            Order korean_order = execute_korean_sell(signal.korean_exchange, position.symbol, actual_covered);
            record_latency(LatencyStage::ExitKoreanSubmitAck,
                           static_cast<int64_t>(korean_order.status),
                           0,
                           korean_order.quantity,
                           korean_order.average_price);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Korean,
                signal.korean_exchange,
                position.symbol,
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ExitKoreanFillQueryStart,
                LatencyStage::ExitKoreanFillWorkerStart,
                LatencyStage::ExitKoreanFillDone,
                &korean_order,
                &fill_done,
            });
            fill_done.wait();

            if (korean_order.status == OrderStatus::Filled) {
                if (foreign_order.filled_quantity > 0) actual_covered = foreign_order.filled_quantity;

                double cover_price = foreign_order.average_price;
                if (cover_price <= 0) {
                    cover_price = current_foreign_ask;
                    Logger::warn("[EXIT] No cover fill price from Bybit, using cache {:.8f}", cover_price);
                }
                double sell_price = korean_order.average_price;
                if (sell_price <= 0) {
                    sell_price = current_korean_bid;
                    Logger::warn("[EXIT] No sell fill price from Bithumb, using cache {:.2f}", sell_price);
                }

                // P&L using average entry prices
                double avg_korean_entry = total_korean_cost / remaining_amount;
                double avg_foreign_entry = total_foreign_value / remaining_amount;
                double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                realized_pnl_krw += split_pnl_krw;

                // Track totals for result
                total_korean_proceeds += actual_covered * sell_price;
                total_foreign_cost += actual_covered * cover_price;
                total_exited_amount += actual_covered;

                // Reduce costs proportionally
                double exit_ratio = actual_covered / remaining_amount;
                total_korean_cost *= (1.0 - exit_ratio);
                total_foreign_value *= (1.0 - exit_ratio);
                remaining_amount -= actual_covered;

                bool final_split = remaining_amount <= 0;
                Position log_pos = position;
                log_pos.korean_entry_price = avg_korean_entry;
                log_pos.foreign_entry_price = avg_foreign_entry;
                append_exit_split_log(log_pos, actual_covered, sell_price, cover_price,
                                      split_pnl_krw, usdt_rate, final_split);

                Logger::info("[ADAPTIVE-EXIT] {} -{:.8f} coins, P&L: {:.0f} KRW, remaining: {:.8f}, exit_pm: {:.4f}%",
                             position.symbol.to_string(), actual_covered, split_pnl_krw, remaining_amount, exit_premium);
                if (final_split) {
                    record_latency(LatencyStage::ExitCompleted, 0, 0, actual_covered, realized_pnl_krw);
                }

                // Update position file
                if (on_position_update_) {
                    if (remaining_amount > 0) {
                        Position snap = position;
                        snap.korean_amount = remaining_amount;
                        snap.foreign_amount = remaining_amount;
                        snap.korean_entry_price = total_korean_cost / remaining_amount;
                        snap.foreign_entry_price = total_foreign_value / remaining_amount;
                        snap.entry_premium = calculate_effective_entry_pm(usdt_rate);
                        snap.realized_pnl_krw = realized_pnl_krw;
                        on_position_update_(&snap);
                    } else {
                        on_position_update_(nullptr);
                    }
                }
            } else {
                double cover_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_ask;

                // SELL failed after COVER — retry to avoid unhedged state
                Logger::error("[EXIT] Korean SELL failed after COVER, retrying... Unhedged: {:.8f} coins", actual_covered);
                record_latency(LatencyStage::Error, static_cast<int64_t>(korean_order.status));
                for (int retry = 1; retry <= 5; ++retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300 * retry));
                    Logger::warn("[EXIT] SELL retry {}/5 for {:.8f} coins", retry, actual_covered);
                    korean_order = execute_korean_sell(signal.korean_exchange, position.symbol, actual_covered);
                    if (korean_order.status == OrderStatus::Filled) {
                        query_korean_fill(signal.korean_exchange, position.symbol, korean_order);
                        double sell_price = korean_order.average_price;
                        if (sell_price <= 0) sell_price = current_korean_bid;

                        double avg_korean_entry = total_korean_cost / remaining_amount;
                        double avg_foreign_entry = total_foreign_value / remaining_amount;
                        double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                        double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                        double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                        double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                        realized_pnl_krw += split_pnl_krw;

                        total_korean_proceeds += actual_covered * sell_price;
                        total_foreign_cost += actual_covered * cover_price;
                        total_exited_amount += actual_covered;

                        double exit_ratio = actual_covered / remaining_amount;
                        total_korean_cost *= (1.0 - exit_ratio);
                        total_foreign_value *= (1.0 - exit_ratio);
                        remaining_amount -= actual_covered;
                        if (remaining_amount <= 0) {
                            record_latency(LatencyStage::ExitCompleted, 0, 0, actual_covered, realized_pnl_krw);
                        }

                        Logger::info("[EXIT] SELL retry succeeded: {:.8f} coins @ {:.2f}, P&L: {:.0f} KRW",
                                     actual_covered, sell_price, split_pnl_krw);
                        break;
                    }
                }
                if (korean_order.status != OrderStatus::Filled) {
                    Logger::error("[EXIT] CRITICAL: Korean SELL failed after 5 retries! "
                                  "UNHEDGED {:.8f} coins — MANUAL INTERVENTION REQUIRED", actual_covered);
                    Position mismatch = position;
                    mismatch.korean_amount = remaining_amount;
                    mismatch.foreign_amount = std::max(0.0, remaining_amount - actual_covered);
                    mismatch.korean_entry_price = remaining_amount > 0 ? (total_korean_cost / remaining_amount) : 0.0;
                    mismatch.foreign_entry_price = remaining_amount > 0 ? (total_foreign_value / remaining_amount) : 0.0;
                    mismatch.entry_premium = calculate_effective_entry_pm(usdt_rate);
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[EXIT] Korean SELL failed after successful Bybit spot-margin cover; bot stopped for manual recovery",
                        mismatch
                    );
                    return result;
                }
            }

        } else if (entry_premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD && remaining_amount < original_amount) {
            // ==================== RE-ENTRY SPLIT ====================
            record_latency(LatencyStage::ReentryLoopStart, 0, 0, entry_premium, remaining_amount);
            double max_reentry_coins = original_amount - remaining_amount;
            double reentry_usd = std::min(TradingConfig::ORDER_SIZE_USD, max_reentry_coins * current_foreign_bid);
            double coin_amount = reentry_usd / current_foreign_bid;

            // Open Bybit spot-margin short first (no fill query — deferred to parallel)
            record_latency(LatencyStage::ReentryForeignSubmitStart, 0, 0, coin_amount, reentry_usd);
            Order foreign_order = execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);
            record_latency(LatencyStage::ReentryForeignSubmitAck,
                           static_cast<int64_t>(foreign_order.status),
                           0,
                           foreign_order.quantity,
                           foreign_order.average_price);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-REENTRY] Bybit spot-margin short failed, retrying");
                record_latency(LatencyStage::Error, static_cast<int64_t>(foreign_order.status));
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            double actual_filled = foreign_order.quantity;
            double krw_amount = actual_filled * current_korean_ask;

            if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
                Logger::warn("[ADAPTIVE-REENTRY] Order too small, rolling back");
                Order rollback = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                if (rollback.status != OrderStatus::Filled) {
                    Position mismatch = position;
                    mismatch.korean_amount = remaining_amount;
                    mismatch.foreign_amount = remaining_amount + actual_filled;
                    mismatch.korean_entry_price = remaining_amount > 0 ? (total_korean_cost / remaining_amount) : 0.0;
                    double short_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_bid;
                    double foreign_value = total_foreign_value + (actual_filled * short_price);
                    mismatch.foreign_entry_price = mismatch.foreign_amount > 0 ? (foreign_value / mismatch.foreign_amount) : 0.0;
                    mismatch.entry_premium = calculate_effective_entry_pm(usdt_rate);
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[ADAPTIVE-REENTRY] Bybit rollback failed after min-order guard (manual intervention required)",
                        mismatch
                    );
                    return result;
                }
                wait_for_next_market_update(update_seq_before_trade);
                continue;
            }

            std::latch fill_done(2);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Foreign,
                signal.foreign_exchange,
                SymbolId{},
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ReentryForeignFillQueryStart,
                LatencyStage::ReentryForeignFillWorkerStart,
                LatencyStage::ReentryForeignFillDone,
                &foreign_order,
                &fill_done,
            });

            record_latency(LatencyStage::ReentryKoreanSubmitStart, 0, 0, actual_filled, krw_amount);
            Order korean_order = execute_korean_buy(signal.korean_exchange, position.symbol, actual_filled, krw_amount);
            record_latency(LatencyStage::ReentryKoreanSubmitAck,
                           static_cast<int64_t>(korean_order.status),
                           0,
                           korean_order.quantity,
                           korean_order.average_price);
            dispatch_fill_query(FillQueryTask{
                FillQueryTask::Kind::Korean,
                signal.korean_exchange,
                position.symbol,
                trace_id,
                trace_start_ns,
                trace_symbol,
                LatencyStage::ReentryKoreanFillQueryStart,
                LatencyStage::ReentryKoreanFillWorkerStart,
                LatencyStage::ReentryKoreanFillDone,
                &korean_order,
                &fill_done,
            });
            fill_done.wait();

            if (korean_order.status == OrderStatus::Filled) {
                if (foreign_order.filled_quantity > 0) actual_filled = foreign_order.filled_quantity;

                double short_price = foreign_order.average_price;
                if (short_price <= 0) {
                    short_price = current_foreign_bid;
                    Logger::warn("[ADAPTIVE-REENTRY] No fill price from Bybit, using cache {:.8f}", short_price);
                }
                double buy_price = korean_order.average_price;
                if (buy_price <= 0) {
                    buy_price = current_korean_ask;
                    Logger::warn("[ADAPTIVE-REENTRY] No fill price from Bithumb, using cache {:.2f}", buy_price);
                }
                double actual_korean_cost = actual_filled * buy_price;

                remaining_amount += actual_filled;
                total_korean_cost += actual_korean_cost;
                total_foreign_value += actual_filled * short_price;

                Logger::info("[ADAPTIVE-REENTRY] {} +{:.8f} coins, remaining: {:.8f}/{:.8f}, entry_pm: {:.4f}%, buy_price: {:.2f}",
                             position.symbol.to_string(), actual_filled, remaining_amount, original_amount, entry_premium, buy_price);
                record_latency(LatencyStage::ReentryCompleted, 0, 0, actual_filled, remaining_amount);

                // Log re-entry split to CSV (actual fill price)
                append_entry_split_log(position.symbol, actual_filled, buy_price,
                                       short_price, usdt_rate,
                                       remaining_amount * current_foreign_bid,
                                       original_amount * current_foreign_bid, entry_premium);

                // Update position file
                if (on_position_update_) {
                    Position snap = position;
                    snap.korean_amount = remaining_amount;
                    snap.foreign_amount = remaining_amount;
                    snap.korean_entry_price = total_korean_cost / remaining_amount;
                    snap.foreign_entry_price = total_foreign_value / remaining_amount;
                    snap.entry_premium = calculate_effective_entry_pm(usdt_rate);
                    snap.realized_pnl_krw = realized_pnl_krw;
                    on_position_update_(&snap);
                }
            } else {
                Logger::error("[ADAPTIVE-REENTRY] Korean BUY failed, rolling back Bybit spot-margin short");
                record_latency(LatencyStage::Error, static_cast<int64_t>(korean_order.status));
                Order rollback = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                if (rollback.status != OrderStatus::Filled) {
                    Position mismatch = position;
                    mismatch.korean_amount = remaining_amount;
                    mismatch.foreign_amount = remaining_amount + actual_filled;
                    mismatch.korean_entry_price = remaining_amount > 0 ? (total_korean_cost / remaining_amount) : 0.0;
                    double short_price = foreign_order.average_price > 0 ? foreign_order.average_price : current_foreign_bid;
                    double foreign_value = total_foreign_value + (actual_filled * short_price);
                    mismatch.foreign_entry_price = mismatch.foreign_amount > 0 ? (foreign_value / mismatch.foreign_amount) : 0.0;
                    mismatch.entry_premium = calculate_effective_entry_pm(usdt_rate);
                    mismatch.realized_pnl_krw = realized_pnl_krw;
                    mismatch.is_active = mismatch.korean_amount > 0.0 || mismatch.foreign_amount > 0.0;

                    trigger_critical_stop(
                        "[ADAPTIVE-REENTRY] Bybit rollback failed after Korean BUY rejection (manual intervention required)",
                        mismatch
                    );
                    return result;
                }
            }

        } else {
            // ==================== WAIT ====================
            if (engine_) {
                uint64_t seq = engine_->get_update_seq();
                engine_->wait_for_update(seq, std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
            }
            continue;
        }

        wait_for_next_market_update(update_seq_before_trade);
    }

    // Shutdown interrupted exit - return with remaining position
    if (remaining_amount > 0) {
        Logger::warn("[ADAPTIVE-EXIT] Shutdown during exit, remaining: {:.8f} coins", remaining_amount);
        result.success = false;
        result.position.korean_amount = remaining_amount;
        result.position.foreign_amount = remaining_amount;
        if (remaining_amount > 0) {
            result.position.korean_entry_price = total_korean_cost / remaining_amount;
            result.position.foreign_entry_price = total_foreign_value / remaining_amount;
            result.position.entry_premium = calculate_effective_entry_pm(last_usdt_rate);
        }
        result.position.is_active = true;
        result.position.realized_pnl_krw = realized_pnl_krw;
        return result;
    }

    // Fully exited
    result.success = true;
    result.position.exit_time = std::chrono::system_clock::now();
    result.position.exit_premium = signal.premium;
    result.position.is_active = false;
    result.position.realized_pnl_krw = realized_pnl_krw;
    if (total_exited_amount > 0) {
        result.position.korean_exit_price = total_korean_proceeds / total_exited_amount;
        result.position.foreign_exit_price = total_foreign_cost / total_exited_amount;
    }
    record_latency(LatencyStage::ExitCompleted, 0, 0, total_exited_amount, realized_pnl_krw);

    Logger::info("[ADAPTIVE] {} fully exited. Total P&L: {:.0f} KRW",
                 position.symbol.to_string(), realized_pnl_krw);

    return result;
}

bool OrderManager::prepare_bybit_shorting(const std::vector<SymbolId>& symbols) {
    auto bybit = get_bybit_exchange();
    if (!bybit) {
        Logger::warn("Bybit preparation skipped: exchange not available");
        return false;
    }

    std::optional<SymbolId> sample_symbol;
    if (!symbols.empty()) {
        sample_symbol = SymbolId(symbols.front().get_base(), "USDT");
    }
    if (!sample_symbol) {
        sample_symbol = SymbolId("BTC", "USDT");
    }

    const bool ready = bybit->prepare_shorting(*sample_symbol);
    if (ready) {
        Logger::info("Prepared Bybit spot-margin shorting for {} symbols", symbols.size());
    } else {
        Logger::error("Failed to prepare Bybit spot-margin shorting");
    }
    return ready;
}

Order OrderManager::execute_korean_buy(Exchange ex, const SymbolId& symbol, double quantity, double krw_amount) {
    auto korean_ex = get_korean_exchange(ex);
    if (!korean_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    if (ex == Exchange::Bithumb) {
        if (bithumb_exchange_) {
            return bithumb_exchange_->place_market_buy_quantity(symbol, quantity);
        }
    }

    return korean_ex->place_market_buy_cost(symbol, krw_amount);
}

Order OrderManager::execute_foreign_short(Exchange /*ex*/, const SymbolId& symbol, double quantity) {
    auto short_ex = get_bybit_exchange();
    if (!short_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    return short_ex->open_short(symbol, quantity);
}

Order OrderManager::execute_korean_sell(Exchange ex, const SymbolId& symbol, double quantity) {
    auto korean_ex = get_korean_exchange(ex);
    if (!korean_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    return korean_ex->place_market_order(symbol, Side::Sell, quantity);
}

Order OrderManager::execute_foreign_cover(Exchange /*ex*/, const SymbolId& symbol, double quantity) {
    auto short_ex = get_bybit_exchange();
    if (!short_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    return short_ex->close_short(symbol, quantity);
}

void OrderManager::query_foreign_fill(Exchange ex, Order& order) {
    if (order.order_id_str.empty() || order.status != OrderStatus::Filled) return;
    if (ex == Exchange::Bybit && bybit_exchange_) {
        bybit_exchange_->query_order_fill(order.order_id_str, order);
    }
}

void OrderManager::query_korean_fill(Exchange ex, const SymbolId& symbol, Order& order) {
    if (order.order_id_str.empty() || order.status != OrderStatus::Filled) return;
    if (ex == Exchange::Bithumb) {
        if (bithumb_exchange_) {
            bithumb_exchange_->query_order_detail(order.order_id_str, symbol, order);
        }
    }
}

void OrderManager::ensure_fill_query_executor_started() {
    std::call_once(fill_query_executor_start_once_, [this]() {
        auto thread_config = opt::ThreadConfig::optimal();
        LifecycleExecutorOptions options;
        options.worker_count = 2;
        options.push_spin_count = 128;
        options.empty_spin_count = 1024;
        options.idle_wait = std::chrono::microseconds(100);
        fill_query_executor_.start(options, [thread_config](std::size_t worker_index) {
            const int total_cores = static_cast<int>(std::thread::hardware_concurrency());
            int core_id = thread_config.execution_core;
            if (core_id >= 0 && total_cores > 0) {
                core_id = std::min(core_id + static_cast<int>(TradingConfig::MAX_POSITIONS) +
                                       static_cast<int>(worker_index),
                                   total_cores - 1);
                if (opt::pin_to_core(core_id)) {
                    Logger::info("Fill query worker {} pinned to core {}", worker_index, core_id);
                }
            }
            if (opt::set_realtime_priority()) {
                Logger::info("Fill query worker {} set to realtime priority", worker_index);
            }
        });
    });
}

void OrderManager::handle_fill_query(FillQueryTask&& task, std::size_t worker_index) {
    auto countdown = [&]() {
        if (task.done) {
            task.done->count_down();
        }
    };
    auto record_stage = [&](LatencyStage stage,
                            int64_t aux0 = 0,
                            int64_t aux1 = 0,
                            double value0 = 0.0,
                            double value1 = 0.0) {
        if (task.trace_id == 0) {
            return;
        }
        LatencyProbe::instance().record(
            task.trace_id,
            task.trace_symbol,
            stage,
            task.trace_start_ns,
            aux0,
            aux1,
            value0,
            value1);
    };

    if (!task.order) {
        countdown();
        return;
    }

    const int64_t worker_aux =
        worker_index == static_cast<std::size_t>(-1) ? -1 : static_cast<int64_t>(worker_index);
    record_stage(task.worker_stage, worker_aux, static_cast<int64_t>(task.kind));

    if (task.kind == FillQueryTask::Kind::Foreign) {
        query_foreign_fill(task.ex, *task.order);
    } else {
        query_korean_fill(task.ex, task.symbol, *task.order);
    }
    record_stage(task.done_stage,
                 static_cast<int64_t>(task.order->status),
                 worker_aux,
                 task.order->filled_quantity,
                 task.order->average_price);
    countdown();
}

void OrderManager::dispatch_fill_query(FillQueryTask task) {
    ensure_fill_query_executor_started();
    if (task.trace_id != 0) {
        LatencyProbe::instance().record(
            task.trace_id,
            task.trace_symbol,
            task.enqueue_stage,
            task.trace_start_ns,
            static_cast<int64_t>(fill_query_executor_.pending()),
            static_cast<int64_t>(task.kind),
            task.order ? task.order->quantity : 0.0,
            0.0);
    }
    if (!fill_query_executor_.enqueue(task)) {
        handle_fill_query(std::move(task), static_cast<std::size_t>(-1));
    }
}

void OrderManager::wait_for_next_market_update(uint64_t update_seq_before_trade) {
    if (engine_) {
        engine_->wait_for_update(update_seq_before_trade,
                                 std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
    }
}

void OrderManager::refresh_external_positions(const std::vector<SymbolId>& symbols,
                                               const std::unordered_set<SymbolId>& bot_managed) {
    Logger::info("Building external position blacklist ({} symbols, {} bot-managed)...",
                 symbols.size(), bot_managed.size());

    std::unordered_set<SymbolId> new_blacklist;

    // Check Bithumb spot balances for all trading symbols
    auto bithumb = get_korean_exchange(Exchange::Bithumb);
    if (bithumb) {
        for (const auto& sym : symbols) {
            if (bot_managed.count(sym)) continue;  // Skip bot's own positions
            double balance = bithumb->get_balance(std::string(sym.get_base()));
            if (balance > 0.0001) {  // Ignore dust
                new_blacklist.insert(sym);
                Logger::warn("[BLACKLIST] {} - Bithumb spot balance: {:.6f}",
                             sym.to_string(), balance);
            }
        }
    }

    // Check Bybit spot-margin short liabilities
    auto bybit = get_bybit_exchange();
    if (bybit) {
        auto positions = bybit->get_short_positions();
        for (const auto& pos : positions) {
            if (pos.foreign_amount > 0.0001) {  // Ignore dust
                SymbolId krw_symbol(pos.symbol.get_base(), "KRW");
                if (bot_managed.count(krw_symbol)) continue;  // Skip bot's own positions
                new_blacklist.insert(krw_symbol);
                Logger::warn("[BLACKLIST] {} - Bybit spot-margin liability: {:.6f}",
                             krw_symbol.to_string(), pos.foreign_amount);
            }
        }
    }

    // Update blacklist atomically
    {
        std::lock_guard lock(blacklist_mutex_);
        external_position_blacklist_ = std::move(new_blacklist);
    }

    Logger::info("External position blacklist built: {} symbols blocked",
                 external_position_blacklist_.size());
}

bool OrderManager::is_safe_to_trade(const SymbolId& symbol, Exchange /*korean_ex*/, Exchange /*foreign_ex*/) {
    // O(1) blacklist lookup - no API calls!
    std::lock_guard lock(blacklist_mutex_);

    if (external_position_blacklist_.count(symbol)) {
        Logger::warn("[SAFETY] Skipping {}: In external position blacklist",
                     symbol.to_string());
        return false;
    }

    return true;
}
} // namespace kimp::execution
