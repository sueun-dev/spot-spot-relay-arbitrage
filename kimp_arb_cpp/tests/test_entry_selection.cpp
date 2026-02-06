/**
 * Test: check_entry_opportunities() 코인 선별 로직 검증
 *
 * 검증 항목:
 * 1. 전체 코인 스캔 → 가장 낮은 프리미엄(역프 최대) 1개 선택
 * 2. 펀딩 간격 8시간 미만 필터링
 * 3. 펀딩비 음수 필터링
 * 4. 프리미엄 -0.75% 초과 필터링
 *
 * 가격 기준 (현실적 데이터):
 * - 빗썸 USDT/KRW: 1,450원
 * - BTC: ~97,000 USDT / ~140,000,000 KRW
 * - ETH: ~2,700 USDT / ~3,900,000 KRW
 * - XRP: ~2.5 USDT / ~3,600 KRW
 * - SOL: ~200 USDT / ~289,000 KRW
 */

#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/core/logger.hpp"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <cmath>

using namespace kimp;
using namespace kimp::strategy;

// 빗썸 USDT/KRW 기준가
static constexpr double USDT_KRW = 1450.0;

// ============================================================
// Helper: 엔진 세팅
// ============================================================
struct TestHarness {
    ArbitrageEngine engine;
    std::optional<ArbitrageSignal> captured_signal;

    TestHarness() {
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);

        // 콜백으로 시그널 캡처
        engine.set_entry_callback([this](const ArbitrageSignal& sig) {
            captured_signal = sig;
        });
    }

    void add_coin(const std::string& base) {
        engine.add_symbol(SymbolId(base, "KRW"));
    }

    // 빗썸 현물 가격 세팅
    void set_korean_price(const std::string& base, double bid, double ask) {
        SymbolId sym(base, "KRW");
        engine.get_price_cache().update(Exchange::Bithumb, sym, bid, ask, (bid + ask) / 2.0);
    }

    // 바이빗 선물 가격 + 펀딩 세팅
    void set_foreign_price(const std::string& base, double bid, double ask,
                           double funding_rate = 0.0001, int funding_interval_h = 8) {
        SymbolId sym(base, "USDT");
        engine.get_price_cache().update(Exchange::Bybit, sym, bid, ask, (bid + ask) / 2.0);
        engine.get_price_cache().update_funding(Exchange::Bybit, sym,
                                                 funding_rate, funding_interval_h, 0);
    }

    // USDT/KRW 환율 세팅 (빗썸 USDT 마켓 기준)
    void set_usdt_rate(double rate) {
        engine.get_price_cache().update_usdt_krw(Exchange::Bithumb, rate);
    }

    // check_entry_opportunities() 트리거:
    // USDT/KRW 틱커를 보내면 on_ticker_update → check_entry_opportunities() 호출
    void trigger_check() {
        captured_signal.reset();
        Ticker usdt_ticker;
        usdt_ticker.exchange = Exchange::Bithumb;
        usdt_ticker.symbol = SymbolId("USDT", "KRW");
        usdt_ticker.bid = USDT_KRW;
        usdt_ticker.ask = USDT_KRW;
        usdt_ticker.last = USDT_KRW;
        usdt_ticker.timestamp = std::chrono::steady_clock::now();
        engine.on_ticker_update(usdt_ticker);
    }

    double calc_premium(double korean_ask, double foreign_bid, double usdt_rate) {
        return PremiumCalculator::calculate_entry_premium(korean_ask, foreign_bid, usdt_rate);
    }
};

// ============================================================
// TEST 1: 전체 스캔 → 가장 낮은 프리미엄 선택
// ============================================================
void test_selects_lowest_premium() {
    std::cout << "TEST 1: 전체 스캔 → 가장 낮은 프리미엄(역프 최대) 선택... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.add_coin("XRP");
    h.add_coin("SOL");
    h.set_usdt_rate(USDT_KRW);

    // 모든 코인: 8h 펀딩, 양수 펀딩비
    // 프리미엄 = ((korean_ask - foreign_bid * usdt) / (foreign_bid * usdt)) * 100

    // BTC: ask=139,500,000  bid=97,000 → foreign_krw=140,650,000 → premium=-0.82%
    h.set_korean_price("BTC", 139400000, 139500000);
    h.set_foreign_price("BTC", 97000, 97050, 0.0001, 8);

    // ETH: ask=3,870,000  bid=2,750 → foreign_krw=3,987,500 → premium=-2.95%  ← 가장 낮음
    h.set_korean_price("ETH", 3860000, 3870000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0002, 8);

    // XRP: ask=3,550  bid=2.52 → foreign_krw=3,654 → premium=-2.85%
    h.set_korean_price("XRP", 3540, 3550);
    h.set_foreign_price("XRP", 2.52, 2.525, 0.0003, 8);

    // SOL: ask=287,000  bid=200 → foreign_krw=290,000 → premium=-1.03%
    h.set_korean_price("SOL", 286500, 287000);
    h.set_foreign_price("SOL", 200, 200.5, 0.0001, 8);

    h.trigger_check();

    assert(h.captured_signal.has_value());
    assert(h.captured_signal->symbol.get_base() == "ETH");

    double expected = h.calc_premium(3870000, 2750, USDT_KRW);
    assert(std::abs(h.captured_signal->premium - expected) < 0.01);

    std::cout << "PASS (ETH 선택, premium=" << h.captured_signal->premium << "%)" << std::endl;
}

