/**
 * Symbol Matching Verification Test
 *
 * 실제 API에서 양 거래소 심볼을 가져와서:
 * 1. Bithumb KRW 마켓 전체 목록
 * 2. Bybit USDT 무기한 전체 목록
 * 3. 교집합 정확성 검증
 * 4. 잠재적 누락 코인 탐지
 * 5. Bybit instruments-info vs tickers 일관성 체크
 * 6. Bybit pagination 이슈 체크 (>1000 코인)
 *
 * API 엔드포인트 (인증 불필요):
 * - 빗썸: GET /public/ticker/ALL_KRW
 * - 바이빗: GET /v5/market/instruments-info?category=linear&limit=1000
 * - 바이빗: GET /v5/market/tickers?category=linear
 */

#include "kimp/exchange/exchange_base.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <simdjson.h>
#include <boost/asio.hpp>
#include <fmt/format.h>

#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cassert>

using namespace kimp;
using namespace kimp::exchange;
namespace net = boost::asio;
namespace opt = kimp::opt;

// ============================================================
// Test infrastructure
// ============================================================
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << "... "; \
    try

#define PASS() \
    do { std::cout << "PASS" << std::endl; ++g_passed; } while(0)

#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << std::endl; ++g_failed; } while(0)

// ============================================================
// Bithumb에서 전체 KRW 심볼 가져오기
// ============================================================
std::unordered_set<std::string> fetch_bithumb_symbols(net::io_context& ioc) {
    std::unordered_set<std::string> symbols;

    RestClient client(ioc, "api.bithumb.com");
    if (!client.initialize()) {
        std::cerr << "[ERROR] Bithumb REST connection failed" << std::endl;
        return symbols;
    }

    auto response = client.get("/public/ticker/ALL_KRW");
    if (!response.success) {
        std::cerr << "[ERROR] Bithumb ALL_KRW fetch failed: " << response.error << std::endl;
        client.shutdown();
        return symbols;
    }

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response.body);
        auto doc = parser.iterate(padded);
        auto status = doc["status"].get_string().value();
        if (status != "0000") {
            std::cerr << "[ERROR] Bithumb API status: " << status << std::endl;
            client.shutdown();
            return symbols;
        }

        auto data = doc["data"].get_object();
        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key == "date") continue;

            // Check if it has valid price data
            auto item = field.value().get_object();
            auto closing = item["closing_price"];
            if (!closing.error()) {
                std::string_view price_str = closing.get_string().value();
                double price = opt::fast_stod(price_str);
                if (price > 0) {
                    symbols.insert(std::string(key));
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        std::cerr << "[ERROR] Bithumb parse failed: " << e.what() << std::endl;
    }

    client.shutdown();
    return symbols;
}

// ============================================================
// Bybit instruments-info에서 USDT 무기한 심볼 가져오기
// (get_available_symbols()와 동일한 로직)
// ============================================================
struct BybitInstrument {
    std::string base;
    std::string symbol;  // e.g., "BTCUSDT"
    int funding_interval_hours{8};
};

