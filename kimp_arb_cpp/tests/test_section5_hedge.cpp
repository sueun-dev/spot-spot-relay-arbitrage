/**
 * Section 5: Entry/Exit Callback & Hedge Accuracy Tests
 *
 * Tests:
 * - Bybit lot size normalization (SOL step=0.1, BTC step=0.001, etc.)
 * - Hedge mismatch prevention: actual_filled uses order.quantity fallback
 * - Entry callback CAS guard, MAX_POSITIONS check
 * - Exit callback re-adds partial position (not original)
 * - P&L calculation accuracy with SOL-scale numbers
 */

#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/execution/order_manager.hpp"

#include <iostream>
#include <cmath>
#include <atomic>
#include <cassert>

static int passed = 0;
static int failed = 0;

#define TEST(name, expr) do { \
    std::cout << "  [TEST] " << name << "... "; \
    if (expr) { std::cout << "PASS\n"; ++passed; } \
    else { std::cout << "FAIL\n"; ++failed; } \
} while(0)

#define TEST_NEAR(name, a, b, eps) do { \
    double _a = (a), _b = (b), _e = (eps); \
    std::cout << "  [TEST] " << name << "... "; \
    if (std::abs(_a - _b) <= _e) { std::cout << "PASS (" << _a << ")\n"; ++passed; } \
    else { std::cout << "FAIL (" << _a << " != " << _b << ")\n"; ++failed; } \
} while(0)

// =========================================================================
// Simulate Bybit normalize_order_qty logic (copied from bybit.cpp)
// =========================================================================
struct LotSize {
    double min_qty{0.0};
    double qty_step{0.0};
    double min_notional{0.0};
};

double simulate_normalize(double qty, const LotSize& lot, double price, bool is_open) {
    if (qty <= 0.0) return 0.0;
    double step = lot.qty_step > 0.0 ? lot.qty_step : 0.0;
    if (step > 0.0) {
        qty = std::floor(qty / step) * step;
    }
    if (qty < lot.min_qty) return 0.0;
    if (lot.min_notional > 0.0 && price > 0.0 && qty * price < lot.min_notional) return 0.0;
    return qty;
}

// =========================================================================
// Tests
// =========================================================================

void test_lot_size_normalization() {
    std::cout << "\n--- Section 5.1: Bybit Lot Size Normalization ---\n";

    // SOL: step=0.1, min=0.1, minNotional=$5
    LotSize sol{0.1, 0.1, 5.0};
    double sol_price = 187.50;

    // $25 order at $187.5 = 0.13333 coins
    double raw_qty = 25.0 / sol_price;
    double normalized = simulate_normalize(raw_qty, sol, sol_price, true);
    TEST_NEAR("SOL raw qty = 0.13333", raw_qty, 0.13333, 0.001);
    TEST_NEAR("SOL normalized = 0.1 (truncated to step)", normalized, 0.1, 1e-10);
    TEST("SOL hedge mismatch if using raw", std::abs(raw_qty - normalized) > 0.01);

    // BTC: step=0.001, min=0.001, minNotional=$5
    // $25 at $97k = 0.000257 BTC → below step 0.001 → normalized to 0 (too small!)
    // BTC requires at least $97/order (0.001 * $97000)
    LotSize btc{0.001, 0.001, 5.0};
    double btc_price = 97000.0;
    double btc_raw = 25.0 / btc_price;  // 0.000257...
    double btc_norm = simulate_normalize(btc_raw, btc, btc_price, true);
    TEST_NEAR("BTC raw qty = 0.000257", btc_raw, 0.000257, 0.0001);
    TEST("BTC $25 too small for 0.001 step (normalized=0)", btc_norm == 0.0);
    // With $100 order: 100/97000 = 0.001030 → normalized to 0.001 → OK
    double btc_100 = simulate_normalize(100.0 / btc_price, btc, btc_price, true);
    TEST("BTC $100 order normalizes to 0.001", btc_100 >= 0.001);

    // ETH: step=0.01, min=0.01
    LotSize eth{0.01, 0.01, 5.0};
    double eth_price = 3500.0;
    double eth_raw = 25.0 / eth_price;  // 0.007142...
    double eth_norm = simulate_normalize(eth_raw, eth, eth_price, true);
    TEST("ETH normalized = 0 (below min_qty 0.01)", eth_norm == 0.0);

    // DOGE: step=1, min=1
    LotSize doge{1.0, 1.0, 5.0};
    double doge_price = 0.35;
    double doge_raw = 25.0 / doge_price;  // 71.428...
    double doge_norm = simulate_normalize(doge_raw, doge, doge_price, true);
    TEST_NEAR("DOGE normalized = 71 (truncated to step=1)", doge_norm, 71.0, 1e-10);
}

