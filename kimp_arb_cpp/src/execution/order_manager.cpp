#include "kimp/execution/order_manager.hpp"
#include "kimp/core/logger.hpp"

#include <thread>
#include <chrono>
#include <cmath>

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
    Logger::info("Executing entry (parallel): {} premium={:.2f}%",
                 signal.symbol.to_string(), signal.premium);

    return execute_split_entry(signal);
}

ExecutionResult OrderManager::execute_entry_futures_first(const ArbitrageSignal& signal) {
    ExecutionResult result;
    result.position.symbol = signal.symbol;
    result.position.korean_exchange = signal.korean_exchange;
    result.position.foreign_exchange = signal.foreign_exchange;
    result.position.entry_time = std::chrono::system_clock::now();
    result.position.entry_premium = signal.premium;
    result.position.position_size_usd = TradingConfig::POSITION_SIZE_USD;

    Logger::info("Executing entry (futures-first): {} premium={:.2f}%",
                 signal.symbol.to_string(), signal.premium);

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Entry failed: {}", result.error_message);
        return result;
    }

    // Set leverage to 1x BEFORE trading
    SymbolId foreign_symbol(signal.symbol.get_base(), "USDT");
    foreign_ex->set_leverage(foreign_symbol, 1);
    Logger::info("Set leverage to 1x for {}", foreign_symbol.to_string());

    double total_korean_amount = 0.0;
    double total_foreign_amount = 0.0;
    double total_korean_cost = 0.0;
    double remaining_usd = TradingConfig::POSITION_SIZE_USD;

    // Split order execution - FUTURES FIRST with premium re-check
    for (int i = 0; i < TradingConfig::SPLIT_ORDERS && remaining_usd > 0; ++i) {
        // Calculate order size (handle last split remainder)
        double order_size_usd = std::min(TradingConfig::ORDER_SIZE_USD, remaining_usd);

        // Re-check premium condition before each split (except first)
        // Wait indefinitely until condition is met again
        if (i > 0 && engine_) {
            double current_premium = engine_->calculate_premium(
                signal.symbol, signal.korean_exchange, signal.foreign_exchange);

            if (current_premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) {
                Logger::info("Split {}: Premium={:.2f}% > {:.2f}%, waiting for entry condition...",
                             i + 1, current_premium, TradingConfig::ENTRY_PREMIUM_THRESHOLD);

                // Wait indefinitely until condition is met
                while (current_premium > TradingConfig::ENTRY_PREMIUM_THRESHOLD) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                    current_premium = engine_->calculate_premium(
                        signal.symbol, signal.korean_exchange, signal.foreign_exchange);
                }

                Logger::info("Split {}: Premium back to {:.2f}%, resuming entry",
                             i + 1, current_premium);
            }
        }

        // Get fresh prices for this split
        double current_foreign_bid = signal.foreign_bid;
        double current_korean_ask = signal.korean_ask;
        if (engine_) {
            auto& cache = engine_->get_price_cache();
            auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
            auto korean_price = cache.get_price(signal.korean_exchange, signal.symbol);
            if (foreign_price.valid && foreign_price.bid > 0) {
                current_foreign_bid = foreign_price.bid;
            }
            if (korean_price.valid && korean_price.ask > 0) {
                current_korean_ask = korean_price.ask;
            }
        }

        // Calculate coin amount for futures (based on USD value)
        double coin_amount = order_size_usd / current_foreign_bid;

        Logger::info("Split {}/{}: SHORT {} {} on Bybit (${:.0f})",
                     i + 1, TradingConfig::SPLIT_ORDERS, coin_amount,
                     signal.symbol.get_base(), order_size_usd);

        // STEP 1: Execute futures SHORT first
        Order foreign_order = execute_foreign_short(signal.foreign_exchange, foreign_symbol, coin_amount);

        if (foreign_order.status != OrderStatus::Filled) {
            Logger::error("Futures SHORT failed for split {}, skipping", i + 1);
            continue;  // Skip this split, try next
        }

        // Get actual filled amount from futures order
        double actual_filled = foreign_order.filled_quantity > 0
            ? foreign_order.filled_quantity
            : coin_amount;
        double actual_price = foreign_order.average_price > 0
            ? foreign_order.average_price
            : current_foreign_bid;

        Logger::info("Futures SHORT filled: {} coins at ${:.2f}", actual_filled, actual_price);

        // STEP 2: Execute Korean BUY with EXACT same amount
        double krw_amount = actual_filled * current_korean_ask;

        // Ensure minimum order
        if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
            Logger::warn("Order too small ({:.0f} KRW), using minimum", krw_amount);
            krw_amount = TradingConfig::MIN_ORDER_KRW;
        }

        Logger::info("BUY {} {} on Bithumb ({:.0f} KRW)",
                     actual_filled, signal.symbol.get_base(), krw_amount);

        Order korean_order = execute_korean_buy(signal.korean_exchange, signal.symbol, krw_amount);

        if (korean_order.status == OrderStatus::Filled) {
            // SUCCESS - both filled
            total_foreign_amount += actual_filled;
            total_korean_amount += actual_filled;  // Should match futures
            total_korean_cost += krw_amount;
            remaining_usd -= order_size_usd;

            Logger::info("Split {}/{} complete: {} coins hedged (remaining: ${:.0f})",
                         i + 1, TradingConfig::SPLIT_ORDERS, actual_filled, remaining_usd);
        } else {
            // Korean failed but futures succeeded - MUST ROLLBACK futures
            Logger::error("Korean BUY failed! Rolling back futures SHORT");
            execute_foreign_cover(signal.foreign_exchange, foreign_symbol, actual_filled);
        }

        // Wait between splits (except last)
        if (i < TradingConfig::SPLIT_ORDERS - 1 && remaining_usd > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
        }
    }

    // Check result
    if (total_foreign_amount > 0 && total_korean_amount > 0) {
        result.success = true;
        result.position.korean_amount = total_korean_amount;
        result.position.foreign_amount = total_foreign_amount;
        result.position.korean_entry_price = total_korean_cost / total_korean_amount;
        result.position.foreign_entry_price = signal.foreign_bid;
        result.position.is_active = true;
        result.korean_filled_amount = total_korean_amount;
        result.foreign_filled_amount = total_foreign_amount;

        double filled_percent = (TradingConfig::POSITION_SIZE_USD - remaining_usd) / TradingConfig::POSITION_SIZE_USD * 100;
        Logger::info("Entry complete: {} | {} coins | {:.0f}% filled",
                     signal.symbol.to_string(), total_foreign_amount, filled_percent);
    } else {
        result.error_message = "No fills - all splits failed";
        Logger::error("Entry failed: {}", result.error_message);
    }

    return result;
}