// ============================================================
// TEST 2: 펀딩 간격 8시간 미만 필터링
// ============================================================
void test_filters_short_funding_interval() {
    std::cout << "TEST 2: 펀딩 간격 8h 미만 필터링... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.set_usdt_rate(USDT_KRW);

    // BTC: 4시간 펀딩 → 필터링 (프리미엄 더 낮아도)
    // ask=138,000,000  bid=97,000 → foreign_krw=140,650,000 → premium=-1.88%
    h.set_korean_price("BTC", 137900000, 138000000);
    h.set_foreign_price("BTC", 97000, 97050, 0.0001, 4);  // 4h!

    // ETH: 8시간 펀딩 → 통과
    // ask=3,920,000  bid=2,750 → foreign_krw=3,987,500 → premium=-1.69%
    h.set_korean_price("ETH", 3910000, 3920000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0001, 8);

    h.trigger_check();

    assert(h.captured_signal.has_value());
    assert(h.captured_signal->symbol.get_base() == "ETH");

    std::cout << "PASS (BTC 4h 필터링, ETH 8h 통과, premium="
              << h.captured_signal->premium << "%)" << std::endl;
}

// ============================================================
// TEST 3: 펀딩비 음수 필터링
// ============================================================
void test_filters_negative_funding() {
    std::cout << "TEST 3: 펀딩비 음수 필터링... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.set_usdt_rate(USDT_KRW);

    // BTC: 음수 펀딩비 → 필터링 (프리미엄 더 낮아도)
    h.set_korean_price("BTC", 137900000, 138000000);
    h.set_foreign_price("BTC", 97000, 97050, -0.0005, 8);  // 음수!

    // ETH: 양수 펀딩비 → 통과
    h.set_korean_price("ETH", 3920000, 3930000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0001, 8);

    h.trigger_check();

    assert(h.captured_signal.has_value());
    assert(h.captured_signal->symbol.get_base() == "ETH");

    std::cout << "PASS (BTC 음수 펀딩 필터링, ETH 양수 통과, premium="
              << h.captured_signal->premium << "%)" << std::endl;
}

// ============================================================
// TEST 4: 프리미엄 -0.75% 초과 → 진입 안함
// ============================================================
void test_filters_premium_above_threshold() {
    std::cout << "TEST 4: 프리미엄 -0.75% 초과 → 진입 불가... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.set_usdt_rate(USDT_KRW);

    // BTC: ask=140,500,000  bid=97,000 → foreign_krw=140,650,000 → premium=-0.11%
    h.set_korean_price("BTC", 140400000, 140500000);
    h.set_foreign_price("BTC", 97000, 97050, 0.0001, 8);

    // ETH: ask=3,975,000  bid=2,750 → foreign_krw=3,987,500 → premium=-0.31%
    h.set_korean_price("ETH", 3970000, 3975000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0001, 8);

    h.trigger_check();

    assert(!h.captured_signal.has_value());  // 시그널 없어야 함

    double btc_prem = h.calc_premium(140500000, 97000, USDT_KRW);
    double eth_prem = h.calc_premium(3975000, 2750, USDT_KRW);
    std::cout << "PASS (BTC=" << btc_prem << "%, ETH=" << eth_prem
              << "%, 모두 > -0.75% → 시그널 없음)" << std::endl;
}

