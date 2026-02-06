#include "kimp/execution/order_manager.hpp"
#include "kimp/core/logger.hpp"

#include <thread>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <ctime>
#include <cstdio>

namespace {

std::string format_time(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf,
                  static_cast<long long>(ms.count()));
    return std::string(out);
}

void append_exit_split_log(const kimp::Position& pos,
                           double split_qty,
                           double korean_exit_price,
                           double foreign_exit_price,
                           double pnl_krw,
                           double usdt_rate,
                           bool final_split) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    std::filesystem::create_directories("trade_logs");
    const std::string path = "trade_logs/exit_splits.csv";

    bool need_header = !std::filesystem::exists(path) ||
                       std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    if (need_header) {
        out << "exit_time,symbol,split_qty,korean_entry_price,korean_exit_price,"
               "foreign_entry_price,foreign_exit_price,pnl_krw,pnl_usd,usdt_rate,final_split\n";
    }

    double pnl_usd = usdt_rate > 0.0 ? (pnl_krw / usdt_rate) : 0.0;
    out << format_time(std::chrono::system_clock::now()) << ','
        << pos.symbol.to_string() << ','
        << std::fixed << std::setprecision(8) << split_qty << ','
        << std::fixed << std::setprecision(2) << pos.korean_entry_price << ','
        << std::fixed << std::setprecision(2) << korean_exit_price << ','
        << std::fixed << std::setprecision(6) << pos.foreign_entry_price << ','
        << std::fixed << std::setprecision(6) << foreign_exit_price << ','
        << std::fixed << std::setprecision(2) << pnl_krw << ','
        << std::fixed << std::setprecision(2) << pnl_usd << ','
        << std::fixed << std::setprecision(2) << usdt_rate << ','
        << (final_split ? "1" : "0")
        << '\n';
}

void append_entry_split_log(const kimp::SymbolId& symbol,
                            double split_qty,
                            double korean_buy_price,
                            double foreign_short_price,
                            double usdt_rate,
                            double held_value_usd,
                            double position_target_usd,
                            double premium) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    std::filesystem::create_directories("trade_logs");
    const std::string path = "trade_logs/entry_splits.csv";

    bool need_header = !std::filesystem::exists(path) ||
                       std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;

    if (need_header) {
        out << "entry_time,symbol,split_qty,korean_buy_price,foreign_short_price,"
               "usdt_rate,held_value_usd,position_target_usd,premium\n";
    }

    out << format_time(std::chrono::system_clock::now()) << ','
        << symbol.to_string() << ','
        << std::fixed << std::setprecision(8) << split_qty << ','
        << std::fixed << std::setprecision(2) << korean_buy_price << ','
        << std::fixed << std::setprecision(8) << foreign_short_price << ','
        << std::fixed << std::setprecision(2) << usdt_rate << ','
        << std::fixed << std::setprecision(2) << held_value_usd << ','
        << std::fixed << std::setprecision(2) << position_target_usd << ','
        << std::fixed << std::setprecision(4) << premium
        << '\n';
}

} // namespace