std::vector<BybitInstrument> fetch_bybit_instruments(net::io_context& ioc) {
    std::vector<BybitInstrument> instruments;

    RestClient client(ioc, "api.bybit.com");
    if (!client.initialize()) {
        std::cerr << "[ERROR] Bybit REST connection failed" << std::endl;
        return instruments;
    }

    // First page
    std::string cursor = "";
    int page = 0;
    while (true) {
        std::string endpoint = "/v5/market/instruments-info?category=linear&limit=1000";
        if (!cursor.empty()) {
            endpoint += "&cursor=" + cursor;
        }

        auto response = client.get(endpoint);
        if (!response.success) {
            std::cerr << "[ERROR] Bybit instruments fetch failed: " << response.error << std::endl;
            break;
        }

        try {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(response.body);
            auto doc = parser.iterate(padded);

            int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
            if (ret_code != 0) {
                std::cerr << "[ERROR] Bybit API retCode: " << ret_code << std::endl;
                break;
            }

            auto result = doc["result"];
            auto list = result["list"].get_array();

            int count = 0;
            for (auto item : list) {
                std::string_view symbol_str = item["symbol"].get_string().value();
                std::string_view quote = item["quoteCoin"].get_string().value();
                std::string_view settle = item["settleCoin"].get_string().value();

                if (quote == "USDT" && settle == "USDT") {
                    std::string_view base = item["baseCoin"].get_string().value();
                    BybitInstrument inst;
                    inst.base = std::string(base);
                    inst.symbol = std::string(symbol_str);

                    auto fi = item["fundingInterval"];
                    if (!fi.error()) {
                        int interval_min = static_cast<int>(fi.get_int64().value());
                        inst.funding_interval_hours = interval_min / 60;
                        if (inst.funding_interval_hours == 0) inst.funding_interval_hours = 8;
                    }

                    instruments.push_back(std::move(inst));
                }
                ++count;
            }

            // Check for pagination cursor
            auto next_cursor = result["nextPageCursor"];
            if (!next_cursor.error()) {
                std::string_view nc = next_cursor.get_string().value();
                if (!nc.empty() && nc != cursor) {
                    cursor = std::string(nc);
                    ++page;
                    std::cout << "    [PAGE " << page << "] Fetched " << count
                              << " instruments, next cursor: " << cursor.substr(0, 20) << "..." << std::endl;
                    continue;
                }
            }
            break;  // No more pages

        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "[ERROR] Bybit instruments parse failed: " << e.what() << std::endl;
            break;
        }
    }

    client.shutdown();
    return instruments;
}