// ============================================================
// TEST 5: 모든 필터 종합 → 조건 만족하는 것 중 best 선택
// ============================================================
void test_combined_filters_select_best() {
    std::cout << "TEST 5: 종합 필터 → 조건 만족하는 것 중 best... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.add_coin("XRP");
    h.add_coin("SOL");
    h.add_coin("DOGE");
    h.set_usdt_rate(USDT_KRW);

    // BTC: premium=-1.88% but 4h 펀딩 → 필터링
    h.set_korean_price("BTC", 137900000, 138000000);
    h.set_foreign_price("BTC", 97000, 97050, 0.0001, 4);

    // ETH: premium=-1.44%, 8h, 양수 → 통과  ← 필터 통과 중 가장 낮음
    // ask=3,930,000  bid=2,750 → foreign_krw=3,987,500 → premium=-1.44%
    h.set_korean_price("ETH", 3920000, 3930000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0002, 8);

    // XRP: premium=-2.85%, 8h, 음수 펀딩 → 필터링
    h.set_korean_price("XRP", 3540, 3550);
    h.set_foreign_price("XRP", 2.52, 2.525, -0.0001, 8);

    // SOL: premium=-0.86%, 8h, 양수 → 통과 (하지만 ETH보다 프리미엄 높음)
    // ask=287,500  bid=200 → foreign_krw=290,000 → premium=-0.86%
    h.set_korean_price("SOL", 287000, 287500);
    h.set_foreign_price("SOL", 200, 200.5, 0.0001, 8);

    // DOGE: premium=-0.3%, 8h, 양수 → threshold 미달로 필터링
    // ask=289  bid=0.2 → foreign_krw=290 → premium=-0.34%
    h.set_korean_price("DOGE", 288, 289);
    h.set_foreign_price("DOGE", 0.2, 0.2005, 0.0001, 8);

    h.trigger_check();

    assert(h.captured_signal.has_value());
    assert(h.captured_signal->symbol.get_base() == "ETH");

    std::cout << "PASS (ETH 선택 premium=" << h.captured_signal->premium
              << "%, BTC:4h, XRP:음수펀딩, DOGE:threshold, SOL:프리미엄높음)" << std::endl;
}

// ============================================================
// TEST 6: 시그널에 올바른 데이터 포함 확인
// ============================================================
void test_signal_data_correctness() {
    std::cout << "TEST 6: 시그널 데이터 정확성... ";

    TestHarness h;
    h.add_coin("ETH");
    h.set_usdt_rate(USDT_KRW);

    // ETH: ask=3,930,000  bid=2,750 → premium=-1.44%
    h.set_korean_price("ETH", 3920000, 3930000);
    h.set_foreign_price("ETH", 2750, 2755, 0.00025, 8);

    h.trigger_check();

    assert(h.captured_signal.has_value());
    auto& sig = *h.captured_signal;

    assert(sig.symbol.get_base() == "ETH");
    assert(sig.korean_exchange == Exchange::Bithumb);
    assert(sig.foreign_exchange == Exchange::Bybit);
    assert(sig.korean_ask == 3930000.0);   // 빗썸 ask (매수 가격)
    assert(sig.foreign_bid == 2750.0);     // 바이빗 bid (숏 진입 가격)
    assert(std::abs(sig.funding_rate - 0.00025) < 1e-9);
    assert(sig.usdt_krw_rate == USDT_KRW);

    double expected = h.calc_premium(3930000, 2750, USDT_KRW);
    assert(std::abs(sig.premium - expected) < 0.001);

    std::cout << "PASS (ask=" << sig.korean_ask
              << " KRW, bid=" << sig.foreign_bid
              << " USDT, usdt=" << sig.usdt_krw_rate
              << ", funding=" << sig.funding_rate
              << ", premium=" << sig.premium << "%)" << std::endl;
}

// ============================================================
// TEST 7: 포지션 보유 중이면 진입 안함
// ============================================================
void test_no_entry_when_position_held() {
    std::cout << "TEST 7: 포지션 보유 중 진입 불가... ";

    TestHarness h;
    h.add_coin("BTC");
    h.add_coin("ETH");
    h.set_usdt_rate(USDT_KRW);

    // 좋은 프리미엄 세팅
    h.set_korean_price("BTC", 138000000, 138500000);
    h.set_foreign_price("BTC", 97000, 97050, 0.0001, 8);
    h.set_korean_price("ETH", 3920000, 3930000);
    h.set_foreign_price("ETH", 2750, 2755, 0.0001, 8);

    // BTC 포지션 열기
    Position pos;
    pos.symbol = SymbolId("BTC", "KRW");
    pos.korean_exchange = Exchange::Bithumb;
    pos.foreign_exchange = Exchange::Bybit;
    h.engine.open_position(pos);

    h.trigger_check();

    // MAX_POSITIONS=1이므로 새 진입 불가
    assert(!h.captured_signal.has_value());

    std::cout << "PASS (MAX_POSITIONS=1, 포지션 보유 중 시그널 없음)" << std::endl;
}

// ============================================================
// MAIN
// ============================================================
int main() {
    Logger::init("test_entry", "warn");

    std::cout << "\n===== check_entry_opportunities() 코인 선별 테스트 =====" << std::endl;
    std::cout << "빗썸 USDT/KRW 기준: " << USDT_KRW << "원\n" << std::endl;

    test_selects_lowest_premium();
    test_filters_short_funding_interval();
    test_filters_negative_funding();
    test_filters_premium_above_threshold();
    test_combined_filters_select_best();
    test_signal_data_correctness();
    test_no_entry_when_position_held();

    std::cout << "\n===== 전체 7개 테스트 PASS =====\n" << std::endl;
    return 0;
}
