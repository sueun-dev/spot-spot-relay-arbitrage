/**
 * Price Verification Test: bid/ask 가격 정확성 10분 실시간 검증
 *
 * 빗썸 + 바이빗 REST API에서 bid/ask를 가져와서:
 * 1. Entry premium (bithumb_ask, bybit_bid) 계산
 * 2. Exit premium (bithumb_bid, bybit_ask) 계산
 * 3. 두 프리미엄 차이 (spread) 확인
 * 4. 10초마다 반복, 10분간
 */

#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <simdjson.h>
#include <boost/asio.hpp>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <fmt/format.h>

using namespace kimp;
using namespace kimp::strategy;
using namespace kimp::exchange;
namespace net = boost::asio;

static std::atomic<bool> g_stop{false};
void sig_handler(int) { g_stop = true; }

struct BithumbCoin {
    std::string base;
    double bid{0}, ask{0}, last{0};
};

struct BybitCoin {
    std::string base;
    double bid{0}, ask{0}, last{0};
    double funding_rate{0};
    int funding_interval_hours{8};
};

struct FetchResult {
    double usdt_krw{0};
    std::vector<BithumbCoin> bithumb;
    std::vector<BybitCoin> bybit;

    bool fetch_all(net::io_context& ioc) {
        return fetch_bithumb(ioc) && fetch_bybit(ioc);
    }

private:
    bool fetch_bithumb(net::io_context& ioc) {
        RestClient client(ioc, "api.bithumb.com");
        if (!client.initialize()) return false;

        // USDT/KRW
        auto usdt_resp = client.get("/public/ticker/USDT_KRW");
        if (usdt_resp.success) {
            try {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(usdt_resp.body);
                auto doc = parser.iterate(padded);
                std::string_view p = doc["data"]["closing_price"].get_string().value();
                usdt_krw = opt::fast_stod(p);
            } catch (...) {}
        }

        // ALL tickers
        auto all_resp = client.get("/public/ticker/ALL_KRW");
        if (!all_resp.success) { client.shutdown(); return false; }

        try {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(all_resp.body);
            auto doc = parser.iterate(padded);
            auto data = doc["data"].get_object();
            for (auto field : data) {
                std::string_view key = field.unescaped_key().value();
                if (key == "date") continue;
                auto item = field.value().get_object();
                auto cp = item["closing_price"];
                if (cp.error()) continue;
                std::string_view ps = cp.get_string().value();
                double price = opt::fast_stod(ps);
                if (price <= 0) continue;
                BithumbCoin c;
                c.base = std::string(key);
                c.last = price;
                c.bid = price;
                c.ask = price;
                bithumb.push_back(std::move(c));
            }
        } catch (...) { client.shutdown(); return false; }

        // Orderbook for real bid/ask
        auto ob_resp = client.get("/public/orderbook/ALL_KRW?count=1");
        if (ob_resp.success) {
            try {
                simdjson::ondemand::parser parser;
                simdjson::padded_string padded(ob_resp.body);
                auto doc = parser.iterate(padded);
                auto status = doc["status"].get_string().value();
                if (status == "0000") {
                    std::unordered_map<std::string, size_t> idx;
                    for (size_t i = 0; i < bithumb.size(); ++i) idx[bithumb[i].base] = i;
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
                                std::string_view p = b["price"].get_string().value();
                                bithumb[it->second].bid = opt::fast_stod(p);
                                break;
                            }
                        }
                        auto asks_arr = item["asks"];
                        if (!asks_arr.error()) {
                            for (auto a : asks_arr.get_array()) {
                                std::string_view p = a["price"].get_string().value();
                                bithumb[it->second].ask = opt::fast_stod(p);
                                break;
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        client.shutdown();
        return true;
    }

    bool fetch_bybit(net::io_context& ioc) {
        RestClient client(ioc, "api.bybit.com");
        if (!client.initialize()) return false;

        auto resp = client.get("/v5/market/tickers?category=linear");
        if (!resp.success) { client.shutdown(); return false; }

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
                c.funding_interval_hours = 8;
                if (c.last > 0 && c.bid > 0 && c.ask > 0) bybit.push_back(std::move(c));
            }
        } catch (...) { client.shutdown(); return false; }

        client.shutdown();
        return true;
    }
};

int main() {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    Logger::init("test_price", "warn");

    constexpr int INTERVAL_SEC = 10;
    constexpr int DURATION_MIN = 10;
    constexpr int TOTAL_ROUNDS = (DURATION_MIN * 60) / INTERVAL_SEC;

    std::cout << fmt::format("\n=== 가격 검증 테스트 ({}분, {}초 간격, {} 라운드) ===\n",
                              DURATION_MIN, INTERVAL_SEC, TOTAL_ROUNDS) << std::endl;

    // Focus coins to track
    std::vector<std::string> focus = {"BTC", "ETH", "XRP", "SOL", "DOGE", "THETA", "LINK", "AVAX", "ADA", "NEAR"};

    for (int round = 1; round <= TOTAL_ROUNDS && !g_stop; ++round) {
        auto start = std::chrono::steady_clock::now();

        net::io_context ioc;
        FetchResult data;

        if (!data.fetch_all(ioc)) {
            std::cerr << fmt::format("[Round {}] Fetch failed, retrying...\n", round);
            std::this_thread::sleep_for(std::chrono::seconds(INTERVAL_SEC));
            continue;
        }

        // Build bybit lookup
        std::unordered_map<std::string, const BybitCoin*> bybit_map;
        for (const auto& c : data.bybit) bybit_map[c.base] = &c;

        // Build bithumb lookup
        std::unordered_map<std::string, const BithumbCoin*> bithumb_map;
        for (const auto& c : data.bithumb) bithumb_map[c.base] = &c;

        // Print header
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&tt, &tm);
        char timebuf[32];
        std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

        std::cout << fmt::format("\n[Round {}/{}  {}  USDT: {:.2f} KRW]\n",
                                  round, TOTAL_ROUNDS, timebuf, data.usdt_krw);
        std::cout << fmt::format("{:<8} {:>12} {:>12} {:>12} {:>12} {:>10} {:>10} {:>8}\n",
                                  "Coin", "KR_bid", "KR_ask", "BY_bid", "BY_ask",
                                  "Entry_PM", "Exit_PM", "Spread");
        std::cout << std::string(96, '-') << "\n";

        // Collect all matched coins with premiums
        struct Row {
            std::string base;
            double kr_bid, kr_ask, by_bid, by_ask;
            double entry_pm, exit_pm, spread;
        };
        std::vector<Row> rows;

        for (const auto& bc : data.bithumb) {
            auto it = bybit_map.find(bc.base);
            if (it == bybit_map.end()) continue;
            const auto& by = *it->second;

            double entry_pm = 0, exit_pm = 0;
            if (by.bid > 0 && data.usdt_krw > 0) {
                double fk = by.bid * data.usdt_krw;
                entry_pm = ((bc.ask - fk) / fk) * 100.0;
            }
            if (by.ask > 0 && data.usdt_krw > 0 && bc.bid > 0) {
                double fk = by.ask * data.usdt_krw;
                exit_pm = ((bc.bid - fk) / fk) * 100.0;
            }
            double spread = entry_pm - exit_pm;

            rows.push_back({bc.base, bc.bid, bc.ask, by.bid, by.ask,
                            entry_pm, exit_pm, spread});
        }

        // Sort by entry premium ascending
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.entry_pm < b.entry_pm; });

        // Print focus coins first (find them in sorted list)
        int printed = 0;
        // Print top 5 lowest entry premium
        std::cout << "  [Top 5 lowest entry premium]\n";
        for (int i = 0; i < std::min(5, (int)rows.size()); ++i) {
            const auto& r = rows[i];
            std::cout << fmt::format("{:<8} {:>12.0f} {:>12.0f} {:>12.6f} {:>12.6f} {:>9.4f}% {:>9.4f}% {:>7.4f}%\n",
                                      r.base, r.kr_bid, r.kr_ask, r.by_bid, r.by_ask,
                                      r.entry_pm, r.exit_pm, r.spread);
        }

        // Print focus coins
        std::cout << "  [Focus coins]\n";
        for (const auto& coin : focus) {
            for (const auto& r : rows) {
                if (r.base == coin) {
                    std::cout << fmt::format("{:<8} {:>12.0f} {:>12.0f} {:>12.6f} {:>12.6f} {:>9.4f}% {:>9.4f}% {:>7.4f}%\n",
                                              r.base, r.kr_bid, r.kr_ask, r.by_bid, r.by_ask,
                                              r.entry_pm, r.exit_pm, r.spread);
                    break;
                }
            }
        }

        // Summary stats
        double avg_spread = 0;
        for (const auto& r : rows) avg_spread += r.spread;
        if (!rows.empty()) avg_spread /= rows.size();

        int entry_candidates = 0, exit_candidates = 0;
        for (const auto& r : rows) {
            if (r.entry_pm <= TradingConfig::ENTRY_PREMIUM_THRESHOLD) ++entry_candidates;
            if (r.exit_pm >= TradingConfig::EXIT_PREMIUM_THRESHOLD) ++exit_candidates;
        }

        std::cout << fmt::format("\n  Matched: {} coins | Avg spread: {:.4f}% | "
                                  "Entry candidates (entry_pm<={:.2f}%): {} | "
                                  "Exit candidates (exit_pm>={:.2f}%): {}\n",
                                  rows.size(), avg_spread,
                                  TradingConfig::ENTRY_PREMIUM_THRESHOLD, entry_candidates,
                                  TradingConfig::EXIT_PREMIUM_THRESHOLD, exit_candidates);

        // Sleep remaining time
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto remaining = std::chrono::seconds(INTERVAL_SEC) - elapsed;
        if (remaining > std::chrono::milliseconds(0) && !g_stop) {
            std::this_thread::sleep_for(remaining);
        }
    }

    std::cout << "\n=== 테스트 완료 ===\n" << std::endl;
    Logger::shutdown();
    return 0;
}