namespace kimp::execution {

OrderManager::~OrderManager() {
    running_ = false;
    if (exec_thread_.joinable()) {
        exec_thread_.join();
    }
}

void OrderManager::set_exchange(Exchange ex, ExchangePtr exchange) {
    exchanges_[static_cast<size_t>(ex)] = std::move(exchange);
}

OrderManager::KoreanExchangePtr OrderManager::get_korean_exchange(Exchange ex) {
    auto ptr = exchanges_[static_cast<size_t>(ex)];
    if (!ptr) return nullptr;

    if (ex == Exchange::Upbit) {
        return std::dynamic_pointer_cast<exchange::upbit::UpbitExchange>(ptr);
    } else if (ex == Exchange::Bithumb) {
        return std::dynamic_pointer_cast<exchange::bithumb::BithumbExchange>(ptr);
    }
    return nullptr;
}

OrderManager::FuturesExchangePtr OrderManager::get_futures_exchange(Exchange ex) {
    auto ptr = exchanges_[static_cast<size_t>(ex)];
    if (!ptr) return nullptr;

    if (ex == Exchange::Bybit) {
        return std::dynamic_pointer_cast<exchange::bybit::BybitExchange>(ptr);
    } else if (ex == Exchange::GateIO) {
        return std::dynamic_pointer_cast<exchange::gateio::GateIOExchange>(ptr);
    }
    return nullptr;
}

ExecutionResult OrderManager::execute_entry(const ArbitrageSignal& signal) {
    return execute_split_entry(signal);
}

ExecutionResult OrderManager::execute_entry_futures_first(const ArbitrageSignal& signal) {
    ExecutionResult result;
    result.position.symbol = signal.symbol;
    result.position.korean_exchange = signal.korean_exchange;
    result.position.foreign_exchange = signal.foreign_exchange;
    result.position.entry_time = std::chrono::system_clock::now();
    result.position.entry_premium = signal.premium;

    // Dynamic position size based on current capital (복리 성장)
    double position_size_usd = engine_ ? engine_->get_position_size_usd() : TradingConfig::POSITION_SIZE_USD;
    result.position.position_size_usd = position_size_usd;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

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
    // Leverage is pre-set to 1x at startup to avoid per-trade REST latency

    // =========================================================================
    // ADAPTIVE SPLIT EXECUTION: 진입/청산 유기적 전환
    // - premium <= -0.75%: entry split ($25 short + buy)
    // - premium >= +0.34%: exit split ($25 cover + sell, <$50 close all)
    // - between: wait for market update
    // Loop ends when: fully entered ($250) or fully exited (0 coins)
    // =========================================================================
    double held_amount = 0.0;
    double total_korean_cost = 0.0;
    double total_foreign_value = 0.0;
    double realized_pnl_krw = 0.0;

    while (running_.load(std::memory_order_acquire)) {
        // Get fresh prices from cache
        double current_foreign_bid = signal.foreign_bid;
        double current_foreign_ask = 0.0;
        double current_korean_ask = signal.korean_ask;
        double current_korean_bid = 0.0;
        double usdt_rate = TradingConfig::DEFAULT_USDT_KRW;
        if (engine_) {
            auto& cache = engine_->get_price_cache();
            auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
            auto korean_price = cache.get_price(signal.korean_exchange, signal.symbol);
            if (foreign_price.valid && foreign_price.bid > 0) current_foreign_bid = foreign_price.bid;
            if (foreign_price.valid && foreign_price.ask > 0) current_foreign_ask = foreign_price.ask;
            if (korean_price.valid && korean_price.ask > 0) current_korean_ask = korean_price.ask;
            if (korean_price.valid && korean_price.bid > 0) current_korean_bid = korean_price.bid;
            double bithumb_usdt = cache.get_usdt_krw(Exchange::Bithumb);
            if (bithumb_usdt > 0) usdt_rate = bithumb_usdt;
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

        double held_value_usd = held_amount * current_foreign_bid;

        // Dynamic exit threshold from actual average entry prices
        double dynamic_exit_threshold = TradingConfig::EXIT_PREMIUM_THRESHOLD;  // fallback
        if (held_amount > 0 && total_foreign_value > 0 && usdt_rate > 0) {
            double avg_foreign_entry = total_foreign_value / held_amount;
            double avg_korean_entry = total_korean_cost / held_amount;
            double effective_entry_pm = ((avg_korean_entry - avg_foreign_entry * usdt_rate)
                                         / (avg_foreign_entry * usdt_rate)) * 100.0;
            dynamic_exit_threshold = effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD;
        }

        if (entry_premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD && held_value_usd < position_size_usd) {
            // ==================== ENTRY SPLIT ====================
            double order_size_usd = std::min(TradingConfig::ORDER_SIZE_USD, position_size_usd - held_value_usd);
            double coin_amount = order_size_usd / current_foreign_bid;

            // SHORT futures first
            Order foreign_order = execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-ENTRY] Futures SHORT failed, retrying next cycle");
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // Use foreign_order.quantity (lot-size normalized) as fallback, NOT coin_amount (pre-normalization)
            double actual_filled = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
            double short_price = foreign_order.average_price;
            if (short_price <= 0) {
                short_price = current_foreign_bid;
                Logger::error("[ADAPTIVE-ENTRY] CRITICAL: No fill price from Bybit after retries! "
                              "Using cache {:.8f} — P&L will be inaccurate", short_price);
            }
            double krw_amount = actual_filled * current_korean_ask;

            if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
                Logger::warn("[ADAPTIVE-ENTRY] Order too small ({:.0f} KRW), rolling back futures", krw_amount);
                execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // BUY Korean spot with exact same amount
            Order korean_order = execute_korean_buy(signal.korean_exchange, signal.symbol, actual_filled, krw_amount);

            if (korean_order.status == OrderStatus::Filled) {
                double buy_price = korean_order.average_price;
                if (buy_price <= 0) {
                    buy_price = current_korean_ask;
                    Logger::error("[ADAPTIVE-ENTRY] CRITICAL: No fill price from Bithumb after retries! "
                                  "Using cache {:.2f} — P&L will be inaccurate", buy_price);
                }
                double actual_korean_cost = actual_filled * buy_price;

                held_amount += actual_filled;
                total_korean_cost += actual_korean_cost;
                total_foreign_value += actual_filled * short_price;

                double new_held_value = held_amount * current_foreign_bid;
                Logger::info("[ADAPTIVE-ENTRY] {} +{:.8f} coins (${:.2f}), held: ${:.2f}/${:.2f}, entry_pm: {:.4f}%, buy_price: {:.2f}",
                             signal.symbol.to_string(), actual_filled, order_size_usd,
                             new_held_value, position_size_usd, entry_premium, buy_price);

                // Log entry split to CSV (actual fill price)
                append_entry_split_log(signal.symbol, actual_filled, buy_price,
                                       short_price, usdt_rate, new_held_value,
                                       position_size_usd, entry_premium);

                // Save position state for crash recovery
                if (on_position_update_) {
                    Position snap;
                    snap.symbol = signal.symbol;
                    snap.korean_exchange = signal.korean_exchange;
                    snap.foreign_exchange = signal.foreign_exchange;
                    snap.entry_time = result.position.entry_time;
                    snap.entry_premium = signal.premium;
                    snap.position_size_usd = position_size_usd;
                    snap.korean_amount = held_amount;
                    snap.foreign_amount = held_amount;
                    snap.korean_entry_price = total_korean_cost / held_amount;
                    snap.foreign_entry_price = total_foreign_value / held_amount;
                    snap.realized_pnl_krw = realized_pnl_krw;
                    snap.is_active = true;
                    on_position_update_(&snap);
                }

                if (new_held_value >= position_size_usd) {
                    Logger::info("[ADAPTIVE-ENTRY] {} fully entered: ${:.2f}",
                                 signal.symbol.to_string(), new_held_value);
                    break;
                }
            } else {
                Logger::error("[ADAPTIVE-ENTRY] Korean BUY failed, rolling back futures SHORT");
                execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
            }

        } else if (exit_premium >= dynamic_exit_threshold && held_amount > 0) {
            // ==================== EXIT SPLIT ====================
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

            // COVER futures first
            Order foreign_order = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, exit_coin_amount);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-EXIT] Futures COVER failed during entry, retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // Use foreign_order.quantity (lot-size normalized) as fallback for hedge accuracy
            double actual_covered = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
            double cover_price = foreign_order.average_price;
            if (cover_price <= 0) {
                cover_price = current_foreign_ask;
                Logger::error("[ADAPTIVE-EXIT] CRITICAL: No fill price from Bybit after retries! "
                              "Using cache {:.8f} — P&L will be inaccurate", cover_price);
            }

            // SELL Korean spot
            Order korean_order = execute_korean_sell(signal.korean_exchange, signal.symbol, actual_covered);

            if (korean_order.status == OrderStatus::Filled) {
                double sell_price = korean_order.average_price;
                if (sell_price <= 0) {
                    sell_price = current_korean_bid;
                    Logger::error("[ADAPTIVE-EXIT] CRITICAL: No sell fill price from Bithumb after retries! "
                                  "Using cache {:.2f} — P&L will be inaccurate", sell_price);
                }

                // P&L using average entry prices
                double avg_korean_entry = total_korean_cost / held_amount;
                double avg_foreign_entry = total_foreign_value / held_amount;
                double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                realized_pnl_krw += split_pnl_krw;

                // Reduce costs proportionally
                double exit_ratio = actual_covered / held_amount;
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

                // Update position file after exit split
                if (on_position_update_) {
                    if (held_amount > 0) {
                        Position snap;
                        snap.symbol = signal.symbol;
                        snap.korean_exchange = signal.korean_exchange;
                        snap.foreign_exchange = signal.foreign_exchange;
                        snap.entry_time = result.position.entry_time;
                        snap.entry_premium = signal.premium;
                        snap.position_size_usd = position_size_usd;
                        snap.korean_amount = held_amount;
                        snap.foreign_amount = held_amount;
                        snap.korean_entry_price = total_korean_cost / held_amount;
                        snap.foreign_entry_price = total_foreign_value / held_amount;
                        snap.realized_pnl_krw = realized_pnl_krw;
                        snap.is_active = true;
                        on_position_update_(&snap);
                    } else {
                        on_position_update_(nullptr);
                    }
                }

                if (held_amount <= 0) {
                    // Fully exited during entry phase
                    Logger::info("[ADAPTIVE] {} fully exited during entry. Total P&L: {:.0f} KRW",
                                 signal.symbol.to_string(), realized_pnl_krw);
                    result.success = false;
                    result.position.realized_pnl_krw = realized_pnl_krw;
                    result.position.is_active = false;
                    return result;
                }
            } else {
                // SELL failed after COVER — retry to avoid unhedged state
                Logger::error("[ADAPTIVE-EXIT] Korean SELL failed after COVER, retrying... Unhedged: {:.8f} coins", actual_covered);
                for (int retry = 1; retry <= 5; ++retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300 * retry));
                    Logger::warn("[ADAPTIVE-EXIT] SELL retry {}/5 for {:.8f} coins", retry, actual_covered);
                    korean_order = execute_korean_sell(signal.korean_exchange, signal.symbol, actual_covered);
                    if (korean_order.status == OrderStatus::Filled) {
                        double sell_price = korean_order.average_price;
                        if (sell_price <= 0) sell_price = current_korean_bid;

                        double avg_korean_entry = total_korean_cost / held_amount;
                        double avg_foreign_entry = total_foreign_value / held_amount;
                        double korean_pnl_krw = (sell_price - avg_korean_entry) * actual_covered;
                        double foreign_pnl_usd = (avg_foreign_entry - cover_price) * actual_covered;
                        double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
                        double split_pnl_krw = korean_pnl_krw + foreign_pnl_krw;
                        realized_pnl_krw += split_pnl_krw;

                        double exit_ratio = actual_covered / held_amount;
                        total_korean_cost *= (1.0 - exit_ratio);
                        total_foreign_value *= (1.0 - exit_ratio);
                        held_amount -= actual_covered;

                        Logger::info("[ADAPTIVE-EXIT] SELL retry succeeded: {:.8f} coins @ {:.2f}, P&L: {:.0f} KRW",
                                     actual_covered, sell_price, split_pnl_krw);
                        break;
                    }
                }
                if (korean_order.status != OrderStatus::Filled) {
                    Logger::error("[ADAPTIVE-EXIT] CRITICAL: Korean SELL failed after 5 retries! "
                                  "UNHEDGED {:.8f} coins — MANUAL INTERVENTION REQUIRED", actual_covered);
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

        // Sleep between splits
        std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
    }

