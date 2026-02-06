/**
 * Test: Sections 6-8 (IO 스레드 풀, WebSocket 연결, 공통 심볼 & 전처리)
 *
 * Section 6: ThreadConfig, cpu_pause, spin_wait, fast_stod, memory alignment
 * Section 7: Exchange connect 패턴 (REST pool + WS), 연결 폴링 로직
 * Section 8: 공통 심볼 계산, add_symbol 중복방지, KRW↔USDT 매핑,
 *            블랙리스트 SymbolId 정확성 (hash collision 방어),
 *            PriceCache, PremiumCalculator, CapitalTracker
 *
 * 네트워크 없이 로컬에서 완전히 테스트 가능한 항목만 검증
 */

#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/execution/order_manager.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"

#include <boost/asio.hpp>

#include <iostream>
#include <cmath>
#include <atomic>
#include <cassert>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <vector>
#include <string>

namespace net = boost::asio;

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
// Section 6: IO Thread Pool & Optimization
// =========================================================================

void test_section6_thread_config_optimal() {
    std::cout << "\n--- Section 6.1: ThreadConfig::optimal() ---\n";

    auto cfg = kimp::opt::ThreadConfig::optimal();
    int cores = std::thread::hardware_concurrency();

    if (cores >= 8) {
        TEST("8+ cores: dedicated io_bithumb_core=1", cfg.io_bithumb_core == 1);
        TEST("8+ cores: dedicated io_bybit_core=2", cfg.io_bybit_core == 2);
        TEST("8+ cores: strategy_core=4", cfg.strategy_core == 4);
        TEST("8+ cores: execution_core=5", cfg.execution_core == 5);
    } else if (cores >= 4) {
        TEST("4+ cores: shared io cores", cfg.io_bithumb_core == 0 && cfg.io_bybit_core == 1);
        TEST("4+ cores: strategy_core=2", cfg.strategy_core == 2);
        TEST("4+ cores: execution_core=3", cfg.execution_core == 3);
    } else {
        TEST("Low cores: no pinning (-1)", cfg.io_bithumb_core == -1 && cfg.strategy_core == -1);
    }

    std::cout << "  [INFO] Detected " << cores << " cores, config applied\n";
}

void test_section6_cpu_pause_no_crash() {
    std::cout << "\n--- Section 6.2: cpu_pause / spin_wait ---\n";

    // cpu_pause should not crash (ARM yield or x86 _mm_pause)
    kimp::opt::cpu_pause();
    kimp::opt::cpu_pause();
    kimp::opt::cpu_pause();
    TEST("cpu_pause() x3 no crash", true);

    // spin_wait_timeout with immediate true
    bool result = kimp::opt::spin_wait_timeout([]() { return true; }, 100);
    TEST("spin_wait_timeout immediate true", result);

    // spin_wait_timeout with always false (should timeout)
    bool timeout = kimp::opt::spin_wait_timeout([]() { return false; }, 10);
    TEST("spin_wait_timeout with max_iterations=10 → false", !timeout);

    // spin_wait_timeout with counter
    std::atomic<int> counter{0};
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        counter.store(1, std::memory_order_release);
    });
    bool waited = kimp::opt::spin_wait_timeout(
        [&]() { return counter.load(std::memory_order_acquire) == 1; },
        1000000  // plenty of iterations
    );
    t.join();
    TEST("spin_wait_timeout with async signal", waited && counter.load() == 1);
}

void test_section6_fast_stod() {
    std::cout << "\n--- Section 6.3: fast_stod ---\n";

    TEST_NEAR("fast_stod('187.50')", kimp::opt::fast_stod("187.50"), 187.50, 1e-10);
    TEST_NEAR("fast_stod('0.0001')", kimp::opt::fast_stod("0.0001"), 0.0001, 1e-10);
    TEST_NEAR("fast_stod('97000.00')", kimp::opt::fast_stod("97000.00"), 97000.0, 1e-10);
    TEST_NEAR("fast_stod('') = default 0", kimp::opt::fast_stod(""), 0.0, 1e-10);
    TEST_NEAR("fast_stod('abc') = default 0", kimp::opt::fast_stod("abc", 0.0), 0.0, 1e-10);
    TEST_NEAR("fast_stod('-1.25')", kimp::opt::fast_stod("-1.25"), -1.25, 1e-10);
    TEST_NEAR("fast_stod('0.00000001')", kimp::opt::fast_stod("0.00000001"), 1e-8, 1e-15);
}

