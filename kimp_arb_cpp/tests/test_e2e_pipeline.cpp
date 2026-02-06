/**
 * E2E Pipeline Test: #1 거래소 연결 → #2 시세 수신 → #3 프리미엄 계산
 *
 * 실제 API 데이터로 전체 파이프라인 교차 검증:
 * 1. 거래소 연결 + 심볼 매칭
 * 2. 티커/BBO/USDT 수신
 * 3. PriceCache(struct key) → PremiumCalculator(scalar) → SIMD batch → ArbitrageEngine 일관성
 */

#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/simd_premium.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"

#include <simdjson.h>
#include <boost/asio.hpp>
#include <fmt/format.h>

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

using namespace kimp;
using namespace kimp::exchange;
using namespace kimp::strategy;
namespace net = boost::asio;

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const std::string& name, const std::string& detail = "") {
    if (cond) {
        std::cout << fmt::format("  [PASS] {}", name);
        ++g_pass;
    } else {
        std::cout << fmt::format("  [FAIL] {}", name);
        ++g_fail;
    }
    if (!detail.empty()) std::cout << " — " << detail;
    std::cout << "\n";
}

// ─── Data structs ────────────────────────────────────────────
struct CoinData {
    std::string base;
    double kr_last{0}, kr_bid{0}, kr_ask{0};
    double by_last{0}, by_bid{0}, by_ask{0};
    double funding_rate{0};
};

// ─── Fetch helpers ───────────────────────────────────────────
static double fetch_bithumb_usdt(RestClient& client) {
    auto resp = client.get("/public/ticker/USDT_KRW");
    if (!resp.success) return 0;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);
        std::string_view p = doc["data"]["closing_price"].get_string().value();
        return opt::fast_stod(p);
    } catch (...) { return 0; }
}

struct RawBithumb { std::string base; double last{0}, bid{0}, ask{0}; };

static std::vector<RawBithumb> fetch_bithumb_all(RestClient& client) {
    std::vector<RawBithumb> coins;
    auto resp = client.get("/public/ticker/ALL_KRW");
    if (!resp.success) return coins;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);
        auto data = doc["data"].get_object();
        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key == "date") continue;
            auto item = field.value().get_object();
            auto cp = item["closing_price"];
            if (cp.error()) continue;
            double price = opt::fast_stod(cp.get_string().value());
            if (price <= 0) continue;
            coins.push_back({std::string(key), price, price, price});
        }
    } catch (...) {}

    // Overlay orderbook BBO
    auto resp2 = client.get("/public/orderbook/ALL_KRW?count=1");
    if (resp2.success) {
        try {
            std::unordered_map<std::string, size_t> idx;
            for (size_t i = 0; i < coins.size(); ++i) idx[coins[i].base] = i;
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(resp2.body);
            auto doc = parser.iterate(padded);
            auto data = doc["data"].get_object();
            for (auto field : data) {
                std::string_view key = field.unescaped_key().value();
                if (key == "timestamp" || key == "payment_currency") continue;
                auto it = idx.find(std::string(key));
                if (it == idx.end()) continue;
                auto item = field.value().get_object();
                auto bids_arr = item["bids"];
                if (!bids_arr.error()) {
                    for (auto b : bids_arr.get_array()) {
                        coins[it->second].bid = opt::fast_stod(b["price"].get_string().value());
                        break;
                    }
                }
                auto asks_arr = item["asks"];
                if (!asks_arr.error()) {
                    for (auto a : asks_arr.get_array()) {
                        coins[it->second].ask = opt::fast_stod(a["price"].get_string().value());
                        break;
                    }
                }
            }
        } catch (...) {}
    }
    return coins;
}

struct RawBybit { std::string base; double last{0}, bid{0}, ask{0}; double funding_rate{0}; };

static std::vector<RawBybit> fetch_bybit_all(RestClient& client) {
    std::vector<RawBybit> coins;
    auto resp = client.get("/v5/market/tickers?category=linear");
    if (!resp.success) return coins;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();
        for (auto item : list) {
            std::string_view sym = item["symbol"].get_string().value();
            if (sym.size() < 5 || sym.substr(sym.size() - 4) != "USDT") continue;
            RawBybit c;
            c.base = std::string(sym.substr(0, sym.size() - 4));
            c.last = opt::fast_stod(item["lastPrice"].get_string().value());
            c.bid = opt::fast_stod(item["bid1Price"].get_string().value());
            c.ask = opt::fast_stod(item["ask1Price"].get_string().value());
            auto fr = item["fundingRate"];
            if (!fr.error()) c.funding_rate = opt::fast_stod(fr.get_string().value());
            if (c.last > 0 && c.bid > 0 && c.ask > 0)
                coins.push_back(std::move(c));
        }
    } catch (...) {}
    return coins;
}