    // Shutdown interrupted entry - return partial position if any
    if (!running_.load(std::memory_order_acquire) && held_amount > 0) {
        Logger::warn("[ADAPTIVE-ENTRY] Shutdown during entry, saving partial position: {:.8f} coins",
                     held_amount);
        result.success = true;
        result.position.korean_amount = held_amount;
        result.position.foreign_amount = held_amount;
        result.position.korean_entry_price = total_korean_cost / held_amount;
        result.position.foreign_entry_price = total_foreign_value / held_amount;
        result.position.is_active = true;
        result.position.realized_pnl_krw = realized_pnl_krw;
        result.korean_filled_amount = held_amount;
        result.foreign_filled_amount = held_amount;
        return result;
    }

    // Fully entered - build position
    result.success = true;
    result.position.korean_amount = held_amount;
    result.position.foreign_amount = held_amount;
    result.position.korean_entry_price = total_korean_cost / held_amount;
    result.position.foreign_entry_price = total_foreign_value / held_amount;
    result.position.is_active = true;
    result.position.realized_pnl_krw = realized_pnl_krw;
    result.korean_filled_amount = held_amount;
    result.foreign_filled_amount = held_amount;

    return result;
}

ExecutionResult OrderManager::execute_exit_futures_first(const ExitSignal& signal, const Position& position) {
    ExecutionResult result;
    result.position = position;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Exit failed: {}", result.error_message);
        return result;
    }

    SymbolId foreign_symbol(position.symbol.get_base(), "USDT");

    // =========================================================================
    // ADAPTIVE SPLIT EXECUTION: 청산/재진입 유기적 전환
    // - exit_pm >= dynamic threshold: exit split ($25 cover + sell, <$50 close all)
    // - entry_pm <= -0.75% && partially exited: re-entry split ($25 short + buy)
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

    while (remaining_amount > 0 && running_.load(std::memory_order_acquire)) {
        // Get fresh prices from cache
        double current_foreign_bid = 0.0;
        double current_foreign_ask = signal.foreign_ask;
        double current_korean_ask = 0.0;
        double current_korean_bid = signal.korean_bid;
        double usdt_rate = signal.usdt_krw_rate > 0 ? signal.usdt_krw_rate : TradingConfig::DEFAULT_USDT_KRW;
        if (engine_) {
            auto& cache = engine_->get_price_cache();
            auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
            auto korean_price = cache.get_price(signal.korean_exchange, position.symbol);
            if (foreign_price.valid && foreign_price.bid > 0) current_foreign_bid = foreign_price.bid;
            if (foreign_price.valid && foreign_price.ask > 0) current_foreign_ask = foreign_price.ask;
            if (korean_price.valid && korean_price.ask > 0) current_korean_ask = korean_price.ask;
            if (korean_price.valid && korean_price.bid > 0) current_korean_bid = korean_price.bid;
            double bithumb_usdt = cache.get_usdt_krw(Exchange::Bithumb);
            if (bithumb_usdt > 0) usdt_rate = bithumb_usdt;
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

        // Dynamic exit threshold from actual average entry prices
        double dynamic_exit_threshold = TradingConfig::EXIT_PREMIUM_THRESHOLD;  // fallback
        if (remaining_amount > 0 && total_foreign_value > 0 && usdt_rate > 0) {
            double avg_foreign_entry = total_foreign_value / remaining_amount;
            double avg_korean_entry = total_korean_cost / remaining_amount;
            double effective_entry_pm = ((avg_korean_entry - avg_foreign_entry * usdt_rate)
                                         / (avg_foreign_entry * usdt_rate)) * 100.0;
            dynamic_exit_threshold = effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD;
        }

        if (exit_premium >= dynamic_exit_threshold) {
            // ==================== EXIT SPLIT ====================
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

            // COVER futures first
            Order foreign_order = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, exit_coin_amount);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-EXIT] Futures COVER failed, retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // Use foreign_order.quantity (lot-size normalized) as fallback for hedge accuracy
            double actual_covered = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
            double cover_price = foreign_order.average_price;
            if (cover_price <= 0) {
                cover_price = current_foreign_ask;
                Logger::error("[EXIT] CRITICAL: No cover fill price from Bybit after retries! "
                              "Using cache {:.8f} — P&L will be inaccurate", cover_price);
            }

            // SELL Korean spot
            Order korean_order = execute_korean_sell(signal.korean_exchange, position.symbol, actual_covered);

            if (korean_order.status == OrderStatus::Filled) {
                double sell_price = korean_order.average_price;
                if (sell_price <= 0) {
                    sell_price = current_korean_bid;
                    Logger::error("[EXIT] CRITICAL: No sell fill price from Bithumb after retries! "
                                  "Using cache {:.2f} — P&L will be inaccurate", sell_price);
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

                // Update position file
                if (on_position_update_) {
                    if (remaining_amount > 0) {
                        Position snap = position;
                        snap.korean_amount = remaining_amount;
                        snap.foreign_amount = remaining_amount;
                        snap.korean_entry_price = total_korean_cost / remaining_amount;
                        snap.foreign_entry_price = total_foreign_value / remaining_amount;
                        snap.realized_pnl_krw = realized_pnl_krw;
                        on_position_update_(&snap);
                    } else {
                        on_position_update_(nullptr);
                    }
                }
            } else {
                // SELL failed after COVER — retry to avoid unhedged state
                Logger::error("[EXIT] Korean SELL failed after COVER, retrying... Unhedged: {:.8f} coins", actual_covered);
                for (int retry = 1; retry <= 5; ++retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300 * retry));
                    Logger::warn("[EXIT] SELL retry {}/5 for {:.8f} coins", retry, actual_covered);
                    korean_order = execute_korean_sell(signal.korean_exchange, position.symbol, actual_covered);
                    if (korean_order.status == OrderStatus::Filled) {
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

                        Logger::info("[EXIT] SELL retry succeeded: {:.8f} coins @ {:.2f}, P&L: {:.0f} KRW",
                                     actual_covered, sell_price, split_pnl_krw);
                        break;
                    }
                }
                if (korean_order.status != OrderStatus::Filled) {
                    Logger::error("[EXIT] CRITICAL: Korean SELL failed after 5 retries! "
                                  "UNHEDGED {:.8f} coins — MANUAL INTERVENTION REQUIRED", actual_covered);
                }
            }

        } else if (entry_premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD && remaining_amount < original_amount) {
            // ==================== RE-ENTRY SPLIT ====================
            // Only re-enter up to original position size
            double max_reentry_coins = original_amount - remaining_amount;
            double reentry_usd = std::min(TradingConfig::ORDER_SIZE_USD, max_reentry_coins * current_foreign_bid);
            double coin_amount = reentry_usd / current_foreign_bid;

            // SHORT futures first
            Order foreign_order = execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);

            if (foreign_order.status != OrderStatus::Filled) {
                Logger::error("[ADAPTIVE-REENTRY] Futures SHORT failed, retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // Use foreign_order.quantity (lot-size normalized) as fallback for hedge accuracy
            double actual_filled = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
            double short_price = foreign_order.average_price;
            if (short_price <= 0) {
                short_price = current_foreign_bid;
                Logger::error("[ADAPTIVE-REENTRY] CRITICAL: No short fill price from Bybit after retries! "
                              "Using cache {:.8f} — P&L will be inaccurate", short_price);
            }
            double krw_amount = actual_filled * current_korean_ask;

            if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
                Logger::warn("[ADAPTIVE-REENTRY] Order too small, rolling back");
                execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
                std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                continue;
            }

            // BUY Korean spot
            Order korean_order = execute_korean_buy(signal.korean_exchange, position.symbol, actual_filled, krw_amount);

            if (korean_order.status == OrderStatus::Filled) {
                double buy_price = korean_order.average_price;
                if (buy_price <= 0) {
                    buy_price = current_korean_ask;
                    Logger::error("[ADAPTIVE-REENTRY] CRITICAL: No fill price from Bithumb after retries! "
                                  "Using cache {:.2f} — P&L will be inaccurate", buy_price);
                }
                double actual_korean_cost = actual_filled * buy_price;

                remaining_amount += actual_filled;
                total_korean_cost += actual_korean_cost;
                total_foreign_value += actual_filled * short_price;

                Logger::info("[ADAPTIVE-REENTRY] {} +{:.8f} coins, remaining: {:.8f}/{:.8f}, entry_pm: {:.4f}%, buy_price: {:.2f}",
                             position.symbol.to_string(), actual_filled, remaining_amount, original_amount, entry_premium, buy_price);

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
                    snap.realized_pnl_krw = realized_pnl_krw;
                    on_position_update_(&snap);
                }
            } else {
                Logger::error("[ADAPTIVE-REENTRY] Korean BUY failed, rolling back futures SHORT");
                execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
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

        // Sleep between splits
        std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
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

    Logger::info("[ADAPTIVE] {} fully exited. Total P&L: {:.0f} KRW",
                 position.symbol.to_string(), realized_pnl_krw);

    return result;
}

