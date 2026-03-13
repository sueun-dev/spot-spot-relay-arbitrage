#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>

using namespace kimp;
using namespace kimp::strategy;

namespace {

constexpr double USDT_KRW = 1000.0;

struct TestHarness {
    ArbitrageEngine engine;
    std::optional<ArbitrageSignal> captured_signal;

    TestHarness() {
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);
        engine.set_entry_callback([this](const ArbitrageSignal& sig) {
            captured_signal = sig;
        });
    }

    void add_coin(const std::string& base) {
        engine.add_symbol(SymbolId(base, "KRW"));
    }

    void set_korean_book(const std::string& base,
                         double bid,
                         double ask,
                         double bid_qty,
                         double ask_qty) {
        engine.get_price_cache().update(
            Exchange::Bithumb,
            SymbolId(base, "KRW"),
            bid,
            ask,
            (bid + ask) * 0.5,
            0,
            bid_qty,
            ask_qty);
    }

    void set_foreign_book(const std::string& base,
                          double bid,
                          double ask,
                          double bid_qty,
                          double ask_qty) {
        engine.get_price_cache().update(
            Exchange::Bybit,
            SymbolId(base, "USDT"),
            bid,
            ask,
            (bid + ask) * 0.5,
            0,
            bid_qty,
            ask_qty);
    }

    void set_usdt_rate(double rate) {
        Ticker ticker;
        ticker.exchange = Exchange::Bithumb;
        ticker.symbol = SymbolId("USDT", "KRW");
        ticker.timestamp = std::chrono::steady_clock::now();
        ticker.bid = rate;
        ticker.ask = rate;
        ticker.last = rate;
        ticker.bid_qty = 1000000.0;
        ticker.ask_qty = 1000000.0;
        engine.on_ticker_update(ticker);
    }
};

void test_selects_best_net_edge() {
    std::cout << "TEST 1: positive 1-tick candidates 중 net edge 최상위 선택... ";

    TestHarness h;
    h.add_coin("AAA");
    h.add_coin("BBB");
    h.add_coin("CCC");

    h.set_korean_book("AAA", 1955.0, 1960.0, 80.0, 80.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 80.0, 80.0);

    h.set_korean_book("BBB", 2905.0, 2910.0, 30.0, 30.0);
    h.set_foreign_book("BBB", 3.0, 3.01, 30.0, 30.0);

    // Positive spread but insufficient first-tick liquidity for 70 USDT.
    h.set_korean_book("CCC", 1980.0, 1985.0, 20.0, 20.0);
    h.set_foreign_book("CCC", 2.0, 2.01, 20.0, 20.0);

    h.set_usdt_rate(USDT_KRW);

    assert(h.captured_signal.has_value());
    assert(h.captured_signal->symbol.get_base() == "BBB");
    assert(h.captured_signal->net_edge_pct > 0.0);
    assert(h.captured_signal->net_profit_krw >= TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    assert(h.captured_signal->both_can_fill_target);

    std::cout << "PASS (" << h.captured_signal->symbol.get_base()
              << ", net=" << h.captured_signal->net_edge_pct << "%)" << std::endl;
}

void test_blocks_when_first_tick_cannot_fill_70_usdt() {
    std::cout << "TEST 2: 70 USDT 1틱 물량 부족이면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Bybit bid=2 => target qty = 35, but each side only has 34.
    h.set_korean_book("AAA", 1980.0, 1985.0, 34.0, 34.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 34.0, 34.0);
    h.set_usdt_rate(USDT_KRW);

    assert(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_blocks_when_fee_adjusted_net_edge_is_negative() {
    std::cout << "TEST 3: 총수수료 반영 후 음수면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Gross spread is tiny; fee model should flip this negative.
    h.set_korean_book("AAA", 1999.0, 2000.0, 100.0, 100.0);
    h.set_foreign_book("AAA", 2.001, 2.01, 100.0, 100.0);
    h.set_usdt_rate(USDT_KRW);

    assert(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_blocks_when_net_profit_is_below_800_krw() {
    std::cout << "TEST 4: NetKRW 800 미만이면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Positive edge on the 70 USDT target, but projected net profit stays below 800 KRW.
    h.set_korean_book("AAA", 1975.0, 1980.0, 100.0, 100.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 100.0, 100.0);
    h.set_usdt_rate(USDT_KRW);

    assert(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_signal_contains_relay_metrics() {
    std::cout << "TEST 5: 시그널에 relay 지표 포함... ";

    TestHarness h;
    h.add_coin("AAA");

    h.set_korean_book("AAA", 1955.0, 1960.0, 50.0, 50.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 50.0, 50.0);
    h.set_usdt_rate(USDT_KRW);

    assert(h.captured_signal.has_value());
    const auto& sig = *h.captured_signal;
    assert(std::abs(sig.target_coin_qty - 35.0) < 1e-9);
    assert(std::abs(sig.korean_ask_qty - 50.0) < 1e-9);
    assert(std::abs(sig.foreign_bid_qty - 50.0) < 1e-9);
    assert(std::abs(sig.match_qty - 50.0) < 1e-9);
    assert(sig.net_edge_pct > 0.0);
    assert(sig.net_profit_krw >= TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    assert(sig.both_can_fill_target);

    std::cout << "PASS (targetQty=" << sig.target_coin_qty
              << ", matchQty=" << sig.match_qty
              << ", net=" << sig.net_edge_pct << "%)" << std::endl;
}

void test_no_entry_when_position_held() {
    std::cout << "TEST 6: 포지션 보유 중이면 새 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");
    h.add_coin("BBB");

    h.set_korean_book("AAA", 1955.0, 1960.0, 80.0, 80.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 80.0, 80.0);
    h.set_korean_book("BBB", 2905.0, 2910.0, 30.0, 30.0);
    h.set_foreign_book("BBB", 3.0, 3.01, 30.0, 30.0);

    Position pos;
    pos.symbol = SymbolId("AAA", "KRW");
    pos.korean_exchange = Exchange::Bithumb;
    pos.foreign_exchange = Exchange::Bybit;
    h.engine.open_position(pos);

    h.set_usdt_rate(USDT_KRW);

    assert(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

}  // namespace

int main() {
    Logger::init("test_entry", "warn");

    std::cout << "\n===== Relay Entry Gate Tests =====\n" << std::endl;
    test_selects_best_net_edge();
    test_blocks_when_first_tick_cannot_fill_70_usdt();
    test_blocks_when_fee_adjusted_net_edge_is_negative();
    test_blocks_when_net_profit_is_below_800_krw();
    test_signal_contains_relay_metrics();
    test_no_entry_when_position_held();

    std::cout << "\n===== 전체 6개 테스트 PASS =====\n" << std::endl;
    return 0;
}