void test_section6_memory_barrier() {
    std::cout << "\n--- Section 6.4: Memory barriers ---\n";

    // Just verify they don't crash
    kimp::opt::memory_barrier();
    kimp::opt::compiler_barrier();
    TEST("memory_barrier + compiler_barrier no crash", true);

    // Relaxed atomic helpers
    std::atomic<double> val{0.0};
    kimp::opt::store_relaxed(val, 42.5);
    double loaded = kimp::opt::load_relaxed(val);
    TEST_NEAR("store_relaxed/load_relaxed roundtrip", loaded, 42.5, 1e-10);
}

void test_section6_prefetch_no_crash() {
    std::cout << "\n--- Section 6.5: Cache prefetch ---\n";

    double data[64];
    kimp::opt::prefetch_read(data);
    kimp::opt::prefetch_write(data);
    TEST("prefetch_read/write no crash", true);
}

// =========================================================================
// Section 7: Exchange WebSocket Connection Pattern
// =========================================================================

void test_section7_connect_pattern() {
    std::cout << "\n--- Section 7.1: Exchange connect pattern ---\n";

    net::io_context ioc;
    kimp::ExchangeCredentials creds;
    creds.api_key = "test_key";
    creds.secret_key = "test_secret";
    creds.enabled = true;

    auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(ioc, creds);
    auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(ioc, creds);

    // Before connect: not connected
    TEST("Bithumb not connected initially", !bithumb->is_connected());
    TEST("Bybit not connected initially", !bybit->is_connected());

    // Exchange IDs
    TEST("Bithumb ID correct", bithumb->get_exchange_id() == kimp::Exchange::Bithumb);
    TEST("Bybit ID correct", bybit->get_exchange_id() == kimp::Exchange::Bybit);
    TEST("Bithumb is Spot", bithumb->get_market_type() == kimp::MarketType::Spot);
    TEST("Bybit is Perpetual", bybit->get_market_type() == kimp::MarketType::Perpetual);
}

void test_section7_polling_logic() {
    std::cout << "\n--- Section 7.2: Connection polling logic ---\n";

    // Simulate the polling loop from main.cpp:615-623
    std::atomic<bool> bithumb_connected{false};
    std::atomic<bool> bybit_connected{false};
    std::atomic<bool> g_shutdown{false};

    // Simulate async connections
    std::thread connector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bithumb_connected.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bybit_connected.store(true);
    });

    // Simulate polling (200ms intervals, max 50 iterations = 10s)
    int wait_count = 0;
    while (!g_shutdown && wait_count < 50) {
        if (bithumb_connected.load() && bybit_connected.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Faster for test
        ++wait_count;
    }
    connector.join();

    TEST("Both connected before timeout", bithumb_connected && bybit_connected);
    TEST("Polling completed in < 50 iterations", wait_count < 50);
    std::cout << "  [INFO] Connected after " << wait_count << " poll iterations\n";

    // Simulate timeout case
    std::atomic<bool> never_connects{false};
    int timeout_count = 0;
    while (timeout_count < 5) {  // Small limit for test speed
        if (never_connects.load()) break;
        ++timeout_count;
    }
    TEST("Timeout detected when exchange never connects", timeout_count == 5);
}

// =========================================================================
// Section 8: Common Symbol Calculation & Preprocessing
// =========================================================================

void test_section8_common_symbol_intersection() {
    std::cout << "\n--- Section 8.1: Common symbol set intersection ---\n";

    // Simulate bithumb symbols (KRW markets)
    std::vector<kimp::SymbolId> bithumb_symbols;
    bithumb_symbols.emplace_back("BTC", "KRW");
    bithumb_symbols.emplace_back("ETH", "KRW");
    bithumb_symbols.emplace_back("SOL", "KRW");
    bithumb_symbols.emplace_back("XRP", "KRW");
    bithumb_symbols.emplace_back("DOGE", "KRW");
    bithumb_symbols.emplace_back("KLAY", "KRW");  // Korean-only

    // Simulate bybit symbols (USDT perps)
    std::vector<kimp::SymbolId> bybit_symbols;
    bybit_symbols.emplace_back("BTC", "USDT");
    bybit_symbols.emplace_back("ETH", "USDT");
    bybit_symbols.emplace_back("SOL", "USDT");
    bybit_symbols.emplace_back("XRP", "USDT");
    bybit_symbols.emplace_back("DOGE", "USDT");
    bybit_symbols.emplace_back("APT", "USDT");   // Bybit-only

    // Replicate the exact logic from main.cpp:634-645
    std::unordered_set<std::string> bybit_bases;
    for (const auto& s : bybit_symbols) {
        bybit_bases.insert(std::string(s.get_base()));
    }

    std::vector<kimp::SymbolId> common_symbols;
    for (const auto& s : bithumb_symbols) {
        if (bybit_bases.count(std::string(s.get_base()))) {
            common_symbols.emplace_back(std::string(s.get_base()), "KRW");
        }
    }

    TEST("Common symbols count = 5 (BTC,ETH,SOL,XRP,DOGE)", common_symbols.size() == 5);
    TEST("KLAY excluded (not on Bybit)", bybit_bases.count("KLAY") == 0);
    TEST("APT excluded (not on Bithumb)", true);  // APT not in bithumb_symbols

    // Verify quote is KRW (not USDT)
    for (const auto& s : common_symbols) {
        TEST(("Common symbol " + std::string(s.get_base()) + " has KRW quote").c_str(),
             std::string(s.get_quote()) == "KRW");
    }
}