ExecutionResult OrderManager::execute_exit_futures_first(const ExitSignal& signal, const Position& position) {
    ExecutionResult result;
    result.position = position;

    Logger::info("Executing exit (futures-first): {} premium={:.2f}%",
                 signal.symbol.to_string(), signal.premium);

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Exit failed: {}", result.error_message);
        return result;
    }

    SymbolId foreign_symbol(position.symbol.get_base(), "USDT");

    // Get current prices for split size calculation
    double current_foreign_ask = signal.foreign_ask;
    if (engine_) {
        auto& cache = engine_->get_price_cache();
        auto foreign_price = cache.get_price(signal.foreign_exchange, foreign_symbol);
        if (foreign_price.valid && foreign_price.ask > 0) {
            current_foreign_ask = foreign_price.ask;
        }
    }

    // Calculate split amounts
    double remaining_coins = position.foreign_amount;
    double coins_per_split = TradingConfig::ORDER_SIZE_USD / current_foreign_ask;
    int num_splits = static_cast<int>(std::ceil(remaining_coins / coins_per_split));

    double total_korean_sold = 0.0;
    double total_foreign_covered = 0.0;
    double total_korean_proceeds = 0.0;
    double total_foreign_cost = 0.0;

    Logger::info("Exit split plan: {} coins in {} splits (~{:.4f} coins each)",
                 remaining_coins, num_splits, coins_per_split);

    // Split order execution - FUTURES FIRST with premium re-check
    for (int i = 0; i < num_splits && remaining_coins > 0; ++i) {
        double split_amount = std::min(coins_per_split, remaining_coins);

        // Re-check premium condition before each split (except first)
        // Wait indefinitely until condition is met again
        if (i > 0 && engine_) {
            double current_premium = engine_->calculate_premium(
                signal.symbol, signal.korean_exchange, signal.foreign_exchange);

            if (current_premium < TradingConfig::EXIT_PREMIUM_THRESHOLD) {
                Logger::info("Split {}: Premium={:.2f}% < {:.2f}%, waiting for exit condition...",
                             i + 1, current_premium, TradingConfig::EXIT_PREMIUM_THRESHOLD);

                // Wait indefinitely until condition is met
                while (current_premium < TradingConfig::EXIT_PREMIUM_THRESHOLD) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
                    current_premium = engine_->calculate_premium(
                        signal.symbol, signal.korean_exchange, signal.foreign_exchange);
                }

                Logger::info("Split {}: Premium back to {:.2f}%, resuming exit",
                             i + 1, current_premium);
            }
        }

        Logger::info("Exit split {}/{}: COVER {} {} on Bybit",
                     i + 1, num_splits, split_amount, position.symbol.get_base());

        // STEP 1: COVER futures first
        Order foreign_order = execute_foreign_cover(signal.foreign_exchange, foreign_symbol, split_amount);

        if (foreign_order.status != OrderStatus::Filled) {
            Logger::error("Futures COVER failed for split {}, skipping", i + 1);
            continue;  // Skip this split, try next
        }

        double actual_covered = foreign_order.filled_quantity > 0
            ? foreign_order.filled_quantity
            : split_amount;
        double cover_price = foreign_order.average_price > 0
            ? foreign_order.average_price
            : current_foreign_ask;

        Logger::info("Futures COVER filled: {} coins at ${:.2f}", actual_covered, cover_price);

        // STEP 2: SELL Korean spot with EXACT same amount
        Logger::info("SELL {} {} on Bithumb",
                     actual_covered, position.symbol.get_base());

        Order korean_order = execute_korean_sell(signal.korean_exchange, position.symbol, actual_covered);

        if (korean_order.status == OrderStatus::Filled) {
            double sell_price = korean_order.average_price > 0
                ? korean_order.average_price
                : signal.korean_bid;

            total_foreign_covered += actual_covered;
            total_korean_sold += actual_covered;
            total_foreign_cost += actual_covered * cover_price;
            total_korean_proceeds += actual_covered * sell_price;
            remaining_coins -= actual_covered;

            Logger::info("Exit split {}/{} complete: {} coins closed (remaining: {:.4f})",
                         i + 1, num_splits, actual_covered, remaining_coins);
        } else {
            // Korean sell failed but futures already covered - CRITICAL
            Logger::error("CRITICAL: Korean SELL failed after futures COVER! Unhedged: {} coins", actual_covered);
            // Continue trying to sell in next splits
        }

        // Wait between splits (except last)
        if (i < num_splits - 1 && remaining_coins > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(TradingConfig::ORDER_INTERVAL_MS));
        }
    }

    // Check result
    if (total_foreign_covered > 0 && total_korean_sold > 0) {
        result.success = true;
        result.position.exit_time = std::chrono::system_clock::now();
        result.position.exit_premium = signal.premium;
        result.position.korean_exit_price = total_korean_proceeds / total_korean_sold;
        result.position.foreign_exit_price = total_foreign_cost / total_foreign_covered;
        result.position.is_active = (remaining_coins > 0);  // Partial if remaining

        // Calculate P&L
        result.position.realized_pnl_krw = calculate_pnl(
            position, result.position.korean_exit_price, result.position.foreign_exit_price, signal.usdt_krw_rate);

        double closed_percent = total_foreign_covered / position.foreign_amount * 100;
        Logger::info("Exit complete: {} | {:.0f}% closed | P&L: {:.0f} KRW",
                     signal.symbol.to_string(), closed_percent, result.position.realized_pnl_krw);

        if (remaining_coins > 0) {
            Logger::warn("Partial exit: {} coins still open", remaining_coins);
        }
    } else {
        result.error_message = "No fills - all exit splits failed";
        Logger::error("Exit failed: {}", result.error_message);
    }

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
    result.position.position_size_usd = TradingConfig::POSITION_SIZE_USD;

    auto korean_ex = get_korean_exchange(signal.korean_exchange);
    auto foreign_ex = get_futures_exchange(signal.foreign_exchange);

    if (!korean_ex || !foreign_ex) {
        result.error_message = "Exchange not available";
        Logger::error("Entry failed: {}", result.error_message);
        return result;
    }

    // Set leverage to 1x on futures exchange
    SymbolId foreign_symbol(signal.symbol.get_base(), "USDT");
    foreign_ex->set_leverage(foreign_symbol, 1);

    double total_korean_amount = 0.0;
    double total_foreign_amount = 0.0;

    // Split order execution
    for (int i = 0; i < TradingConfig::SPLIT_ORDERS; ++i) {
        double order_size_usd = TradingConfig::ORDER_SIZE_USD;

        // Calculate amounts
        double coin_amount = order_size_usd / signal.foreign_bid;
        double krw_amount = coin_amount * signal.korean_ask;

        // Ensure minimum order amount
        if (krw_amount < TradingConfig::MIN_ORDER_KRW) {
            krw_amount = TradingConfig::MIN_ORDER_KRW;
            coin_amount = krw_amount / signal.korean_ask;
        }

        Logger::info("Split order {}/{}: {} USD = {} coins, {} KRW",
                     i + 1, TradingConfig::SPLIT_ORDERS,
                     order_size_usd, coin_amount, krw_amount);

        // Execute Korean buy and Foreign short in parallel
        std::future<Order> korean_future = std::async(std::launch::async, [&]() {
            return execute_korean_buy(signal.korean_exchange, signal.symbol, krw_amount);
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
            Logger::info("Split order {}/{} successful", i + 1, TradingConfig::SPLIT_ORDERS);
        } else if (korean_success && !foreign_success) {
            // Korean succeeded but foreign failed - ROLLBACK
            Logger::error("Foreign order failed, rolling back Korean buy");

            if (rollback_korean_buy(signal.korean_exchange, signal.symbol, coin_amount)) {
                Logger::info("Rollback successful");
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

        Logger::info("Entry complete: {} korean={} foreign={}",
                     signal.symbol.to_string(),
                     total_korean_amount, total_foreign_amount);
    } else {
        result.error_message = "No fills received";
        Logger::error("Entry failed: {}", result.error_message);
    }

    return result;
}

ExecutionResult OrderManager::execute_exit(const ExitSignal& signal, const Position& position) {
    ExecutionResult result;
    result.position = position;

    Logger::info("Executing exit: {} premium={:.2f}%",
                 signal.symbol.to_string(), signal.premium);

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

        Logger::info("Exit complete: {} P&L={:.0f} KRW",
                     signal.symbol.to_string(), result.position.realized_pnl_krw);
    } else {
        result.error_message = "Exit orders failed";
        Logger::error("Exit failed: korean={} foreign={}",
                      korean_success ? "OK" : "FAILED",
                      foreign_success ? "OK" : "FAILED");
    }

    return result;
}

Order OrderManager::execute_korean_buy(Exchange ex, const SymbolId& symbol, double krw_amount) {
    auto korean_ex = get_korean_exchange(ex);
    if (!korean_ex) {
        Order order;
        order.status = OrderStatus::Rejected;
        return order;
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

} // namespace kimp::execution