// ─── Main ────────────────────────────────────────────────────
int main() {
    Logger::init("test_e2e_pipeline", "warn");

    std::cout << "\n========================================\n";
    std::cout << "  E2E Pipeline Test: #1→#2→#3 검증\n";
    std::cout << "========================================\n\n";

    net::io_context ioc;

    // ═══════════════════════════════════════════════════════════
    // #1 거래소 연결 + 데이터 수집
    // ═══════════════════════════════════════════════════════════
    std::cout << "[#1] 거래소 연결\n";

    RestClient bithumb_client(ioc, "api.bithumb.com");
    check(bithumb_client.initialize(), "Bithumb REST 연결");

    RestClient bybit_client(ioc, "api.bybit.com");
    check(bybit_client.initialize(), "Bybit REST 연결");

    double usdt_krw = fetch_bithumb_usdt(bithumb_client);
    check(usdt_krw >= 1200 && usdt_krw <= 1700,
          "USDT/KRW 수신", fmt::format("{:.2f}", usdt_krw));

    auto bithumb_raw = fetch_bithumb_all(bithumb_client);
    check(bithumb_raw.size() >= 100, "Bithumb 코인 수신",
          fmt::format("{} coins", bithumb_raw.size()));

    auto bybit_raw = fetch_bybit_all(bybit_client);
    check(bybit_raw.size() >= 200, "Bybit 코인 수신",
          fmt::format("{} coins", bybit_raw.size()));

    bithumb_client.shutdown();
    bybit_client.shutdown();

    // Build matched coin list
    std::unordered_map<std::string, const RawBybit*> bybit_map;
    for (const auto& c : bybit_raw) bybit_map[c.base] = &c;

    std::vector<CoinData> matched;
    for (const auto& kr : bithumb_raw) {
        auto it = bybit_map.find(kr.base);
        if (it == bybit_map.end()) continue;
        const auto& by = *it->second;
        CoinData cd;
        cd.base = kr.base;
        cd.kr_last = kr.last; cd.kr_bid = kr.bid; cd.kr_ask = kr.ask;
        cd.by_last = by.last; cd.by_bid = by.bid; cd.by_ask = by.ask;
        cd.funding_rate = by.funding_rate;
        matched.push_back(std::move(cd));
    }
    check(matched.size() >= 200, "공통 심볼 매칭",
          fmt::format("{} coins", matched.size()));

    std::cout << "\n";

    // ═══════════════════════════════════════════════════════════
    // #2 시세 정합성 (bid/ask 방향, 스프레드)
    // ═══════════════════════════════════════════════════════════
    std::cout << "[#2] 시세 정합성\n";

    int kr_bbo_ok = 0, by_bbo_ok = 0;
    int kr_spread_bad = 0, by_spread_bad = 0;
    for (const auto& c : matched) {
        if (c.kr_bid > 0 && c.kr_ask > 0 && c.kr_bid != c.kr_ask) ++kr_bbo_ok;
        if (c.by_bid > 0 && c.by_ask > 0 && c.by_bid != c.by_ask) ++by_bbo_ok;
        if (c.kr_bid > 0 && c.kr_ask > 0 && c.kr_bid > c.kr_ask * 1.01) ++kr_spread_bad;
        if (c.by_bid > 0 && c.by_ask > 0 && c.by_bid > c.by_ask * 1.001) ++by_spread_bad;
    }
    check(kr_bbo_ok > (int)matched.size() / 2, "Bithumb BBO 유효 (50%+)",
          fmt::format("{}/{}", kr_bbo_ok, matched.size()));
    check(by_bbo_ok > (int)matched.size() / 2, "Bybit BBO 유효 (50%+)",
          fmt::format("{}/{}", by_bbo_ok, matched.size()));
    check(kr_spread_bad == 0, "Bithumb 스프레드 정상",
          fmt::format("{} bad", kr_spread_bad));
    check(by_spread_bad == 0, "Bybit 스프레드 정상",
          fmt::format("{} bad", by_spread_bad));

    std::cout << "\n";

    // ═══════════════════════════════════════════════════════════
    // #3 프리미엄 계산: 4경로 교차 검증
    // ═══════════════════════════════════════════════════════════
    std::cout << "[#3] 프리미엄 계산 교차 검증\n";

    // ─── Method A: 수동 계산 (ground truth) ───────────────────
    std::vector<double> manual_premiums;
    std::vector<size_t> valid_indices;
    manual_premiums.reserve(matched.size());
    valid_indices.reserve(matched.size());

    for (size_t i = 0; i < matched.size(); ++i) {
        const auto& c = matched[i];
        if (c.kr_ask <= 0 || c.by_bid <= 0 || usdt_krw <= 0) continue;
        double fk = c.by_bid * usdt_krw;
        double pm = ((c.kr_ask - fk) / fk) * 100.0;
        if (!std::isfinite(pm) || std::fabs(pm) > 50.0) continue;
        manual_premiums.push_back(pm);
        valid_indices.push_back(i);
    }

    check(!manual_premiums.empty(), "유효 프리미엄 계산",
          fmt::format("{}/{}", manual_premiums.size(), matched.size()));

    // ─── Method B: PremiumCalculator scalar ───────────────────
    int scalar_match = 0;
    double scalar_max_diff = 0;
    for (size_t j = 0; j < valid_indices.size(); ++j) {
        const auto& c = matched[valid_indices[j]];
        double scalar_pm = PremiumCalculator::calculate_entry_premium(
            c.kr_ask, c.by_bid, usdt_krw);
        double diff = std::fabs(scalar_pm - manual_premiums[j]);
        scalar_max_diff = std::max(scalar_max_diff, diff);
        if (diff < 1e-10) ++scalar_match;
    }
    check(scalar_match == (int)valid_indices.size(),
          "PremiumCalculator vs 수동 일치",
          fmt::format("{}/{}, max_diff={:.2e}", scalar_match, valid_indices.size(), scalar_max_diff));

    // ─── Method C: SIMD batch ─────────────────────────────────
    const size_t n = valid_indices.size();
    std::vector<double> kr_arr(n), by_arr(n), usdt_arr(n), simd_result(n);
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        kr_arr[j] = c.kr_ask;
        by_arr[j] = c.by_bid;
        usdt_arr[j] = usdt_krw;
    }

    SIMDPremiumCalculator::calculate_batch(
        kr_arr.data(), by_arr.data(), usdt_arr.data(), simd_result.data(), n);

    int simd_match = 0;
    double simd_max_diff = 0;
    for (size_t j = 0; j < n; ++j) {
        double diff = std::fabs(simd_result[j] - manual_premiums[j]);
        simd_max_diff = std::max(simd_max_diff, diff);
        // FMA may produce slightly different results due to reduced intermediate rounding
        if (diff < 1e-8) ++simd_match;
    }
    check(simd_match == (int)n,
          fmt::format("SIMD({}) vs 수동 일치", SIMDPremiumCalculator::get_simd_type()),
          fmt::format("{}/{}, max_diff={:.2e}", simd_match, n, simd_max_diff));

    // ─── Method D: ArbitrageEngine full pipeline ──────────────
    // PriceCache(struct key) → add_symbol → get_all_premiums() → 비교
    ArbitrageEngine engine;
    engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);

    // Feed all matched coins into engine
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        SymbolId kr_sym(c.base, "KRW");
        SymbolId by_sym(c.base, "USDT");
        engine.add_symbol(kr_sym);

        // Feed into PriceCache
        engine.get_price_cache().update(Exchange::Bithumb, kr_sym,
                                         c.kr_bid, c.kr_ask, c.kr_last);
        engine.get_price_cache().update(Exchange::Bybit, by_sym,
                                         c.by_bid, c.by_ask, c.by_last);
        engine.get_price_cache().update_funding(Exchange::Bybit, by_sym,
                                                 c.funding_rate, 8, 0);
    }
    engine.get_price_cache().update_usdt_krw(Exchange::Bithumb, usdt_krw);

    // Verify PriceCache struct key roundtrip
    int cache_ok = 0;
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        SymbolId kr_sym(c.base, "KRW");
        auto pd = engine.get_price_cache().get_price(Exchange::Bithumb, kr_sym);
        if (pd.valid && pd.bid == c.kr_bid && pd.ask == c.kr_ask && pd.last == c.kr_last) {
            ++cache_ok;
        }
    }
    check(cache_ok == (int)n, "PriceCache struct key 라운드트립",
          fmt::format("{}/{}", cache_ok, n));

    // Verify USDT/KRW roundtrip
    double cached_usdt = engine.get_price_cache().get_usdt_krw(Exchange::Bithumb);
    check(cached_usdt == usdt_krw, "PriceCache USDT/KRW 라운드트립",
          fmt::format("cached={:.2f} actual={:.2f}", cached_usdt, usdt_krw));

    // get_all_premiums() - full SoA + SIMD pipeline
    auto all_pm = engine.get_all_premiums();
    check((int)all_pm.size() == (int)n, "get_all_premiums 결과 수",
          fmt::format("{}/{}", all_pm.size(), n));

    // Build lookup for comparison (use string key to avoid hash collision)
    std::unordered_map<std::string, double> engine_pm_map;
    for (const auto& info : all_pm) {
        engine_pm_map[info.symbol.to_string()] = info.premium;
    }

    int engine_match = 0;
    double engine_max_diff = 0;
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        SymbolId kr_sym(c.base, "KRW");
        auto it = engine_pm_map.find(kr_sym.to_string());
        if (it == engine_pm_map.end()) continue;
        double diff = std::fabs(it->second - manual_premiums[j]);
        engine_max_diff = std::max(engine_max_diff, diff);
        if (diff < 1e-8) ++engine_match;
    }
    check(engine_match == (int)n,
          "ArbitrageEngine pipeline vs 수동 일치",
          fmt::format("{}/{}, max_diff={:.2e}", engine_match, n, engine_max_diff));

    // ─── Entry/Exit 방향 검증 ─────────────────────────────────
    std::cout << "\n[#3b] Entry/Exit bid/ask 방향 검증\n";

    // Entry: korean ASK, foreign BID (worst case for buyer)
    // Exit:  korean BID, foreign ASK (worst case for seller)
    // For any coin, entry_premium > exit_premium (ask > bid, bid < ask)
    int direction_ok = 0, direction_bad = 0;
    std::string direction_bad_detail;
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        double entry_pm = PremiumCalculator::calculate_entry_premium(
            c.kr_ask, c.by_bid, usdt_krw);
        double exit_pm = PremiumCalculator::calculate_exit_premium(
            c.kr_bid, c.by_ask, usdt_krw);
        // entry uses ask (higher), exit uses bid (lower) on Korean side
        // entry uses bid (lower), exit uses ask (higher) on foreign side
        // → entry premium should be >= exit premium
        if (entry_pm >= exit_pm - 1e-10) {
            ++direction_ok;
        } else {
            ++direction_bad;
            if (direction_bad <= 3) {
                direction_bad_detail += fmt::format("{}(entry={:.4f}%,exit={:.4f}%) ",
                    c.base, entry_pm, exit_pm);
            }
        }
    }
    check(direction_bad == 0,
          "entry_pm >= exit_pm (전 코인)",
          direction_bad > 0 ? fmt::format("{} bad: {}", direction_bad, direction_bad_detail)
                            : fmt::format("all {} OK", direction_ok));

    // Spread between entry and exit should be positive (this is the cost of round-trip)
    double avg_spread = 0;
    for (size_t j = 0; j < n; ++j) {
        const auto& c = matched[valid_indices[j]];
        double entry_pm = PremiumCalculator::calculate_entry_premium(
            c.kr_ask, c.by_bid, usdt_krw);
        double exit_pm = PremiumCalculator::calculate_exit_premium(
            c.kr_bid, c.by_ask, usdt_krw);
        avg_spread += (entry_pm - exit_pm);
    }
    avg_spread /= static_cast<double>(n);
    check(avg_spread > 0, "평균 entry-exit 스프레드 > 0",
          fmt::format("{:.4f}%%", avg_spread));

    // ─── calculate_premium() 단일 심볼 검증 ───────────────────
    std::cout << "\n[#3c] calculate_premium() 단일 심볼 검증\n";

    int calc_pm_ok = 0;
    double calc_pm_max_diff = 0;
    for (size_t j = 0; j < std::min(n, (size_t)50); ++j) {
        const auto& c = matched[valid_indices[j]];
        SymbolId kr_sym(c.base, "KRW");
        double engine_pm = engine.calculate_premium(kr_sym, Exchange::Bithumb, Exchange::Bybit);
        double diff = std::fabs(engine_pm - manual_premiums[j]);
        calc_pm_max_diff = std::max(calc_pm_max_diff, diff);
        if (diff < 1e-10) ++calc_pm_ok;
    }
    int checked_count = static_cast<int>(std::min(n, (size_t)50));
    check(calc_pm_ok == checked_count,
          "calculate_premium() vs 수동 일치",
          fmt::format("{}/{}, max_diff={:.2e}", calc_pm_ok, checked_count, calc_pm_max_diff));

    // ─── 프리미엄 범위/분포 출력 ──────────────────────────────
    std::cout << "\n[#3d] 프리미엄 분포\n";

    double pm_min = 999, pm_max = -999;
    std::string pm_min_coin, pm_max_coin;
    int entry_cand = 0, exit_cand = 0;
    for (size_t j = 0; j < n; ++j) {
        double pm = manual_premiums[j];
        const auto& c = matched[valid_indices[j]];
        if (pm < pm_min) { pm_min = pm; pm_min_coin = c.base; }
        if (pm > pm_max) { pm_max = pm; pm_max_coin = c.base; }
        if (pm <= TradingConfig::ENTRY_PREMIUM_THRESHOLD) ++entry_cand;
        if (pm >= TradingConfig::EXIT_PREMIUM_THRESHOLD) ++exit_cand;
    }

    check(pm_min > -30.0 && pm_max < 30.0,
          "프리미엄 범위 합리성 (-30%~+30%)",
          fmt::format("min={:.4f}%({}) max={:.4f}%({})", pm_min, pm_min_coin, pm_max, pm_max_coin));

    std::cout << fmt::format("  [INFO] Entry candidates (pm <= {:.2f}%%): {}\n",
                              TradingConfig::ENTRY_PREMIUM_THRESHOLD, entry_cand);
    std::cout << fmt::format("  [INFO] Exit candidates  (pm >= {:.2f}%%): {}\n",
                              TradingConfig::EXIT_PREMIUM_THRESHOLD, exit_cand);

    // ─── entry_signal / exit_signal 플래그 검증 ────────────────
    std::cout << "\n[#3e] entry/exit 시그널 플래그 검증\n";

    // entry_signal uses entry premium (info.premium = ask/bid)
    // exit_signal uses SEPARATE exit premium (bid/ask) — not stored in info.premium
    // So we can only verify entry_signal against info.premium directly.
    // For exit_signal, verify consistency: exit_signal implies exit_pm >= threshold,
    // and exit_pm <= entry_pm always (bid <= ask), so exit_signal should never fire
    // when entry_signal fires (entry_pm is very negative → exit_pm even more negative).
    int entry_ok = 0, entry_bad = 0;
    int exit_consistency_ok = 0, exit_consistency_bad = 0;
    for (const auto& info : all_pm) {
        // entry_signal check: directly testable
        bool expect_entry = info.premium <= TradingConfig::ENTRY_PREMIUM_THRESHOLD;
        if (info.entry_signal == expect_entry) {
            ++entry_ok;
        } else {
            ++entry_bad;
        }
        // exit_signal consistency: entry + exit should never both be true
        // (entry needs pm << 0, exit needs pm >> 0, and exit_pm < entry_pm always)
        if (info.entry_signal && info.exit_signal) {
            ++exit_consistency_bad;
        } else {
            ++exit_consistency_ok;
        }
    }
    check(entry_bad == 0, "entry_signal 플래그 정확성",
          fmt::format("{}/{} correct", entry_ok, entry_ok + entry_bad));
    check(exit_consistency_bad == 0, "entry+exit 동시 불가 일관성",
          fmt::format("{}/{} consistent", exit_consistency_ok, exit_consistency_ok + exit_consistency_bad));

    // ═══════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════
    std::cout << fmt::format("\n========================================\n");
    std::cout << fmt::format("  E2E 결과: {}/{} PASS\n", g_pass, g_pass + g_fail);
    std::cout << fmt::format("========================================\n\n");

    Logger::shutdown();
    return g_fail > 0 ? 1 : 0;
}
