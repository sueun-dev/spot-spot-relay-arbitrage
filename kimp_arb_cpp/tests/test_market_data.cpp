/**
 * Market Data Reception Test: 시세 수신 파이프라인 검증
 *
 * 1. Bithumb REST: 티커(closePrice) + 오더북(bid/ask) → BBO 오버레이 검증
 * 2. Bybit REST: 티커(bid1Price/ask1Price) 직접 BBO 검증
 * 3. USDT/KRW 환율 합리성
 * 4. 스프레드 정상성: ask >= bid
 * 5. 프리미엄 계산 일관성
 */

#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
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
struct BithumbCoin {
    std::string base;
    double last{0}, bid{0}, ask{0};
};

struct BybitCoin {
    std::string base;
    double last{0}, bid{0}, ask{0};
    double funding_rate{0};
};

// ─── Fetch helpers ───────────────────────────────────────────
static double fetch_bithumb_usdt([[maybe_unused]] net::io_context& ioc, RestClient& client) {
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

static std::vector<BithumbCoin> fetch_bithumb_tickers(RestClient& client) {
    std::vector<BithumbCoin> coins;
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
            BithumbCoin c;
            c.base = std::string(key);
            c.last = price;
            c.bid = price;  // default: last (no BBO yet)
            c.ask = price;
            coins.push_back(std::move(c));
        }
    } catch (...) {}
    return coins;
}

static int overlay_bithumb_orderbook(RestClient& client, std::vector<BithumbCoin>& coins) {
    int overlayed = 0;
    auto resp = client.get("/public/orderbook/ALL_KRW?count=1");
    if (!resp.success) return 0;
    try {
        std::unordered_map<std::string, size_t> idx;
        for (size_t i = 0; i < coins.size(); ++i) idx[coins[i].base] = i;

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);
        auto status = doc["status"].get_string().value();
        if (status != "0000") return 0;

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
            if (coins[it->second].bid != coins[it->second].last ||
                coins[it->second].ask != coins[it->second].last) {
                ++overlayed;
            }
        }
    } catch (...) {}
    return overlayed;
}