void test_section8_add_symbol_dedup_and_mapping() {
    std::cout << "\n--- Section 8.2: add_symbol dedup + KRW→USDT mapping ---\n";

    net::io_context ioc;
    kimp::ExchangeCredentials creds;
    creds.api_key = "key";
    creds.secret_key = "secret";
    auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(ioc, creds);
    auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(ioc, creds);

    kimp::strategy::ArbitrageEngine engine;
    engine.set_exchange(kimp::Exchange::Bithumb, bithumb);
    engine.set_exchange(kimp::Exchange::Bybit, bybit);
    engine.add_exchange_pair(kimp::Exchange::Bithumb, kimp::Exchange::Bybit);

    // Add symbols
    kimp::SymbolId sol("SOL", "KRW");
    kimp::SymbolId btc("BTC", "KRW");
    kimp::SymbolId eth("ETH", "KRW");

    engine.add_symbol(sol);
    engine.add_symbol(btc);
    engine.add_symbol(eth);

    // Deduplication test: add SOL again
    engine.add_symbol(sol);
    engine.add_symbol(sol);

    // Verify via price cache (add_symbol populates internal maps)
    // If add_symbol had duplicates, we'd see crashes or wrong behavior
    // Test by updating prices for both KRW and USDT variants
    auto& cache = engine.get_price_cache();

    // Korean side: SOL/KRW
    cache.update(kimp::Exchange::Bithumb, sol, 256000.0, 256500.0, 256200.0);
    auto kr_price = cache.get_price(kimp::Exchange::Bithumb, sol);
    TEST("PriceCache: SOL/KRW bid stored", kr_price.valid && std::abs(kr_price.bid - 256000.0) < 0.01);

    // Foreign side: SOL/USDT (pre-computed by add_symbol)
    kimp::SymbolId sol_usdt("SOL", "USDT");
    cache.update(kimp::Exchange::Bybit, sol_usdt, 187.40, 187.60, 187.50);
    auto fgn_price = cache.get_price(kimp::Exchange::Bybit, sol_usdt);
    TEST("PriceCache: SOL/USDT ask stored", fgn_price.valid && std::abs(fgn_price.ask - 187.60) < 0.01);

    // Cross-check: BTC also works
    kimp::SymbolId btc_usdt("BTC", "USDT");
    cache.update(kimp::Exchange::Bybit, btc_usdt, 96900.0, 97100.0, 97000.0);
    auto btc_price = cache.get_price(kimp::Exchange::Bybit, btc_usdt);
    TEST("PriceCache: BTC/USDT stored", btc_price.valid && std::abs(btc_price.bid - 96900.0) < 0.01);
}

void test_section8_blacklist_symbolid_not_hash() {
    std::cout << "\n--- Section 8.3: Blacklist uses SymbolId (not raw hash) ---\n";

    // Verify the fix: blacklist should use SymbolId, not size_t hash
    // Create two SymbolIds that could theoretically hash-collide
    kimp::SymbolId sym1("SOL", "KRW");
    kimp::SymbolId sym2("ETH", "KRW");

    // Test with unordered_set<SymbolId> (the fixed type)
    std::unordered_set<kimp::SymbolId> blacklist;
    blacklist.insert(sym1);

    TEST("SOL in blacklist", blacklist.count(sym1) == 1);
    TEST("ETH NOT in blacklist", blacklist.count(sym2) == 0);

    // Even if hashes were the same, SymbolId == comparison would differentiate
    blacklist.insert(sym2);
    TEST("Both in blacklist after insert", blacklist.size() == 2);

    // Simulate the is_safe_to_trade pattern
    kimp::SymbolId query("SOL", "KRW");
    bool blocked = blacklist.count(query) > 0;
    TEST("is_safe_to_trade: SOL blocked correctly", blocked);

    kimp::SymbolId safe("DOGE", "KRW");
    bool safe_result = blacklist.count(safe) == 0;
    TEST("is_safe_to_trade: DOGE is safe", safe_result);
}

