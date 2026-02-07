#pragma once

#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/exchange/upbit/upbit.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/gateio/gateio.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"

#include <memory>
#include <future>
#include <thread>
#include <unordered_set>
#include <mutex>

namespace kimp::execution {

/**
 * Order execution result
 */
struct ExecutionResult {
    bool success{false};
    Position position;
    std::string error_message;
    double korean_filled_amount{0.0};
    double foreign_filled_amount{0.0};
};

/**
 * Order Manager handles trade execution with split orders and rollback
 *
 * Features:
 * - Split order execution (2 orders with interval)
 * - Parallel Korean/Foreign execution
 * - Automatic rollback on partial failure
 * - Position lifecycle management
 */
class OrderManager {
public:
    using ExchangePtr = std::shared_ptr<exchange::IExchange>;
    using KoreanExchangePtr = std::shared_ptr<exchange::KoreanExchangeBase>;
    using FuturesExchangePtr = std::shared_ptr<exchange::ForeignFuturesExchangeBase>;

private:
    // Exchange references
    std::array<ExchangePtr, static_cast<size_t>(Exchange::Count)> exchanges_{};

    // Strategy engine for position tracking
    strategy::ArbitrageEngine* engine_{nullptr};

    // Execution thread
    std::thread exec_thread_;
    std::atomic<bool> running_{true};

public:
    OrderManager() = default;
    ~OrderManager();

    // Configuration
    void set_exchange(Exchange ex, ExchangePtr exchange);
    void set_engine(strategy::ArbitrageEngine* engine) { engine_ = engine; }

    // Execute entry (open position) - parallel execution
    ExecutionResult execute_entry(const ArbitrageSignal& signal);

    // Execute exit (close position) - parallel execution
    ExecutionResult execute_exit(const ExitSignal& signal, const Position& position);

    // Execute entry with futures FIRST for perfect hedge
    // 1. SHORT on Bybit (get exact contract size)
    // 2. BUY on Bithumb (same amount)
    ExecutionResult execute_entry_futures_first(const ArbitrageSignal& signal);

    // Execute exit with futures FIRST
    // 1. COVER on Bybit (close short)
    // 2. SELL on Bithumb (same amount)
    ExecutionResult execute_exit_futures_first(const ExitSignal& signal, const Position& position);

    // Pre-set leverage to 1x for all tradable symbols at startup
    void pre_set_leverage(const std::vector<SymbolId>& symbols);

    // Request graceful shutdown of any running adaptive loops
    void request_shutdown() { running_.store(false, std::memory_order_release); }

    // =========================================================================
    // SAFETY CHECK - Prevent trading coins with existing external positions
    // =========================================================================
    // Check if we already have positions outside the bot's tracking
    // (e.g., manually bought spot or existing futures positions)
    // Returns true if safe to trade, false if should skip
    bool is_safe_to_trade(const SymbolId& symbol, Exchange korean_ex, Exchange foreign_ex);

    // Position update callback for crash recovery persistence
    // Called with non-null Position* after each split (save), nullptr on full exit (delete)
    using PositionUpdateCallback = std::function<void(const Position*)>;
    void set_position_update_callback(PositionUpdateCallback cb) { on_position_update_ = std::move(cb); }

    // Build external position blacklist at startup (checks all trading symbols)
    void refresh_external_positions(const std::vector<SymbolId>& symbols,
                                    const std::unordered_set<SymbolId>& bot_managed = {});

private:
    PositionUpdateCallback on_position_update_;
    // External position blacklist (symbols we shouldn't trade)
    // Built once at startup, checked with O(1) lookup
    std::unordered_set<SymbolId> external_position_blacklist_;
    std::mutex blacklist_mutex_;
    // Get typed exchange
    KoreanExchangePtr get_korean_exchange(Exchange ex);
    FuturesExchangePtr get_futures_exchange(Exchange ex);

    // Split order execution
    ExecutionResult execute_split_entry(const ArbitrageSignal& signal);

    // Single order execution helpers
    Order execute_korean_buy(Exchange ex, const SymbolId& symbol, double quantity, double krw_amount);
    Order execute_foreign_short(Exchange ex, const SymbolId& symbol, double quantity);
    Order execute_korean_sell(Exchange ex, const SymbolId& symbol, double quantity);
    Order execute_foreign_cover(Exchange ex, const SymbolId& symbol, double quantity);

    // Rollback
    bool rollback_korean_buy(Exchange ex, const SymbolId& symbol, double quantity);

    // P&L calculation
    double calculate_pnl(const Position& pos, double exit_korean_price,
                         double exit_foreign_price, double usdt_rate);
};

} // namespace kimp::execution
