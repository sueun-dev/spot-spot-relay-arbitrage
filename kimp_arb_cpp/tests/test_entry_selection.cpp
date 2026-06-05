#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"

#include "test_check.hpp"
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

    void add_pair(Exchange korean, Exchange foreign) {
        engine.add_exchange_pair(korean, foreign);
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

    void set_korean_book(Exchange ex,
                         const std::string& base,
                         double bid,
                         double ask,
                         double bid_qty,
                         double ask_qty) {
        engine.get_price_cache().update(
            ex,
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

    void set_foreign_book(Exchange ex,
                          const std::string& base,
                          double bid,
                          double ask,
                          double bid_qty,
                          double ask_qty) {
        engine.get_price_cache().update(
            ex,
            SymbolId(base, "USDT"),
            bid,
            ask,
            (bid + ask) * 0.5,
            0,
            bid_qty,
            ask_qty);
    }

    void set_usdt_rate(double rate) {
        set_usdt_rate(Exchange::Bithumb, rate);
    }

    void set_usdt_rate(Exchange ex, double rate) {
        Ticker ticker;
        ticker.exchange = ex;
        ticker.symbol = SymbolId("USDT", "KRW");
        ticker.timestamp = std::chrono::steady_clock::now();
        ticker.bid = rate;
        ticker.ask = rate;
        ticker.last = rate;
        ticker.bid_qty = 1000000.0;
        ticker.ask_qty = 1000000.0;
        engine.on_ticker_update(ticker);
    }

    void set_route(Exchange korean, Exchange foreign, const std::string& base,
                   double fee_coins, const std::string& network = "ETH") {
        engine.get_price_cache().set_withdraw_network_fees(
            korean, base, {PriceCache::NetworkFee{network, fee_coins}});
        engine.get_price_cache().set_foreign_deposit_networks(
            foreign, base, {network});
        engine.get_price_cache().set_korean_withdraw_enabled(korean, base, true);
        engine.get_price_cache().finalize_withdraw_fees();
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

    // CCC fills the 35 USDT target but its net profit stays below the floor, so
    // only BBB (highest net edge) qualifies and must be the selected signal.
    h.set_korean_book("CCC", 1980.0, 1985.0, 20.0, 20.0);
    h.set_foreign_book("CCC", 2.0, 2.01, 20.0, 20.0);

    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "BBB", 0.0);
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "CCC", 0.0);
    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(h.captured_signal.has_value());
    KIMP_CHECK(h.captured_signal->symbol.get_base() == "BBB");
    KIMP_CHECK(h.captured_signal->net_edge_pct > 0.0);
    KIMP_CHECK(h.captured_signal->net_profit_krw >= TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    KIMP_CHECK(h.captured_signal->both_can_fill_target);

    std::cout << "PASS (" << h.captured_signal->symbol.get_base()
              << ", net=" << h.captured_signal->net_edge_pct << "%)" << std::endl;
}

void test_blocks_when_first_tick_cannot_fill_target_usdt() {
    std::cout << "TEST 2: 타깃 USDT 1틱 물량 부족이면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Healthy premium, but each side only has 17 coins (~34 USDT at bid=2),
    // below the 35 USDT target — entry must be blocked on liquidity alone.
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_korean_book("AAA", 1940.0, 1945.0, 17.0, 17.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 17.0, 17.0);
    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_blocks_when_fee_adjusted_net_edge_is_negative() {
    std::cout << "TEST 3: 총수수료 반영 후 음수면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Gross spread is tiny; fee model should flip this negative.
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_korean_book("AAA", 1999.0, 2000.0, 100.0, 100.0);
    h.set_foreign_book("AAA", 2.001, 2.01, 100.0, 100.0);
    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_blocks_when_net_profit_is_below_floor() {
    std::cout << "TEST 4: NetKRW가 진입 최소 수익 미만이면 진입 차단... ";

    TestHarness h;
    h.add_coin("AAA");

    // Positive edge on the 35 USDT target, but projected net profit (~231 KRW)
    // stays below MIN_ENTRY_NET_PROFIT_KRW.
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_korean_book("AAA", 1975.0, 1980.0, 100.0, 100.0);
    h.set_foreign_book("AAA", 2.0, 2.01, 100.0, 100.0);
    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_signal_contains_relay_metrics() {
    std::cout << "TEST 5: 시그널에 relay 지표 포함... ";

    TestHarness h;
    h.add_coin("AAA");

    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_korean_book("AAA", 1940.0, 1945.0, 50.0, 50.0);
    // Foreign spread must stay within MAX_FOREIGN_SPREAD_PCT (0.40%).
    h.set_foreign_book("AAA", 2.0, 2.005, 50.0, 50.0);
    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(h.captured_signal.has_value());
    const auto& sig = *h.captured_signal;
    // target_coin_qty = TARGET_ENTRY_USDT (35) / foreign_bid (2.0) = 17.5
    KIMP_CHECK(std::abs(sig.target_coin_qty - 17.5) < 1e-9);
    KIMP_CHECK(std::abs(sig.korean_ask_qty - 50.0) < 1e-9);
    KIMP_CHECK(std::abs(sig.foreign_bid_qty - 50.0) < 1e-9);
    KIMP_CHECK(std::abs(sig.match_qty - 50.0) < 1e-9);
    KIMP_CHECK(sig.net_edge_pct > 0.0);
    KIMP_CHECK(sig.net_profit_krw >= TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
    KIMP_CHECK(sig.both_can_fill_target);

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

    // BBB clears every entry gate on its own; the only thing that should keep it
    // from firing is the position slot already taken by AAA (MAX_POSITIONS=1).
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "AAA", 0.0);
    h.set_route(Exchange::Bithumb, Exchange::Bybit, "BBB", 0.0);

    Position pos;
    pos.symbol = SymbolId("AAA", "KRW");
    pos.korean_exchange = Exchange::Bithumb;
    pos.foreign_exchange = Exchange::Bybit;
    h.engine.open_position(pos);

    h.set_usdt_rate(USDT_KRW);

    KIMP_CHECK(!h.captured_signal.has_value());
    std::cout << "PASS" << std::endl;
}

void test_multi_exchange_prefers_best_net_route() {
    std::cout << "TEST 7: 다중 거래소 중 gross가 아니라 net 기준 페어 선택... ";

    TestHarness h;
    h.add_pair(Exchange::Upbit, Exchange::OKX);
    h.add_coin("NET");

    h.set_route(Exchange::Bithumb, Exchange::Bybit, "NET", 20.0, "ETH");
    h.set_route(Exchange::Upbit, Exchange::OKX, "NET", 0.05, "ETH");

    h.set_korean_book(Exchange::Bithumb, "NET", 99.0, 100.0, 10000.0, 10000.0);
    h.set_foreign_book(Exchange::Bybit, "NET", 0.0685, 0.0687, 10000.0, 10000.0);

    h.set_korean_book(Exchange::Upbit, "NET", 99.4, 100.5, 10000.0, 10000.0);
    h.set_foreign_book(Exchange::OKX, "NET", 0.0684, 0.0686, 10000.0, 10000.0);

    h.set_usdt_rate(Exchange::Bithumb, 1500.0);
    h.set_usdt_rate(Exchange::Upbit, 1500.0);

    auto premiums = h.engine.get_all_premiums();
    KIMP_CHECK(premiums.size() == 1);
    const auto& p = premiums.front();
    KIMP_CHECK(p.best_korean_exchange == Exchange::Upbit);
    KIMP_CHECK(p.best_foreign_exchange == Exchange::OKX);
    KIMP_CHECK(p.net_profit_krw > 0.0);

    std::cout << "PASS (winner="
              << static_cast<int>(p.best_korean_exchange) << "-"
              << static_cast<int>(p.best_foreign_exchange)
              << ", net=" << p.net_profit_krw << "krw)" << std::endl;
}

}  // namespace

int main() {
    Logger::init("test_entry", "warn");

    std::cout << "\n===== Relay Entry Gate Tests =====\n" << std::endl;
    test_selects_best_net_edge();
    test_blocks_when_first_tick_cannot_fill_target_usdt();
    test_blocks_when_fee_adjusted_net_edge_is_negative();
    test_blocks_when_net_profit_is_below_floor();
    test_signal_contains_relay_metrics();
    test_no_entry_when_position_held();
    test_multi_exchange_prefers_best_net_route();

    std::cout << "\n===== 전체 7개 테스트 PASS =====\n" << std::endl;
    return 0;
}
