#pragma once

#include "kimp/core/types.hpp"
#include "kimp/core/latency_probe.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/okx/okx.hpp"
#include "kimp/execution/lifecycle_executor.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"

#include <cstddef>
#include <latch>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace kimp::execution {

/**
 * Order execution result
 */
struct ExecutionResult {
    bool success{false};
    bool position_managed{false};  // Position already managed in engine tracker by the function
    Position position;
    std::string error_message;
    double korean_filled_amount{0.0};
    double foreign_filled_amount{0.0};
};

/**
 * Order Manager handles the spot relay lifecycle.
 *
 * Features:
 * - Bithumb spot buy/sell execution
 * - Bybit spot-margin short open/cover execution
 * - Rollback and critical-stop safety handling
 * - Position lifecycle persistence and recovery
 */
class OrderManager {
public:
    using ExchangePtr = std::shared_ptr<exchange::IExchange>;
    using KoreanExchangePtr = std::shared_ptr<exchange::KoreanExchangeBase>;
    using BybitExchangePtr = std::shared_ptr<exchange::bybit::BybitExchange>;
    using OkxExchangePtr = std::shared_ptr<exchange::okx::OkxExchange>;

    OrderManager();

private:
    struct FillQueryTask {
        enum class Kind : uint8_t {
            Foreign,
            Korean,
        };

        Kind kind{Kind::Foreign};
        Exchange ex{Exchange::Bybit};
        SymbolId symbol{};
        uint64_t trace_id{0};
        uint64_t trace_start_ns{0};
        LatencySymbol trace_symbol{};
        LatencyStage enqueue_stage{LatencyStage::EntryForeignFillQueryStart};
        LatencyStage worker_stage{LatencyStage::EntryForeignFillWorkerStart};
        LatencyStage done_stage{LatencyStage::EntryForeignFillDone};
        Order* order{nullptr};
        std::latch* done{nullptr};
    };

    // Exchange references
    std::array<ExchangePtr, static_cast<size_t>(Exchange::Count)> exchanges_{};
    std::shared_ptr<exchange::bithumb::BithumbExchange> bithumb_exchange_;
    std::shared_ptr<exchange::bybit::BybitExchange> bybit_exchange_;
    std::shared_ptr<exchange::okx::OkxExchange> okx_exchange_;

    // Strategy engine for position tracking
    strategy::ArbitrageEngine* engine_{nullptr};

    std::atomic<bool> running_{true};
    LifecycleExecutor<FillQueryTask, 64> fill_query_executor_;
    std::once_flag fill_query_executor_start_once_;

public:
    ~OrderManager();

    // Configuration
    void set_exchange(Exchange ex, ExchangePtr exchange);
    void set_engine(strategy::ArbitrageEngine* engine) { engine_ = engine; }

    // Execute entry with foreign short FIRST for hedge sizing
    // 1. SHORT on Bybit spot margin
    // 2. BUY on Bithumb (same amount)
    // Optional initial_position for top-up: resumes from existing partial position state
    ExecutionResult execute_spot_relay_entry(
        const ArbitrageSignal& signal,
        const std::optional<Position>& initial_position = std::nullopt);

    // Execute exit with foreign short leg FIRST
    // 1. COVER on Bybit spot margin
    // 2. SELL on Bithumb (same amount)
    ExecutionResult execute_spot_relay_exit(const ExitSignal& signal, const Position& position);

    // Prepare foreign spot margin accounts before live trading
    bool prepare_bybit_shorting(const std::vector<SymbolId>& symbols);
    bool prepare_okx_shorting(const std::vector<SymbolId>& symbols);

    // Request graceful shutdown of any running adaptive loops
    void request_shutdown() { running_.store(false, std::memory_order_release); }

    // =========================================================================
    // SAFETY CHECK - Prevent trading coins with existing external positions
    // =========================================================================
    // Check if we already have positions outside the bot's tracking
    // (e.g., manually bought spot or existing short liabilities)
    // Returns true if safe to trade, false if should skip
    bool is_safe_to_trade(const SymbolId& symbol, Exchange korean_ex, Exchange foreign_ex);

    // Position update callback for crash recovery persistence
    // Called with non-null Position* after each split (save), nullptr on full exit (delete)
    using PositionUpdateCallback = std::function<void(const Position*)>;
    void set_position_update_callback(PositionUpdateCallback cb) { on_position_update_ = std::move(cb); }

    // Trade completion callback — called when a full enter→exit cycle completes within the lifecycle loop
    using TradeCompleteCallback = std::function<void(const Position& closed_pos, double pnl_krw, double usdt_rate)>;
    void set_trade_complete_callback(TradeCompleteCallback cb) { on_trade_complete_ = std::move(cb); }

    // Build external position blacklist at startup (checks all trading symbols)
    void refresh_external_positions(const std::vector<SymbolId>& symbols,
                                    const std::unordered_set<SymbolId>& bot_managed = {});

private:
    PositionUpdateCallback on_position_update_;
    TradeCompleteCallback on_trade_complete_;
    // External position blacklist (symbols we shouldn't trade)
    // Built once at startup, checked with O(1) lookup
    std::unordered_set<SymbolId> external_position_blacklist_;
    std::mutex blacklist_mutex_;
    // Get typed exchange
    KoreanExchangePtr get_korean_exchange(Exchange ex);
    BybitExchangePtr get_bybit_exchange();
    OkxExchangePtr get_okx_exchange();
    std::shared_ptr<exchange::ForeignShortExchangeBase> get_foreign_exchange(Exchange ex);

    // Single order execution helpers
    Order execute_korean_buy(Exchange ex, const SymbolId& symbol, double quantity, double krw_amount);
    Order execute_foreign_short(Exchange ex, const SymbolId& symbol, double quantity);
    Order execute_korean_sell(Exchange ex, const SymbolId& symbol, double quantity);
    Order execute_foreign_cover(Exchange ex, const SymbolId& symbol, double quantity);

    // Async fill price queries (parallel with hedge orders)
    void query_foreign_fill(Exchange ex, Order& order);
    void query_korean_fill(Exchange ex, const SymbolId& symbol, Order& order);
    void ensure_fill_query_executor_started();
    void handle_fill_query(FillQueryTask&& task, std::size_t worker_index);
    void dispatch_fill_query(FillQueryTask task);
    void wait_for_next_market_update(uint64_t update_seq_before_trade);
};

} // namespace kimp::execution