ExecutionResult OrderManager::execute_split_entry(const ArbitrageSignal& signal) {
    ExecutionResult result;
    result.position.symbol = signal.symbol;
    result.position.korean_exchange = signal.korean_exchange;
    result.position.foreign_exchange = signal.foreign_exchange;
    result.position.entry_time = std::chrono::system_clock::now();
    result.position.entry_premium = signal.premium;
    result.position.korean_entry_price = signal.korean_ask;
    result.position.foreign_entry_price = signal.foreign_bid;

    // Dynamic position size based on current capital (복리 성장)
    double position_size_usd = engine_ ? engine_->get_position_size_usd() : TradingConfig::POSITION_SIZE_USD;
    result.position.position_size_usd = position_size_usd;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Entry failed: {}", result.error_message);
        return result;
    }

    SymbolId foreign_symbol(signal.symbol.get_base(), "USDT");
    // Leverage is pre-set to 1x at startup to avoid per-trade REST latency

    double total_korean_amount = 0.0;
    double total_foreign_amount = 0.0;

    // Split order execution
    for (int i = 0; i < TradingConfig::SPLIT_ORDERS; ++i) {
        double order_size_usd = TradingConfig::ORDER_SIZE_USD;

        // Re-check premium condition before each split
        if (engine_) {
            double current_premium = engine_->calculate_premium(
                signal.symbol, signal.korean_exchange, signal.foreign_exchange);
            if (current_premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) {
                uint64_t seq = engine_->get_update_seq();
                while (current_premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) {
                    engine_->wait_for_update(seq, std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                    seq = engine_->get_update_seq();
                    current_premium = engine_->calculate_premium(
                        signal.symbol, signal.korean_exchange, signal.foreign_exchange);
                }
            }
        }

        // Calculate amounts
        double coin_amount = order_size_usd / signal.foreign_bid;
        double krw_amount = coin_amount * signal.korean_ask;

        // Ensure minimum order amount
        if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
            krw_amount = TradingConfig::MIN_ORDER_KRW;
            coin_amount = krw_amount / signal.korean_ask;
        }

        // Execute Korean buy and Foreign short in parallel
        std::future<Order> korean_future = std::async(std::launch::async, [&]() {
            return execute_korean_buy(signal.korean_exchange, signal.symbol, coin_amount, krw_amount);
        });

        std::future<Order> foreign_future = std::async(std::launch::async, [&]() {
            return execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);
        });

        // Wait for both
        Order korean_order = korean_future.get();
        Order foreign_order = foreign_future.get();

        // Check results
        bool korean_success = korean_order.status == OrderStatus::Filled;
        bool foreign_success = foreign_order.status == OrderStatus::Filled;

        if (korean_success && foreign_success) {
            // Both succeeded
            total_korean_amount += coin_amount;  // Approximate, ideally get from order response
            total_foreign_amount += coin_amount;
        } else if (korean_success && !foreign_success) {
            // Korean succeeded but foreign failed - ROLLBACK
            Logger::error("Foreign order failed, rolling back Korean buy");

            if (rollback_korean_buy(signal.korean_exchange, signal.symbol, coin_amount)) {
            } else {
                Logger::error("Rollback failed! Manual intervention required");
                result.error_message = "Rollback failed after partial fill";
                // Continue with what we have
            }
        } else if (!korean_success && foreign_success) {
            // Korean failed but foreign succeeded - close foreign position
            Logger::error("Korean order failed, closing foreign short");
            execute_foreign_cover(signal.foreign_exchange, foreign_symbol, coin_amount);
        } else {
            // Both failed
            Logger::error("Both orders failed for split {}", i + 1);
        }

        // Wait between split orders (except for last)
        if (i < TradingConfig::SPLIT_ORDERS - 1) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
        }
    }

    // Check if we got any fill
    if (total_korean_amount > 0 && total_foreign_amount > 0) {
        result.success = true;
        result.position.korean_amount = total_korean_amount;
        result.position.foreign_amount = total_foreign_amount;
        result.position.is_active = true;
        result.korean_filled_amount = total_korean_amount;
        result.foreign_filled_amount = total_foreign_amount;

    } else {
        result.error_message = "No fills received";
        Logger::error("Entry failed: {}", result.error_message);
    }

    return result;
}

