#include "kimp/core/logger.hpp"
#include "kimp/core/types.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace kimp;
using namespace kimp::strategy;

namespace {

constexpr double USDT_KRW = 1000.0;

struct Harness {
    ArbitrageEngine engine;
    std::optional<ArbitrageSignal> signal;

    Harness() {
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::OKX);
        engine.add_exchange_pair(Exchange::Upbit, Exchange::Bybit);
        engine.add_exchange_pair(Exchange::Upbit, Exchange::OKX);
        engine.set_entry_callback([this](const ArbitrageSignal& s) { signal = s; });
    }

    void add_coin(const std::string& base) {
        engine.add_symbol(SymbolId(base, "KRW"));
    }

    void set_book(Exchange ex, const std::string& base, double bid, double ask,
                  double bid_qty, double ask_qty, uint64_t ts_ms = 0) {
        const auto quote = (ex == Exchange::Bithumb || ex == Exchange::Upbit) ? "KRW" : "USDT";
        engine.get_price_cache().update(
            ex, SymbolId(base, quote), bid, ask, (bid + ask) * 0.5, ts_ms, bid_qty, ask_qty);
    }

    void set_usdt_rate(Exchange ex, double rate) {
        Ticker ticker;
        ticker.exchange = ex;
        ticker.symbol = SymbolId("USDT", "KRW");
        ticker.timestamp = std::chrono::steady_clock::now();
        ticker.bid = rate;
        ticker.ask = rate;
        ticker.last = rate;
        ticker.bid_qty = 1'000'000.0;
        ticker.ask_qty = 1'000'000.0;
        engine.on_ticker_update(ticker);
    }

    void set_route(Exchange korean, Exchange foreign, const std::string& base, double fee_coins = 0.0) {
        engine.get_price_cache().set_withdraw_network_fees(
            korean, base, {PriceCache::NetworkFee{"ETH", fee_coins}});
        engine.get_price_cache().set_korean_withdraw_enabled(korean, base, true);
        engine.get_price_cache().set_foreign_deposit_networks(foreign, base, {"ETH"});
        engine.get_price_cache().finalize_withdraw_fees();
    }

    void arm_rates() {
        set_usdt_rate(Exchange::Bithumb, USDT_KRW);
        set_usdt_rate(Exchange::Upbit, USDT_KRW);
    }

    std::vector<ArbitrageEngine::PremiumInfo> premiums() const {
        return engine.get_all_premiums();
    }
};

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void test_blocks_without_transfer_route() {
    std::cout << "TEST 1: transfer route 없으면 진입 차단... ";
    Harness h;
    h.add_coin("AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1955.0, 1960.0, 80.0, 80.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.01, 80.0, 80.0);
    h.arm_rates();
    assert(!h.signal.has_value());
    assert(h.premiums().empty());
    std::cout << "PASS\n";
}

void test_blocks_stale_quotes() {
    std::cout << "TEST 2: stale quote면 진입 차단... ";
    Harness h;
    h.add_coin("AAA");
    const auto stale = now_ms() - TradingConfig::MAX_QUOTE_AGE_MS - 50;
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1955.0, 1960.0, 80.0, 80.0, stale);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.01, 80.0, 80.0, stale);
    h.arm_rates();
    assert(!h.signal.has_value());
    assert(h.premiums().empty());
    std::cout << "PASS\n";
}

void test_blocks_desynced_quotes() {
    std::cout << "TEST 3: KR/Foreign timestamp desync면 차단... ";
    Harness h;
    h.add_coin("AAA");
    const auto base_ts = now_ms();
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1955.0, 1960.0, 80.0, 80.0, base_ts);
    h.set_book(
        Exchange::Bybit, "AAA", 2.0, 2.01, 80.0, 80.0,
        base_ts - TradingConfig::MAX_QUOTE_DESYNC_MS - 50);
    h.arm_rates();
    assert(!h.signal.has_value());
    assert(h.premiums().empty());
    std::cout << "PASS\n";
}