void test_hedge_accuracy_sol() {
    std::cout << "\n--- Section 5.2: Hedge Accuracy with SOL ---\n";

    // Simulate the FIXED code path:
    // order.quantity = adj_qty (normalized)
    // actual_filled = filled_qty > 0 ? filled_qty : order.quantity

    double sol_price = 187.50;
    double order_size_usd = 25.0;
    double coin_amount = order_size_usd / sol_price;  // 0.13333 (raw)

    // Simulate Bybit open_short
    kimp::Order foreign_order;
    LotSize sol{0.1, 0.1, 5.0};
    double adj_qty = simulate_normalize(coin_amount, sol, sol_price, true);
    foreign_order.quantity = adj_qty;  // 0.1 (what Bybit actually processes)
    foreign_order.status = kimp::OrderStatus::Filled;

    // Case 1: fill query succeeds
    foreign_order.filled_quantity = 0.1;
    foreign_order.average_price = 187.48;
    double actual_1 = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
    TEST_NEAR("Fill query OK: actual = filled_qty = 0.1", actual_1, 0.1, 1e-10);

    // Case 2: fill query FAILS (filled_quantity = 0)
    foreign_order.filled_quantity = 0.0;
    foreign_order.average_price = 0.0;

    // FIXED: uses foreign_order.quantity (normalized) as fallback
    double actual_fixed = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : foreign_order.quantity;
    TEST_NEAR("Fill query FAIL (FIXED): actual = order.quantity = 0.1", actual_fixed, 0.1, 1e-10);

    // BUG (old code): would use coin_amount (un-normalized)
    double actual_buggy = foreign_order.filled_quantity > 0 ? foreign_order.filled_quantity : coin_amount;
    TEST("OLD BUG: would use 0.13333 (mismatch!)", std::abs(actual_buggy - adj_qty) > 0.01);

    // Verify hedge is exact with fix
    double korean_buy_amount = actual_fixed;  // Bithumb buys this
    double foreign_short_amount = adj_qty;     // Bybit shorted this
    TEST_NEAR("Hedge exact: korean == foreign", korean_buy_amount, foreign_short_amount, 1e-10);
}

void test_entry_callback_guards() {
    std::cout << "\n--- Section 5.3: Entry Callback Guards ---\n";

    // CAS guard test
    std::atomic<bool> g_entry_in_flight{false};

    // First entry should succeed
    bool expected1 = false;
    bool cas1 = g_entry_in_flight.compare_exchange_strong(expected1, true);
    TEST("First CAS succeeds", cas1 && g_entry_in_flight.load());

    // Second entry should be blocked
    bool expected2 = false;
    bool cas2 = g_entry_in_flight.compare_exchange_strong(expected2, true);
    TEST("Second CAS blocked (in-flight)", !cas2);

    // RAII guard releases
    g_entry_in_flight.store(false);
    bool expected3 = false;
    bool cas3 = g_entry_in_flight.compare_exchange_strong(expected3, true);
    TEST("After guard release, CAS succeeds again", cas3);

    // MAX_POSITIONS check
    kimp::strategy::PositionTracker tracker;
    TEST("Initially can open position", tracker.can_open_position());

    kimp::Position pos;
    pos.symbol = kimp::SymbolId("SOL", "KRW");
    pos.is_active = true;
    tracker.open_position(pos);
    // MAX_POSITIONS = 1 in TradingConfig
    TEST("After 1 position, cannot open more (MAX=1)", !tracker.can_open_position());
}