static std::vector<BybitCoin> fetch_bybit_tickers(RestClient& client) {
    std::vector<BybitCoin> coins;
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
            BybitCoin c;
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
    Logger::init("test_market_data", "warn");

    std::cout << "\n=== 시세 수신 파이프라인 검증 ===\n\n";

    net::io_context ioc;

    // ─── Section 1: USDT/KRW ─────────────────────────────────
    std::cout << "[1] USDT/KRW 환율\n";
    RestClient bithumb_client(ioc, "api.bithumb.com");
    check(bithumb_client.initialize(), "Bithumb REST 연결");

    double usdt_krw = fetch_bithumb_usdt(ioc, bithumb_client);
    check(usdt_krw > 0, "USDT/KRW 수신", fmt::format("{:.2f}", usdt_krw));
    check(usdt_krw >= 1200 && usdt_krw <= 1700,
          "USDT/KRW 범위 (1200~1700)", fmt::format("{:.2f}", usdt_krw));

    // ─── Section 2: Bithumb 티커 + 오더북 BBO ────────────────
    std::cout << "\n[2] Bithumb 티커 + 오더북 BBO\n";
    auto bithumb_coins = fetch_bithumb_tickers(bithumb_client);
    check(!bithumb_coins.empty(), "Bithumb 티커 수신",
          fmt::format("{} coins", bithumb_coins.size()));
    check(bithumb_coins.size() >= 100, "Bithumb 코인 수 >= 100",
          fmt::format("{}", bithumb_coins.size()));

    // Before overlay: all bid == ask == last
    int same_before = 0;
    for (const auto& c : bithumb_coins) {
        if (c.bid == c.last && c.ask == c.last) ++same_before;
    }
    check(same_before == (int)bithumb_coins.size(),
          "오버레이 전: bid=ask=last",
          fmt::format("{}/{}", same_before, bithumb_coins.size()));

    // Apply orderbook overlay
    int overlayed = overlay_bithumb_orderbook(bithumb_client, bithumb_coins);
    check(overlayed > 0, "오더북 BBO 오버레이 적용",
          fmt::format("{} coins got real bid/ask", overlayed));
    check(overlayed >= 100, "오버레이 코인 >= 100",
          fmt::format("{}", overlayed));

    // After overlay: bid != ask for many coins
    int different_after = 0;
    int bid_gt_ask = 0;
    for (const auto& c : bithumb_coins) {
        if (c.bid != c.ask) ++different_after;
        if (c.bid > c.ask && c.bid > 0 && c.ask > 0) ++bid_gt_ask;
    }
    check(different_after > 0, "오버레이 후: bid != ask 존재",
          fmt::format("{}/{}", different_after, bithumb_coins.size()));

    // Spread sanity: ask >= bid (allow small tolerance for cross)
    int bad_spread = 0;
    std::string bad_spread_detail;
    for (const auto& c : bithumb_coins) {
        if (c.ask > 0 && c.bid > 0 && c.bid > c.ask * 1.01) {
            ++bad_spread;
            if (bad_spread <= 3) {
                bad_spread_detail += fmt::format("{}(bid={:.0f},ask={:.0f}) ", c.base, c.bid, c.ask);
            }
        }
    }
    check(bad_spread == 0, "Bithumb 스프레드 정상 (bid <= ask*1.01)",
          bad_spread > 0 ? fmt::format("{} bad: {}", bad_spread, bad_spread_detail) : "all OK");

    // Check specific coins
    std::vector<std::string> check_coins = {"BTC", "ETH", "XRP", "SOL", "DOGE"};
    std::unordered_map<std::string, const BithumbCoin*> bithumb_map;
    for (const auto& c : bithumb_coins) bithumb_map[c.base] = &c;

    for (const auto& name : check_coins) {
        auto it = bithumb_map.find(name);
        if (it != bithumb_map.end()) {
            const auto& c = *it->second;
            check(c.last > 0, fmt::format("Bithumb {} last > 0", name),
                  fmt::format("last={:.0f} bid={:.0f} ask={:.0f}", c.last, c.bid, c.ask));
        }
    }

    bithumb_client.shutdown();

    // ─── Section 3: Bybit 티커 ───────────────────────────────
    std::cout << "\n[3] Bybit 티커 (BBO 포함)\n";
    RestClient bybit_client(ioc, "api.bybit.com");
    check(bybit_client.initialize(), "Bybit REST 연결");

    auto bybit_coins = fetch_bybit_tickers(bybit_client);
    check(!bybit_coins.empty(), "Bybit 티커 수신",
          fmt::format("{} coins", bybit_coins.size()));
    check(bybit_coins.size() >= 200, "Bybit 코인 수 >= 200",
          fmt::format("{}", bybit_coins.size()));

    // Bybit should have bid != ask for most coins (real BBO in ticker)
    int bybit_bbo_ok = 0;
    for (const auto& c : bybit_coins) {
        if (c.bid > 0 && c.ask > 0 && c.bid != c.ask) ++bybit_bbo_ok;
    }
    check(bybit_bbo_ok > (int)bybit_coins.size() / 2,
          "Bybit bid != ask (50% 이상)",
          fmt::format("{}/{}", bybit_bbo_ok, bybit_coins.size()));

    // Spread sanity
    int bybit_bad_spread = 0;
    for (const auto& c : bybit_coins) {
        if (c.ask > 0 && c.bid > c.ask * 1.001) ++bybit_bad_spread;
    }
    check(bybit_bad_spread == 0, "Bybit 스프레드 정상 (bid <= ask*1.001)",
          bybit_bad_spread > 0 ? fmt::format("{} bad", bybit_bad_spread) : "all OK");

    // Funding rate sanity
    int has_funding = 0;
    for (const auto& c : bybit_coins) {
        if (std::fabs(c.funding_rate) > 0) ++has_funding;
    }
    check(has_funding > (int)bybit_coins.size() / 2,
          "Bybit 펀딩비 수신 (50% 이상)",
          fmt::format("{}/{}", has_funding, bybit_coins.size()));

    // Check specific coins
    std::unordered_map<std::string, const BybitCoin*> bybit_map;
    for (const auto& c : bybit_coins) bybit_map[c.base] = &c;

    for (const auto& name : check_coins) {
        auto it = bybit_map.find(name);
        if (it != bybit_map.end()) {
            const auto& c = *it->second;
            check(c.bid > 0 && c.ask > 0,
                  fmt::format("Bybit {} bid/ask > 0", name),
                  fmt::format("bid={:.6f} ask={:.6f} fr={:.6f}", c.bid, c.ask, c.funding_rate));
        }
    }

    bybit_client.shutdown();

    // ─── Section 4: 크로스 검증 (프리미엄 계산) ──────────────
    std::cout << "\n[4] 프리미엄 계산 크로스 검증\n";

    int matched = 0, premium_valid = 0, premium_extreme = 0;
    double min_pm = 999, max_pm = -999;
    std::string min_pm_coin, max_pm_coin;

    for (const auto& bc : bithumb_coins) {
        auto it = bybit_map.find(bc.base);
        if (it == bybit_map.end()) continue;
        const auto& by = *it->second;
        ++matched;

        if (usdt_krw > 0 && by.bid > 0 && bc.ask > 0) {
            double fk = by.bid * usdt_krw;
            double entry_pm = ((bc.ask - fk) / fk) * 100.0;
            if (std::isfinite(entry_pm) && std::fabs(entry_pm) < 50.0) {
                ++premium_valid;
                if (std::fabs(entry_pm) >= 20.0) ++premium_extreme;
                if (entry_pm < min_pm) { min_pm = entry_pm; min_pm_coin = bc.base; }
                if (entry_pm > max_pm) { max_pm = entry_pm; max_pm_coin = bc.base; }
            }
        }
    }

    check(matched >= 200, "공통 코인 >= 200",
          fmt::format("{}", matched));
    check(premium_valid >= matched * 0.9,
          "프리미엄 계산 성공 >= 90%",
          fmt::format("{}/{}", premium_valid, matched));
    int extreme_limit = std::max(3, premium_valid / 20);  // <=5% or up to 3 outliers
    check(premium_extreme <= extreme_limit,
          "프리미엄 극단치 비율 (|pm|>=20%) <= 5%",
          fmt::format("{}/{} (limit {}), min={:.4f}%({}) max={:.4f}%({})",
                      premium_extreme, premium_valid, extreme_limit,
                      min_pm, min_pm_coin, max_pm, max_pm_coin));

    // Entry candidates
    int entry_candidates = 0;
    for (const auto& bc : bithumb_coins) {
        auto it = bybit_map.find(bc.base);
        if (it == bybit_map.end()) continue;
        if (usdt_krw > 0 && it->second->bid > 0 && bc.ask > 0) {
            double fk = it->second->bid * usdt_krw;
            double pm = ((bc.ask - fk) / fk) * 100.0;
            if (pm <= TradingConfig::ENTRY_PREMIUM_THRESHOLD) ++entry_candidates;
        }
    }
    std::cout << fmt::format("  [INFO] Entry candidates (pm <= {:.2f}%%): {}\n",
                              TradingConfig::ENTRY_PREMIUM_THRESHOLD, entry_candidates);

    // ─── Section 5: Bybit 심볼 파싱 검증 ────────────────────
    std::cout << "\n[5] Bybit 심볼 파싱 (suffix 제거)\n";
    // Verify the hardcoded -4 suffix removal works for all symbols
    int parse_ok = 0, parse_bad = 0;
    std::string bad_symbols;
    for (const auto& c : bybit_coins) {
        // Reconstruct: base + "USDT" should give valid symbol
        std::string reconstructed = c.base + "USDT";
        // base should not be empty and should not contain "USDT"
        if (!c.base.empty() && c.base.find("USDT") == std::string::npos) {
            ++parse_ok;
        } else {
            ++parse_bad;
            if (parse_bad <= 5) bad_symbols += c.base + " ";
        }
    }
    check(parse_bad == 0, "Bybit 심볼 파싱 정확성",
          parse_bad > 0 ? fmt::format("{} bad: {}", parse_bad, bad_symbols) :
          fmt::format("all {} OK", parse_ok));

    // ─── Summary ─────────────────────────────────────────────
    std::cout << fmt::format("\n=== 결과: {}/{} PASS ===\n\n",
                              g_pass, g_pass + g_fail);

    Logger::shutdown();
    return g_fail > 0 ? 1 : 0;
}