void test_blocks_wide_spread() {
    std::cout << "TEST 4: spread guard를 넘으면 차단... ";
    Harness h;
    h.add_coin("AAA");
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1800.0, 1960.0, 80.0, 80.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.02, 80.0, 80.0);
    h.arm_rates();
    assert(!h.signal.has_value());
    assert(h.premiums().empty());
    std::cout << "PASS\n";
}

void test_blocks_when_top_book_is_too_small() {
    std::cout << "TEST 5: 탑호가로 35 USDT 못 채우면 차단... ";
    Harness h;
    h.add_coin("AAA");
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1955.0, 1960.0, 10.0, 10.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.01, 10.0, 10.0);
    h.arm_rates();
    const auto premiums = h.premiums();
    assert(!h.signal.has_value());
    assert(premiums.size() == 1);
    assert(!premiums.front().both_can_fill_target);
    assert(!premiums.front().entry_signal);
    std::cout << "PASS\n";
}

void test_blocks_when_net_profit_is_below_floor() {
    std::cout << "TEST 6: NetKRW 300 미만이면 차단... ";
    Harness h;
    h.add_coin("AAA");
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1975.0, 1980.0, 100.0, 100.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.01, 100.0, 100.0);
    h.arm_rates();
    const auto premiums = h.premiums();
    assert(!h.signal.has_value());
    assert(premiums.size() == 1);
    assert(premiums.front().net_profit_krw < TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    assert(!premiums.front().entry_signal);
    std::cout << "PASS\n";
}

void test_selects_best_pair_across_all_venues() {
    std::cout << "TEST 7: 4개 조합 중 NetKRW 최상위 페어 선택... ";
    Harness h;
    h.add_coin("AAA");
    for (const auto korean : {Exchange::Bithumb, Exchange::Upbit}) {
        for (const auto foreign : {Exchange::Bybit, Exchange::OKX}) {
            h.set_route(korean, foreign, "AAA");
        }
    }

    h.set_book(Exchange::Bithumb, "AAA", 1987.0, 1990.0, 80.0, 80.0);
    h.set_book(Exchange::Upbit, "AAA", 1982.0, 1985.0, 80.0, 80.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.005, 80.0, 80.0);
    h.set_book(Exchange::OKX, "AAA", 2.01, 2.015, 80.0, 80.0);
    h.arm_rates();

    const auto premiums = h.premiums();
    assert(premiums.size() == 1);
    assert(premiums.front().best_korean_exchange == Exchange::Upbit);
    assert(premiums.front().best_foreign_exchange == Exchange::OKX);
    assert(h.signal.has_value());
    assert(h.signal->korean_exchange == Exchange::Upbit);
    assert(h.signal->foreign_exchange == Exchange::OKX);
    std::cout << "PASS\n";
}

void test_emits_signal_when_all_steps_pass() {
    std::cout << "TEST 8: 모든 게이트 통과 시 entry signal 발생... ";
    Harness h;
    h.add_coin("AAA");
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA");
    h.set_book(Exchange::Bithumb, "AAA", 1955.0, 1960.0, 80.0, 80.0);
    h.set_book(Exchange::Bybit, "AAA", 2.0, 2.01, 80.0, 80.0);
    h.arm_rates();

    const auto premiums = h.premiums();
    assert(h.signal.has_value());
    assert(premiums.size() == 1);
    assert(premiums.front().quote_usable);
    assert(premiums.front().both_can_fill_target);
    assert(premiums.front().net_edge_pct > 0.0);
    assert(premiums.front().net_profit_krw >= TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    assert(premiums.front().entry_signal);
    std::cout << "PASS\n";
}

} // namespace

int main() {
    kimp::Logger::init("logs/kimp_test_entry_pipeline_steps.log", "info", 10, 3, 2048, true);
    test_blocks_without_transfer_route();
    test_blocks_stale_quotes();
    test_blocks_desynced_quotes();
    test_blocks_wide_spread();
    test_blocks_when_top_book_is_too_small();
    test_blocks_when_net_profit_is_below_floor();
    test_selects_best_pair_across_all_venues();
    test_emits_signal_when_all_steps_pass();
    std::cout << "[PASS] step-by-step entry pipeline regression passed\n";
    kimp::Logger::shutdown();
    return 0;
}