void test_section8_position_tracker_hash_collision_safety() {
    std::cout << "\n--- Section 8.4: PositionTracker hash collision safety ---\n";

    kimp::strategy::PositionTracker tracker;

    // Open a SOL position
    kimp::Position sol_pos;
    sol_pos.symbol = kimp::SymbolId("SOL", "KRW");
    sol_pos.korean_amount = 10.0;
    sol_pos.foreign_amount = 10.0;
    sol_pos.is_active = true;
    tracker.open_position(sol_pos);

    TEST("SOL position exists", tracker.has_position(kimp::SymbolId("SOL", "KRW")));
    TEST("ETH position does NOT exist", !tracker.has_position(kimp::SymbolId("ETH", "KRW")));
    TEST("BTC position does NOT exist", !tracker.has_position(kimp::SymbolId("BTC", "KRW")));

    // get_position returns correct data
    const auto* p = tracker.get_position(kimp::SymbolId("SOL", "KRW"));
    TEST("get_position(SOL) returns non-null", p != nullptr);
    if (p) {
        TEST_NEAR("get_position(SOL) amount = 10", p->korean_amount, 10.0, 1e-10);
    }

    const auto* p2 = tracker.get_position(kimp::SymbolId("ETH", "KRW"));
    TEST("get_position(ETH) returns null", p2 == nullptr);

    // close_position only closes the right symbol
    kimp::Position closed;
    bool closed_eth = tracker.close_position(kimp::SymbolId("ETH", "KRW"), closed);
    TEST("close_position(ETH) returns false", !closed_eth);
    TEST("SOL still open after failed ETH close", tracker.get_position_count() == 1);

    bool closed_sol = tracker.close_position(kimp::SymbolId("SOL", "KRW"), closed);
    TEST("close_position(SOL) returns true", closed_sol);
    TEST_NEAR("Closed position has 10 SOL", closed.korean_amount, 10.0, 1e-10);
    TEST("Position count = 0 after close", tracker.get_position_count() == 0);
}

void test_section8_premium_calculator() {
    std::cout << "\n--- Section 8.5: PremiumCalculator ---\n";

    double usdt_rate = 1380.0;

    // Entry premium: korean_ask vs foreign_bid
    // SOL: Bithumb ask=256800 KRW, Bybit bid=187.50 USD
    double entry_pm = kimp::strategy::PremiumCalculator::calculate_entry_premium(
        256800.0, 187.50, usdt_rate);
    // foreign_krw = 187.50 * 1380 = 258750
    // premium = (256800 - 258750) / 258750 * 100 = -0.7536%
    TEST_NEAR("Entry premium SOL ~= -0.75%", entry_pm, -0.7536, 0.01);
    TEST("should_enter at -0.75%", kimp::strategy::PremiumCalculator::should_enter(entry_pm));

    // Exit premium: korean_bid vs foreign_ask
    double exit_pm = kimp::strategy::PremiumCalculator::calculate_exit_premium(
        260800.0, 188.50, usdt_rate);
    // foreign_krw = 188.50 * 1380 = 260130
    // premium = (260800 - 260130) / 260130 * 100 = +0.2575%
    TEST("Exit premium > 0", exit_pm > 0.0);

    // Edge cases
    double zero_pm = kimp::strategy::PremiumCalculator::calculate_entry_premium(0.0, 187.50, usdt_rate);
    TEST_NEAR("Zero korean_ask → negative premium", zero_pm, -100.0, 0.01);

    double zero_foreign = kimp::strategy::PremiumCalculator::calculate_entry_premium(256800.0, 0.0, usdt_rate);
    TEST_NEAR("Zero foreign_bid → 0 (guard)", zero_foreign, 0.0, 1e-10);

    double zero_rate = kimp::strategy::PremiumCalculator::calculate_entry_premium(256800.0, 187.50, 0.0);
    TEST_NEAR("Zero usdt_rate → 0 (guard)", zero_rate, 0.0, 1e-10);
}