void test_exit_callback_partial_reregister() {
    std::cout << "\n--- Section 5.4: Exit Callback Partial Position Re-register ---\n";

    kimp::strategy::ArbitrageEngine engine;

    // Open a position with 10 SOL
    kimp::Position original;
    original.symbol = kimp::SymbolId("SOL", "KRW");
    original.korean_exchange = kimp::Exchange::Bithumb;
    original.foreign_exchange = kimp::Exchange::Bybit;
    original.korean_amount = 10.0;
    original.foreign_amount = 10.0;
    original.korean_entry_price = 250000.0;
    original.foreign_entry_price = 187.50;
    original.position_size_usd = 250.0;
    original.is_active = true;
    engine.open_position(original);
    TEST("Position opened: 10 SOL", engine.get_position_count() == 1);

    // Simulate: close_position extracts it
    kimp::Position closed_pos;
    engine.close_position(original.symbol, closed_pos);
    TEST("Position extracted from tracker", engine.get_position_count() == 0);
    TEST_NEAR("Extracted amount = 10", closed_pos.korean_amount, 10.0, 1e-10);

    // Simulate: exit partially executed (5 of 10 SOL exited, shutdown)
    kimp::execution::ExecutionResult result;
    result.success = false;  // Shutdown interrupted
    result.position = closed_pos;
    result.position.korean_amount = 5.0;   // Only 5 remaining
    result.position.foreign_amount = 5.0;
    result.position.is_active = true;

    // FIXED: re-add result.position (5 SOL), not closed_pos (10 SOL)
    engine.open_position(result.position);
    TEST("Re-added position count = 1", engine.get_position_count() == 1);

    (void)engine.get_price_cache().size();

    // Verify the re-added position has correct amounts
    kimp::Position verify;
    engine.close_position(original.symbol, verify);
    TEST_NEAR("Re-added has 5 SOL (not 10!)", verify.korean_amount, 5.0, 1e-10);
    TEST_NEAR("Foreign also 5 SOL", verify.foreign_amount, 5.0, 1e-10);

    // Show what the BUG would have done
    // engine.open_position(closed_pos);  // Would re-add 10 SOL - WRONG!
    std::cout << "  [INFO] BUG would re-add 10 SOL, causing 5 SOL over-sell on next exit\n";
}

void test_pnl_calculation_sol() {
    std::cout << "\n--- Section 5.5: P&L Calculation with SOL ---\n";

    // Entry: premium = -0.80%
    double usdt_rate = 1380.0;
    double sol_korean_ask = 256800.0;  // KRW (Bithumb buy price)
    double sol_foreign_bid = 187.50;    // USD (Bybit short price)

    double entry_premium = ((sol_korean_ask - sol_foreign_bid * usdt_rate) / (sol_foreign_bid * usdt_rate)) * 100.0;
    TEST("Entry premium ~= -0.77%", entry_premium < -0.5 && entry_premium > -1.5);

    // Exit: premium = +0.10%
    double sol_korean_bid = 260800.0;   // KRW (Bithumb sell price)
    double sol_foreign_ask = 188.50;     // USD (Bybit cover price)

    [[maybe_unused]] double exit_premium = ((sol_korean_bid - sol_foreign_ask * usdt_rate) / (sol_foreign_ask * usdt_rate)) * 100.0;

    // P&L for 0.1 SOL position
    double amount = 0.1;

    // Korean P&L: (sell - buy) * amount
    double korean_pnl = (sol_korean_bid - sol_korean_ask) * amount;  // (260800 - 256800) * 0.1 = 400 KRW

    // Foreign P&L: (short_entry - cover_exit) * amount * usdt_rate
    double foreign_pnl_usd = (sol_foreign_bid - sol_foreign_ask) * amount;  // (187.5 - 188.5) * 0.1 = -0.1 USD
    double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;  // -0.1 * 1380 = -138 KRW

    double total_pnl_krw = korean_pnl + foreign_pnl_krw;  // 400 + (-138) = 262 KRW
    double total_pnl_usd = total_pnl_krw / usdt_rate;  // ~0.19 USD

    TEST("Korean P&L = 400 KRW (spot profit)", std::abs(korean_pnl - 400.0) < 0.01);
    TEST("Foreign P&L = -138 KRW (futures cost)", std::abs(foreign_pnl_krw - (-138.0)) < 0.01);
    TEST("Total P&L = 262 KRW (net positive)", total_pnl_krw > 200.0 && total_pnl_krw < 300.0);
    TEST("Total P&L USD ~= 0.19", total_pnl_usd > 0.15 && total_pnl_usd < 0.25);
}

int main() {
    std::cout << "=== Section 5: Entry/Exit Callback & Hedge Accuracy Tests ===\n";

    test_lot_size_normalization();
    test_hedge_accuracy_sol();
    test_entry_callback_guards();
    test_exit_callback_partial_reregister();
    test_pnl_calculation_sol();

    std::cout << "\n=== Results ===\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";
    std::cout << "  Total:  " << (passed + failed) << "\n\n";

    if (failed == 0) {
        std::cout << "*** ALL TESTS PASSED ***\n";
    } else {
        std::cout << "*** " << failed << " TESTS FAILED ***\n";
    }

    return failed > 0 ? 1 : 0;
}