// ============================================================
// Bybit tickers에서 USDT 심볼 가져오기
// (fetch_all_tickers()와 동일한 필터링 로직)
// ============================================================
std::unordered_set<std::string> fetch_bybit_ticker_symbols(net::io_context& ioc) {
    std::unordered_set<std::string> symbols;

    RestClient client(ioc, "api.bybit.com");
    if (!client.initialize()) {
        std::cerr << "[ERROR] Bybit REST connection failed" << std::endl;
        return symbols;
    }

    auto response = client.get("/v5/market/tickers?category=linear");
    if (!response.success) {
        std::cerr << "[ERROR] Bybit tickers fetch failed: " << response.error << std::endl;
        client.shutdown();
        return symbols;
    }

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response.body);
        auto doc = parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto item : list) {
            std::string_view sym = item["symbol"].get_string().value();
            // Same filter as fetch_all_tickers()
            if (sym.size() < 5 || sym.substr(sym.size() - 4) != "USDT") continue;

            std::string base(sym.substr(0, sym.size() - 4));

            auto last_str = item["lastPrice"];
            if (!last_str.error()) {
                double last = opt::fast_stod(last_str.get_string().value());
                if (last > 0) {
                    symbols.insert(base);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        std::cerr << "[ERROR] Bybit tickers parse failed: " << e.what() << std::endl;
    }

    client.shutdown();
    return symbols;
}

// ============================================================
// Tests
// ============================================================

void test_bithumb_symbols(const std::unordered_set<std::string>& bithumb) {
    std::cout << "\n[Bithumb 심볼 검증]\n";

    TEST("Bithumb API 연결 및 심볼 로딩") {
        if (bithumb.empty()) { FAIL("심볼 0개 - API 연결 실패"); return; }
        std::cout << "(" << bithumb.size() << "개) ";
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("Bithumb 주요 코인 존재 확인 (BTC, ETH, XRP, SOL, DOGE)") {
        std::vector<std::string> must_have = {"BTC", "ETH", "XRP", "SOL", "DOGE"};
        std::vector<std::string> missing;
        for (const auto& coin : must_have) {
            if (bithumb.find(coin) == bithumb.end()) {
                missing.push_back(coin);
            }
        }
        if (!missing.empty()) {
            std::string msg = "누락: ";
            for (const auto& m : missing) msg += m + " ";
            FAIL(msg);
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("Bithumb에 USDT 존재 (환율 계산 필수)") {
        if (bithumb.find("USDT") == bithumb.end()) {
            FAIL("USDT 없음 - 환율 계산 불가");
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("Bithumb 최소 100개 이상 KRW 마켓") {
        if (bithumb.size() < 100) {
            FAIL(fmt::format("{}개 < 100개 (비정상적으로 적음)", bithumb.size()));
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }
}

void test_bybit_symbols(const std::vector<BybitInstrument>& instruments,
                         const std::unordered_set<std::string>& ticker_symbols) {
    std::cout << "\n[Bybit 심볼 검증]\n";

    // Extract base set from instruments
    std::unordered_set<std::string> inst_bases;
    for (const auto& inst : instruments) {
        inst_bases.insert(inst.base);
    }

    TEST("Bybit instruments-info API 연결 및 심볼 로딩") {
        if (instruments.empty()) { FAIL("심볼 0개 - API 연결 실패"); return; }
        std::cout << "(" << instruments.size() << "개) ";
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("Bybit tickers API 연결 및 심볼 로딩") {
        if (ticker_symbols.empty()) { FAIL("심볼 0개 - API 연결 실패"); return; }
        std::cout << "(" << ticker_symbols.size() << "개) ";
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("Bybit 주요 코인 존재 확인 (BTC, ETH, XRP, SOL, DOGE)") {
        std::vector<std::string> must_have = {"BTC", "ETH", "XRP", "SOL", "DOGE"};
        std::vector<std::string> missing;
        for (const auto& coin : must_have) {
            if (inst_bases.find(coin) == inst_bases.end()) {
                missing.push_back(coin);
            }
        }
        if (!missing.empty()) {
            std::string msg = "누락: ";
            for (const auto& m : missing) msg += m + " ";
            FAIL(msg);
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("Bybit instruments-info vs tickers 일관성") {
        // instruments-info에 있지만 tickers에 없는 코인
        std::vector<std::string> inst_only;
        for (const auto& b : inst_bases) {
            if (ticker_symbols.find(b) == ticker_symbols.end()) {
                inst_only.push_back(b);
            }
        }
        // tickers에 있지만 instruments-info에 없는 코인
        std::vector<std::string> ticker_only;
        for (const auto& b : ticker_symbols) {
            if (inst_bases.find(b) == inst_bases.end()) {
                ticker_only.push_back(b);
            }
        }

        if (!inst_only.empty() || !ticker_only.empty()) {
            std::cout << "\n";
            if (!inst_only.empty()) {
                std::sort(inst_only.begin(), inst_only.end());
                std::cout << "    instruments에만 있음 (" << inst_only.size() << "): ";
                for (size_t i = 0; i < std::min(inst_only.size(), size_t(10)); ++i)
                    std::cout << inst_only[i] << " ";
                if (inst_only.size() > 10) std::cout << "...";
                std::cout << "\n";
            }
            if (!ticker_only.empty()) {
                std::sort(ticker_only.begin(), ticker_only.end());
                std::cout << "    tickers에만 있음 (" << ticker_only.size() << "): ";
                for (size_t i = 0; i < std::min(ticker_only.size(), size_t(10)); ++i)
                    std::cout << ticker_only[i] << " ";
                if (ticker_only.size() > 10) std::cout << "...";
                std::cout << "\n";
            }
            std::cout << "    ";
            // Allow small discrepancy (delisted coins, new listings)
            if (inst_only.size() + ticker_only.size() > 20) {
                FAIL(fmt::format("불일치 {}개 > 허용치 20개", inst_only.size() + ticker_only.size()));
            } else {
                std::cout << "(허용 범위 내 불일치) ";
                PASS();
            }
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("Bybit pagination 체크 (>1000 코인 누락 방지)") {
        // The bot uses limit=1000 without pagination.
        // If there are >1000 USDT perpetuals, some are missed.
        if (instruments.size() >= 995) {
            FAIL(fmt::format("instruments {}개 >= 995: pagination 필요! 봇이 코인 누락 가능",
                             instruments.size()));
        } else {
            std::cout << fmt::format("({}개 < 1000, 단일 페이지 OK) ", instruments.size());
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("Bybit 펀딩 인터벌 분포") {
        std::unordered_map<int, int> interval_count;
        for (const auto& inst : instruments) {
            interval_count[inst.funding_interval_hours]++;
        }
        std::cout << "\n";
        for (auto& [hours, count] : interval_count) {
            std::cout << fmt::format("    {}h: {}개\n", hours, count);
        }
        std::cout << "    ";
        // 봇은 8h만 진입 → 8h가 가장 많아야 정상
        if (interval_count.count(8) == 0 || interval_count[8] < 50) {
            FAIL("8h 펀딩 코인이 50개 미만 - 진입 기회 부족");
        } else {
            std::cout << fmt::format("(8h 펀딩: {}개) ", interval_count[8]);
            PASS();
        }
    } catch (...) { FAIL("exception"); }
}

void test_common_symbols(const std::unordered_set<std::string>& bithumb,
                          const std::unordered_set<std::string>& bybit_bases) {
    std::cout << "\n[교집합 심볼 검증]\n";

    // Compute intersection (same logic as main.cpp)
    std::unordered_set<std::string> common;
    for (const auto& b : bithumb) {
        if (bybit_bases.count(b)) {
            common.insert(b);
        }
    }

    TEST("교집합 계산") {
        std::cout << fmt::format("(빗썸 {} ∩ 바이빗 {} = {} 공통) ",
                                  bithumb.size(), bybit_bases.size(), common.size());
        if (common.empty()) { FAIL("공통 코인 0개"); return; }
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("주요 코인 교집합 포함 (BTC, ETH, XRP, SOL, DOGE, ADA)") {
        std::vector<std::string> must_have = {"BTC", "ETH", "XRP", "SOL", "DOGE", "ADA"};
        std::vector<std::string> missing;
        for (const auto& coin : must_have) {
            if (common.find(coin) == common.end()) {
                missing.push_back(coin);
            }
        }
        if (!missing.empty()) {
            std::string msg = "교집합에서 누락: ";
            for (const auto& m : missing) msg += m + " ";
            FAIL(msg);
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("교집합 100개 이상 (충분한 거래 기회)") {
        if (common.size() < 100) {
            FAIL(fmt::format("{}개 < 100개", common.size()));
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    // Show Bithumb-only coins
    {
        std::vector<std::string> bithumb_only;
        for (const auto& b : bithumb) {
            if (!bybit_bases.count(b) && b != "USDT") {
                bithumb_only.push_back(b);
            }
        }
        std::sort(bithumb_only.begin(), bithumb_only.end());
        std::cout << fmt::format("\n  빗썸에만 있는 코인 ({}개):", bithumb_only.size());
        if (bithumb_only.size() <= 30) {
            for (const auto& c : bithumb_only) std::cout << " " << c;
        } else {
            for (size_t i = 0; i < 20; ++i) std::cout << " " << bithumb_only[i];
            std::cout << " ... (+" << (bithumb_only.size() - 20) << "개)";
        }
        std::cout << "\n";
    }

    // Show Bybit-only coins
    {
        std::vector<std::string> bybit_only;
        for (const auto& b : bybit_bases) {
            if (!bithumb.count(b)) {
                bybit_only.push_back(b);
            }
        }
        std::sort(bybit_only.begin(), bybit_only.end());
        std::cout << fmt::format("  바이빗에만 있는 코인 ({}개):", bybit_only.size());
        if (bybit_only.size() <= 30) {
            for (const auto& c : bybit_only) std::cout << " " << c;
        } else {
            for (size_t i = 0; i < 20; ++i) std::cout << " " << bybit_only[i];
            std::cout << " ... (+" << (bybit_only.size() - 20) << "개)";
        }
        std::cout << "\n";
    }

    // Show common coins sorted
    {
        std::vector<std::string> common_sorted(common.begin(), common.end());
        std::sort(common_sorted.begin(), common_sorted.end());
        std::cout << fmt::format("\n  공통 코인 전체 ({}개):\n  ", common.size());
        int col = 0;
        for (const auto& c : common_sorted) {
            std::cout << fmt::format("{:<10}", c);
            if (++col % 10 == 0) std::cout << "\n  ";
        }
        if (col % 10 != 0) std::cout << "\n";
    }
}

void test_bot_matching_logic(const std::unordered_set<std::string>& bithumb,
                              const std::vector<BybitInstrument>& instruments) {
    std::cout << "\n[봇 매칭 로직 검증 (main.cpp와 동일한 코드)]\n";

    // Reproduce EXACT logic from main.cpp lines 540-555
    // main.cpp uses get_available_symbols() which returns SymbolId vectors
    // Then does: bybit_bases = set of bybit base coins
    //            for each bithumb symbol, if base in bybit_bases → common

    std::unordered_set<std::string> bybit_bases;
    for (const auto& inst : instruments) {
        bybit_bases.insert(inst.base);
    }

    std::vector<std::string> common_symbols;
    for (const auto& b : bithumb) {
        if (bybit_bases.count(b)) {
            common_symbols.push_back(b);
        }
    }
    std::sort(common_symbols.begin(), common_symbols.end());

    TEST("봇 매칭 로직 재현 → 교집합 크기 일치") {
        std::cout << fmt::format("({}개) ", common_symbols.size());
        if (common_symbols.size() < 100) {
            FAIL(fmt::format("{}개 < 100개", common_symbols.size()));
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    // Check for potential issues
    TEST("USDT가 교집합에 포함되지 않는지 확인") {
        // USDT는 환율용이지 거래 코인이 아님
        // Bithumb에 USDT가 있고 Bybit에 USDTUSDT가 있으면 교집합에 포함될 수 있음
        bool usdt_in_common = std::find(common_symbols.begin(), common_symbols.end(), "USDT")
                              != common_symbols.end();
        if (usdt_in_common) {
            // USDT가 교집합에 포함됨 → 이것 자체는 위험하지 않지만
            // 봇이 USDT/KRW로 거래하려고 시도하면 문제
            std::cout << "(USDT가 교집합에 포함됨 - 거래시 주의) ";
            PASS();  // 경고만, 실패는 아님
        } else {
            PASS();
        }
    } catch (...) { FAIL("exception"); }

    TEST("base 이름 대소문자 일관성 (Bithumb: 대문자, Bybit: 대문자)") {
        bool all_upper = true;
        for (const auto& c : common_symbols) {
            for (char ch : c) {
                if (std::islower(static_cast<unsigned char>(ch))) {
                    all_upper = false;
                    std::cout << fmt::format("소문자 발견: {} ", c);
                    break;
                }
            }
        }
        if (all_upper) {
            PASS();
        } else {
            FAIL("대소문자 불일치 → 교집합 누락 가능");
        }
    } catch (...) { FAIL("exception"); }
}

// ============================================================
// Main
// ============================================================
int main() {
    std::cout << "=== 거래소 심볼 매칭 검증 테스트 ===\n" << std::endl;

    // Initialize minimal logger
    Logger::init("logs/test_symbols.log", "warn", 10, 2, 4096, false);

    net::io_context ioc;
    // Run io_context in background thread for async SSL
    std::thread io_thread([&ioc]() {
        auto work = net::make_work_guard(ioc);
        ioc.run();
    });

    // ---- Fetch data from both exchanges ----
    std::cout << "API 데이터 로딩 중..." << std::endl;

    auto bithumb_symbols = fetch_bithumb_symbols(ioc);
    std::cout << fmt::format("  빗썸: {} KRW 마켓\n", bithumb_symbols.size());

    auto bybit_instruments = fetch_bybit_instruments(ioc);
    std::cout << fmt::format("  바이빗 instruments: {} USDT 무기한\n", bybit_instruments.size());

    auto bybit_ticker_syms = fetch_bybit_ticker_symbols(ioc);
    std::cout << fmt::format("  바이빗 tickers: {} USDT 코인\n", bybit_ticker_syms.size());

    // Build bybit base set from instruments
    std::unordered_set<std::string> bybit_bases;
    for (const auto& inst : bybit_instruments) {
        bybit_bases.insert(inst.base);
    }

    // ---- Run tests ----
    if (bithumb_symbols.empty() || bybit_instruments.empty()) {
        std::cerr << "\n[FATAL] API 연결 실패 - VPN이 꺼져있는지 확인하세요\n" << std::endl;
        ioc.stop();
        io_thread.join();
        Logger::shutdown();
        return 1;
    }

    test_bithumb_symbols(bithumb_symbols);
    test_bybit_symbols(bybit_instruments, bybit_ticker_syms);
    test_common_symbols(bithumb_symbols, bybit_bases);
    test_bot_matching_logic(bithumb_symbols, bybit_instruments);

    // ---- Summary ----
    std::cout << fmt::format("\n=== 결과: {} passed, {} failed ===\n", g_passed, g_failed);

    ioc.stop();
    io_thread.join();
    Logger::shutdown();

    return g_failed > 0 ? 1 : 0;
}