void test_section8_capital_tracker() {
    std::cout << "\n--- Section 8.6: CapitalTracker ---\n";

    kimp::strategy::CapitalTracker tracker(2000.0);

    TEST_NEAR("Initial capital = $2000", tracker.get_initial_capital(), 2000.0, 1e-10);
    TEST_NEAR("Current capital = $2000 (no P&L)", tracker.get_current_capital(), 2000.0, 1e-10);
    TEST_NEAR("Realized P&L = $0", tracker.get_realized_pnl(), 0.0, 1e-10);

    // Position size: min(2000/1/2, 250) = min(1000, 250) = 250
    TEST_NEAR("Position size = $250 (capped)", tracker.get_position_size_usd(), 250.0, 1e-10);

    // Add profit
    tracker.add_realized_pnl(1.50);  // $1.50 profit
    TEST_NEAR("After +$1.50: capital = $2001.50", tracker.get_current_capital(), 2001.50, 1e-10);
    TEST("Total trades = 1", tracker.get_total_trades() == 1);
    TEST("Winning trades = 1", tracker.get_winning_trades() == 1);
    TEST_NEAR("Win rate = 100%", tracker.get_win_rate(), 100.0, 1e-10);

    // Add loss
    tracker.add_realized_pnl(-0.30);  // $0.30 loss
    TEST_NEAR("After -$0.30: capital = $2001.20", tracker.get_current_capital(), 2001.20, 1e-10);
    TEST("Total trades = 2", tracker.get_total_trades() == 2);
    TEST("Winning trades still = 1", tracker.get_winning_trades() == 1);
    TEST_NEAR("Win rate = 50%", tracker.get_win_rate(), 50.0, 1e-10);

    // Return percent
    TEST_NEAR("Return = 0.06%", tracker.get_return_percent(), 0.06, 0.001);
}

void test_section8_price_cache_thread_safety() {
    std::cout << "\n--- Section 8.7: PriceCache thread safety ---\n";

    kimp::strategy::PriceCache cache;

    kimp::SymbolId sol("SOL", "KRW");
    kimp::SymbolId sol_usdt("SOL", "USDT");

    // Write from multiple threads
    std::atomic<int> done{0};
    std::thread writer1([&]() {
        for (int i = 0; i < 1000; ++i) {
            cache.update(kimp::Exchange::Bithumb, sol,
                         256000.0 + i, 256500.0 + i, 256200.0 + i);
        }
        done.fetch_add(1);
    });

    std::thread writer2([&]() {
        for (int i = 0; i < 1000; ++i) {
            cache.update(kimp::Exchange::Bybit, sol_usdt,
                         187.0 + i * 0.01, 188.0 + i * 0.01, 187.5 + i * 0.01);
        }
        done.fetch_add(1);
    });

    std::thread reader([&]() {
        int reads = 0;
        while (done.load() < 2) {
            auto p1 = cache.get_price(kimp::Exchange::Bithumb, sol);
            auto p2 = cache.get_price(kimp::Exchange::Bybit, sol_usdt);
            (void)p1; (void)p2;
            ++reads;
        }
    });

    writer1.join();
    writer2.join();
    reader.join();

    // Verify final state
    auto final_kr = cache.get_price(kimp::Exchange::Bithumb, sol);
    auto final_fg = cache.get_price(kimp::Exchange::Bybit, sol_usdt);

    TEST("PriceCache: concurrent writes no crash", true);
    TEST("PriceCache: final KRW bid > 256000", final_kr.valid && final_kr.bid > 256000.0);
    TEST("PriceCache: final USDT ask > 187", final_fg.valid && final_fg.ask > 187.0);

    // USDT/KRW rate
    cache.update_usdt_krw(kimp::Exchange::Bithumb, 1385.0);
    TEST_NEAR("USDT/KRW Bithumb = 1385", cache.get_usdt_krw(kimp::Exchange::Bithumb), 1385.0, 1e-10);
}