void OrderManager::pre_set_leverage(const std::vector<SymbolId>& symbols) {
    auto bybit = get_futures_exchange(Exchange::Bybit);
    if (!bybit) {
        Logger::warn("Pre-set leverage skipped: Bybit exchange not available");
        return;
    }

    int ok = 0;
    int failed = 0;
    for (const auto& symbol : symbols) {
        SymbolId foreign_symbol(symbol.get_base(), "USDT");
        if (bybit->set_leverage(foreign_symbol, 1)) {
            ++ok;
        } else {
            ++failed;
        }
    }

    Logger::info("Pre-set leverage to 1x for {} symbols (ok={}, failed={})",
                 symbols.size(), ok, failed);
}

ExecutionResult OrderManager::execute_exit(const ExitSignal& signal, const Position& position) {
    ExecutionResult result;
    result.position = position;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Exit failed: {}", result.error_message);
        return result;
    }

    // Get actual balance from Korean exchange
    double actual_korean_amount = korean_ex->get_balance(std::string(position.symbol.get_base()));
    if (actual_korean_amount <= 0) {
        actual_korean_amount = position.korean_amount;
    }

    SymbolId foreign_symbol(position.symbol.get_base(), "USDT");

    // Execute in parallel
    std::future<Order> korean_future = std::async(std::launch::async, [&]() {
        return execute_korean_sell(signal.korean_exchange, position.symbol, actual_korean_amount);
    });

    std::future<Order> foreign_future = std::async(std::launch::async, [&]() {
        return execute_foreign_cover(signal.foreign_exchange, foreign_symbol, position.foreign_amount);
    });

    Order korean_order = korean_future.get();
    Order foreign_order = foreign_future.get();

    bool korean_success = korean_order.status == OrderStatus::Filled;
    bool foreign_success = foreign_order.status == OrderStatus::Filled;

    if (korean_success && foreign_success) {
        result.success = true;
        result.position.exit_time = std::chrono::system_clock::now();
        result.position.exit_premium = signal.premium;
        result.position.korean_exit_price = signal.korean_bid;
        result.position.foreign_exit_price = signal.foreign_ask;
        result.position.is_active = false;

        // Calculate P&L
        result.position.realized_pnl_krw = calculate_pnl(
            position, signal.korean_bid, signal.foreign_ask, signal.usdt_krw_rate);

    } else {
        result.error_message = "Exit orders failed";
        Logger::error("Exit failed: korean={} foreign={}",
                      korean_success ? "OK" : "FAILED",
                      foreign_success ? "OK" : "FAILED");
    }

    return result;
}