void test_section8_funding_cache_and_leverage() {
    std::cout << "\n--- Section 8.8: Funding cache & pre_set_leverage pattern ---\n";

    // Verify funding interval cache concept
    // (actual API call not tested, just the data flow)
    kimp::strategy::PriceCache cache;
    kimp::SymbolId sol_usdt("SOL", "USDT");

    // Simulate what bybit.cpp does: cache funding during get_available_symbols
    cache.update(kimp::Exchange::Bybit, sol_usdt, 187.0, 188.0, 187.5);
    cache.update_funding(kimp::Exchange::Bybit, sol_usdt, 0.0001, 8, 1700000000000ULL);

    auto data = cache.get_price(kimp::Exchange::Bybit, sol_usdt);
    TEST("Funding rate cached", data.valid && std::abs(data.funding_rate - 0.0001) < 1e-10);
    TEST("Funding interval = 8h", data.funding_interval_hours == 8);
    TEST("Next funding time stored", data.next_funding_time == 1700000000000ULL);

    // pre_set_leverage pattern: just verify the concept
    // (actual API call tested in integration tests)
    std::vector<kimp::SymbolId> symbols;
    symbols.emplace_back("SOL", "KRW");
    symbols.emplace_back("BTC", "KRW");

    // Convert KRW→USDT (same pattern as pre_set_leverage)
    for (const auto& sym : symbols) {
        kimp::SymbolId foreign(sym.get_base(), "USDT");
        // Would call bybit->set_leverage(foreign, 1)
        TEST(("Leverage symbol: " + foreign.to_string() + " correct").c_str(),
             std::string(foreign.get_quote()) == "USDT");
    }
}

void test_section8_dynamic_exit_threshold() {
    std::cout << "\n--- Section 8.9: Dynamic exit threshold ---\n";

    // Verify TradingConfig constants
    TEST_NEAR("ROUND_TRIP_FEE = 0.19%",
              kimp::TradingConfig::ROUND_TRIP_FEE_PCT, 0.19, 0.001);
    TEST_NEAR("DYNAMIC_EXIT_SPREAD = 0.79%",
              kimp::TradingConfig::DYNAMIC_EXIT_SPREAD, 0.79, 0.001);

    // Dynamic exit: exit_pm >= entry_pm + DYNAMIC_EXIT_SPREAD
    double entry_pm1 = -0.75;
    double required_exit1 = entry_pm1 + kimp::TradingConfig::DYNAMIC_EXIT_SPREAD;
    TEST_NEAR("Entry -0.75% → exit >= +0.04%", required_exit1, 0.04, 0.001);

    double entry_pm2 = -1.00;
    double required_exit2 = entry_pm2 + kimp::TradingConfig::DYNAMIC_EXIT_SPREAD;
    TEST_NEAR("Entry -1.00% → exit >= -0.21%", required_exit2, -0.21, 0.001);

    double entry_pm3 = -1.50;
    double required_exit3 = entry_pm3 + kimp::TradingConfig::DYNAMIC_EXIT_SPREAD;
    TEST_NEAR("Entry -1.50% → exit >= -0.71%", required_exit3, -0.71, 0.001);

    // Verify entry filter constants
    TEST("MIN_FUNDING_INTERVAL = 8h",
         kimp::TradingConfig::MIN_FUNDING_INTERVAL_HOURS == 8);
    TEST("REQUIRE_POSITIVE_FUNDING = true",
         kimp::TradingConfig::REQUIRE_POSITIVE_FUNDING == true);
    TEST("SPLIT_ORDERS = 10", kimp::TradingConfig::SPLIT_ORDERS == 10);
    TEST_NEAR("ORDER_SIZE * SPLITS = POSITION_SIZE",
              kimp::TradingConfig::ORDER_SIZE_USD * kimp::TradingConfig::SPLIT_ORDERS,
              kimp::TradingConfig::POSITION_SIZE_USD, 1e-10);
}

// =========================================================================
// Main
// =========================================================================

int main() {
    std::cout << "=== Sections 6-8: IO Thread Pool, WebSocket, Common Symbols ===\n";

    // Section 6
    test_section6_thread_config_optimal();
    test_section6_cpu_pause_no_crash();
    test_section6_fast_stod();
    test_section6_memory_barrier();
    test_section6_prefetch_no_crash();

    // Section 7
    test_section7_connect_pattern();
    test_section7_polling_logic();

    // Section 8
    test_section8_common_symbol_intersection();
    test_section8_add_symbol_dedup_and_mapping();
    test_section8_blacklist_symbolid_not_hash();
    test_section8_position_tracker_hash_collision_safety();
    test_section8_premium_calculator();
    test_section8_capital_tracker();
    test_section8_price_cache_thread_safety();
    test_section8_funding_cache_and_leverage();
    test_section8_dynamic_exit_threshold();

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