Order OrderManager::execute_korean_buy(Exchange ex, const SymbolId& symbol, double quantity, double krw_amount) {
    auto korean_ex = get_korean_exchange(ex);
    if (!korean_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    if (ex == Exchange::Bithumb) {
        auto bithumb = std::dynamic_pointer_cast<exchange::bithumb::BithumbExchange>(korean_ex);
        if (bithumb) {
            return bithumb->place_market_buy_quantity(symbol, quantity);
        }
    }

    return korean_ex->place_market_buy_cost(symbol, krw_amount);
}

Order OrderManager::execute_foreign_short(Exchange ex, const SymbolId& symbol, double quantity) {
    auto futures_ex = get_futures_exchange(ex);
    if (!futures_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    return futures_ex->open_short(symbol, quantity);
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

Order OrderManager::execute_foreign_cover(Exchange ex, const SymbolId& symbol, double quantity) {
    auto futures_ex = get_futures_exchange(ex);
    if (!futures_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
    }

    return futures_ex->close_short(symbol, quantity);
}

bool OrderManager::rollback_korean_buy(Exchange ex, const SymbolId& symbol, double quantity) {
    auto order = execute_korean_sell(ex, symbol, quantity);
    return order.status == OrderStatus::Filled;
}

double OrderManager::calculate_pnl(const Position& pos, double exit_korean_price,
                                    double exit_foreign_price, double usdt_rate) {
    // Korean spot P&L (long position)
    double korean_pnl_krw = (exit_korean_price - pos.korean_entry_price) * pos.korean_amount;

    // Foreign futures P&L (short position: sell at entry, buy at exit)
    double foreign_pnl_usd = (pos.foreign_entry_price - exit_foreign_price) * pos.foreign_amount;
    double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;

    double total_pnl = korean_pnl_krw + foreign_pnl_krw;

    Logger::debug("P&L calculation: korean={:.0f} KRW, foreign={:.2f} USD ({:.0f} KRW), total={:.0f} KRW",
                  korean_pnl_krw, foreign_pnl_usd, foreign_pnl_krw, total_pnl);

    return total_pnl;
}

void OrderManager::refresh_external_positions(const std::vector<SymbolId>& symbols) {
    Logger::info("Building external position blacklist ({} symbols)...", symbols.size());

    std::unordered_set<SymbolId> new_blacklist;

    // Check Bithumb spot balances for all trading symbols
    auto bithumb = get_korean_exchange(Exchange::Bithumb);
    if (bithumb) {
        for (const auto& sym : symbols) {
            double balance = bithumb->get_balance(std::string(sym.get_base()));
            if (balance > 0.0001) {  // Ignore dust
                new_blacklist.insert(sym);
                Logger::warn("[BLACKLIST] {} - Bithumb spot balance: {:.6f}",
                             sym.to_string(), balance);
            }
        }
    }

    // Check Bybit futures positions
    auto bybit = get_futures_exchange(Exchange::Bybit);
    if (bybit) {
        auto positions = bybit->get_positions();
        for (const auto& pos : positions) {
            if (pos.foreign_amount > 0.0001) {  // Ignore dust
                SymbolId krw_symbol(pos.symbol.get_base(), "KRW");
                new_blacklist.insert(krw_symbol);
                Logger::warn("[BLACKLIST] {} - Bybit futures position: {:.6f}",
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

bool OrderManager::is_safe_to_trade(const SymbolId& symbol, Exchange korean_ex, Exchange foreign_ex) {
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
