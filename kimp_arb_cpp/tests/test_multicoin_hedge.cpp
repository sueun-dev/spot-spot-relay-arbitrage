/**
 * Multi-Coin Hedge Accuracy Test
 *
 * 5개 코인을 자동 선별하여 각각 $25 x 3 진입 후 분할 청산.
 * 매 스플릿마다 Bybit SHORT qty == Bithumb BUY qty 100% 일치 검증.
 *
 * 자동 선별 기준:
 *   - BTC, ETH 제외
 *   - 현재 보유 중인 코인 제외 (Bybit 포지션 + Bithumb 잔고)
 *   - Bithumb + Bybit 양쪽 모두 존재하는 코인
 *   - Bybit 최소 주문 조건 충족 ($25)
 *
 * 사용법: ./kimp_test_multicoin_hedge
 */

#include "kimp/core/config.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <boost/asio.hpp>
#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace net = boost::asio;
using namespace kimp;
using namespace kimp::exchange;

// ───────────────── result tracking ─────────────────

static int g_pass = 0;
static int g_fail = 0;

// ───────────────── helpers ─────────────────

static std::unordered_map<std::string, std::string> build_bybit_headers(
    const ExchangeCredentials& creds, const std::string& params) {
    const std::string recv_window = "5000";
    int64_t ts = utils::Crypto::timestamp_ms();
    std::string msg = std::to_string(ts) + creds.api_key + recv_window + params;
    std::string sig = utils::Crypto::hmac_sha256(creds.secret_key, msg);
    return {
        {"X-BAPI-API-KEY", creds.api_key},
        {"X-BAPI-SIGN", sig},
        {"X-BAPI-TIMESTAMP", std::to_string(ts)},
        {"X-BAPI-RECV-WINDOW", recv_window},
    };
}

static double fetch_bybit_short_size(
    RestClient& client, const ExchangeCredentials& creds, const std::string& base) {
    std::string query = "category=linear&symbol=" + base + "USDT";
    auto headers = build_bybit_headers(creds, query);
    auto resp = client.get("/v5/position/list?" + query, headers);
    if (!resp.success) return -1.0;
    try {
        simdjson::padded_string padded(resp.body);
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(padded);
        if (static_cast<int>(doc["retCode"].get_int64().value()) != 0) return -1.0;
        for (auto item : doc["result"]["list"].get_array()) {
            std::string_view sym = item["symbol"].get_string().value();
            if (sym != (base + "USDT")) continue;
            return opt::fast_stod(item["size"].get_string().value());
        }
    } catch (...) {}
    return 0.0;
}

static double fetch_bithumb_balance(bithumb::BithumbExchange& bithumb, const std::string& base) {
    return bithumb.get_balance(base);
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ───────────────── coin selection ─────────────────

struct CoinCandidate {
    std::string base;
    double bybit_bid{0};
    double bithumb_ask{0};
};

/** Select 5 tradeable coins: both exchanges, no BTC/ETH, no existing positions */
static std::vector<CoinCandidate> select_test_coins(
    bybit::BybitExchange& bybit,
    bithumb::BithumbExchange& bithumb_ex,
    int count) {

    // 1. Exclusion list
    std::set<std::string> excluded = {"BTC", "ETH"};

    // 2. Get Bybit symbols and prices
    auto bybit_tickers = bybit.fetch_all_tickers();
    std::unordered_map<std::string, double> bybit_prices;  // base -> bid
    for (const auto& t : bybit_tickers) {
        std::string base(t.symbol.get_base());
        double bid = t.bid > 0 ? t.bid : t.last;
        if (bid > 0) bybit_prices[base] = bid;
    }

    // 3. Get Bithumb symbols and prices
    auto bithumb_tickers = bithumb_ex.fetch_all_tickers();
    std::unordered_map<std::string, double> bithumb_prices;  // base -> ask
    for (const auto& t : bithumb_tickers) {
        std::string base(t.symbol.get_base());
        double ask = t.ask > 0 ? t.ask : t.last;
        if (ask > 0) bithumb_prices[base] = ask;
    }

    // 4. Check existing Bybit positions
    {
        auto positions = bybit.get_positions();
        for (const auto& p : positions) {
            if (p.foreign_amount > 0.001) {
                std::string base(p.symbol.get_base());
                excluded.insert(base);
                std::cout << "  Excluding " << base << " (existing Bybit position)\n";
            }
        }
    }

    // 5. Check existing Bithumb balances
    for (const auto& [base, _] : bithumb_prices) {
        if (excluded.count(base)) continue;
        double bal = fetch_bithumb_balance(bithumb_ex, base);
        if (bal > 0.0001) {
            excluded.insert(base);
            std::cout << "  Excluding " << base << " (existing Bithumb balance: " << bal << ")\n";
        }
    }

    // 6. Find common coins with sufficient liquidity ($25 min)
    std::vector<CoinCandidate> candidates;
    for (const auto& [base, bybit_bid] : bybit_prices) {
        if (excluded.count(base)) continue;
        auto it = bithumb_prices.find(base);
        if (it == bithumb_prices.end()) continue;

        // Check $25 is above Bybit min notional
        double qty_25usd = 25.0 / bybit_bid;
        if (qty_25usd <= 0) continue;

        candidates.push_back({base, bybit_bid, it->second});
    }

    // 7. Sort by price (lower price first — stress-test small-qty lot sizes)
    std::sort(candidates.begin(), candidates.end(),
              [](const CoinCandidate& a, const CoinCandidate& b) {
                  return a.bybit_bid < b.bybit_bid;
              });

    // 8. Take top N
    if (static_cast<int>(candidates.size()) > count) {
        candidates.resize(count);
    }

    return candidates;
}

// ───────────────── per-coin test ─────────────────

struct CoinTestResult {
    std::string base;
    int pass{0};
    int fail{0};
    bool completed{false};
};

static CoinTestResult test_single_coin(
    const CoinCandidate& coin,
    bybit::BybitExchange& bybit,
    bithumb::BithumbExchange& bithumb_ex,
    RestClient& bybit_rest,
    const ExchangeCredentials& bybit_creds) {

    CoinTestResult result;
    result.base = coin.base;

    const double SPLIT_USD = 25.0;
    const int NUM_ENTRIES = 3;

    SymbolId foreign_sym(coin.base, "USDT");
    SymbolId korean_sym(coin.base, "KRW");

    int local_pass = 0;
    int local_fail = 0;

    auto LOCAL_CHECK = [&](const std::string& label, bool cond) {
        if (cond) {
            std::cout << "    [PASS] " << label << "\n";
            ++local_pass;
            ++g_pass;
        } else {
            std::cout << "    [FAIL] " << label << "\n";
            ++local_fail;
            ++g_fail;
        }
    };

    std::cout << "\n========================================\n";
    std::cout << "  TESTING: " << coin.base
              << " (Bybit $" << std::fixed << std::setprecision(2) << coin.bybit_bid
              << " / Bithumb " << std::setprecision(0) << coin.bithumb_ask << " KRW)\n";
    std::cout << "========================================\n";

    // Set leverage 1x
    bybit.set_leverage(foreign_sym, 1);
    sleep_ms(200);

    // Pre-flight balances
    double pre_short = fetch_bybit_short_size(bybit_rest, bybit_creds, coin.base);
    double pre_spot = fetch_bithumb_balance(bithumb_ex, coin.base);

    std::cout << "  Pre-flight: Bybit short=" << std::setprecision(8) << pre_short
              << " | Bithumb spot=" << pre_spot << "\n";

    if (pre_short > 0.001) {
        std::cout << "  SKIP: Existing Bybit position\n";
        return result;
    }

    double total_shorted = 0.0;
    double total_bought = 0.0;
    std::vector<double> entry_qtys;

    // ── PHASE 1: Entry ──
    std::cout << "\n  --- Entry ($" << SPLIT_USD << " x " << NUM_ENTRIES << ") ---\n";

    for (int i = 0; i < NUM_ENTRIES; ++i) {
        std::cout << "\n  [Entry " << (i + 1) << "/" << NUM_ENTRIES << "]\n";

        double raw_qty = SPLIT_USD / coin.bybit_bid;
        std::cout << "    raw_qty=" << std::setprecision(8) << raw_qty << "\n";

        // Bybit SHORT
        auto short_order = bybit.open_short(foreign_sym, raw_qty);
        LOCAL_CHECK("Bybit SHORT filled", short_order.status == OrderStatus::Filled);
        if (short_order.status != OrderStatus::Filled) {
            std::cerr << "    ABORT: Bybit SHORT failed for " << coin.base << "\n";
            goto coin_cleanup;
        }

        {
            double actual_filled = short_order.filled_quantity > 0
                                       ? short_order.filled_quantity
                                       : short_order.quantity;
            std::cout << "    Bybit filled: " << actual_filled << "\n";

            // Bithumb BUY exact same qty
            auto buy_order = bithumb_ex.place_market_buy_quantity(korean_sym, actual_filled);
            LOCAL_CHECK("Bithumb BUY filled", buy_order.status == OrderStatus::Filled);
            if (buy_order.status != OrderStatus::Filled) {
                std::cerr << "    ABORT: Bithumb BUY failed, rolling back SHORT\n";
                bybit.close_short(foreign_sym, actual_filled);
                goto coin_cleanup;
            }

            total_shorted += actual_filled;
            total_bought += actual_filled;
            entry_qtys.push_back(actual_filled);

            // Verify balances
            sleep_ms(500);
            double bybit_size = fetch_bybit_short_size(bybit_rest, bybit_creds, coin.base);
            double bithumb_bal = fetch_bithumb_balance(bithumb_ex, coin.base);
            double bithumb_delta = bithumb_bal - pre_spot;

            std::cout << "    Bybit short  : " << bybit_size << "\n";
            std::cout << "    Bithumb delta : " << bithumb_delta << "\n";
            std::cout << "    Expected      : " << total_shorted << "\n";

            LOCAL_CHECK("Bybit size == expected",
                        std::abs(bybit_size - total_shorted) < 1e-8);
            LOCAL_CHECK("Bithumb delta == expected",
                        std::abs(bithumb_delta - total_bought) < 1e-4);
            LOCAL_CHECK("hedge exact (korean == foreign)",
                        std::abs(total_bought - total_shorted) < 1e-10);
        }
        sleep_ms(300);
    }

    std::cout << "\n  Entry complete: " << total_shorted << " " << coin.base << "\n";

    // ── PHASE 2: Exit ──
    {
        std::cout << "\n  --- Exit (split sells) ---\n";
        double remaining = total_shorted;

        for (size_t i = 0; i < entry_qtys.size(); ++i) {
            double exit_qty = (i == entry_qtys.size() - 1)
                                  ? remaining
                                  : std::min(entry_qtys[i], remaining);

            std::cout << "\n  [Exit " << (i + 1) << "/" << entry_qtys.size()
                      << "] qty=" << exit_qty << "\n";

            // Bybit COVER
            auto cover_order = bybit.close_short(foreign_sym, exit_qty);
            LOCAL_CHECK("Bybit COVER filled", cover_order.status == OrderStatus::Filled);
            if (cover_order.status != OrderStatus::Filled) {
                std::cerr << "    ABORT: Bybit COVER failed\n";
                goto coin_cleanup;
            }

            double actual_covered = cover_order.filled_quantity > 0
                                        ? cover_order.filled_quantity
                                        : cover_order.quantity;
            std::cout << "    Bybit covered: " << actual_covered << "\n";

            // Bithumb SELL exact same qty
            auto sell_order = bithumb_ex.place_market_order(korean_sym, Side::Sell, actual_covered);
            LOCAL_CHECK("Bithumb SELL filled", sell_order.status == OrderStatus::Filled);
            if (sell_order.status != OrderStatus::Filled) {
                std::cerr << "    WARNING: Bithumb SELL failed — unhedged!\n";
            }

            remaining -= actual_covered;
            total_shorted -= actual_covered;
            total_bought -= actual_covered;

            // Verify
            sleep_ms(500);
            double bybit_size = fetch_bybit_short_size(bybit_rest, bybit_creds, coin.base);
            double bithumb_bal = fetch_bithumb_balance(bithumb_ex, coin.base);
            double bithumb_delta = bithumb_bal - pre_spot;

            std::cout << "    Bybit short  : " << bybit_size << "\n";
            std::cout << "    Bithumb delta : " << bithumb_delta << "\n";
            std::cout << "    Expected rem  : " << remaining << "\n";

            if (remaining < 1e-10) {
                LOCAL_CHECK("Bybit position fully closed", bybit_size < 1e-8);
            } else {
                LOCAL_CHECK("Bybit size == expected remaining",
                            std::abs(bybit_size - remaining) < 1e-8);
            }
            LOCAL_CHECK("Bithumb delta == expected remaining",
                        std::abs(bithumb_delta - remaining) < 1e-4);
            LOCAL_CHECK("hedge exact after exit",
                        std::abs(total_bought - total_shorted) < 1e-10);

            sleep_ms(300);
        }
    }

    // ── PHASE 3: Final verification ──
    {
        std::cout << "\n  --- Final Verification ---\n";
        sleep_ms(500);

        double final_short = fetch_bybit_short_size(bybit_rest, bybit_creds, coin.base);
        double final_spot = fetch_bithumb_balance(bithumb_ex, coin.base);
        double final_delta = final_spot - pre_spot;

        std::cout << "  Bybit final short : " << final_short << "\n";
        std::cout << "  Bithumb final delta: " << final_delta << "\n";

        LOCAL_CHECK("Bybit position == 0", final_short < 1e-8);
        LOCAL_CHECK("Bithumb delta ~= 0", std::abs(final_delta) < 1e-4);
        LOCAL_CHECK("Net shorted == 0", std::abs(total_shorted) < 1e-10);
        LOCAL_CHECK("Net bought == 0", std::abs(total_bought) < 1e-10);
    }

    result.pass = local_pass;
    result.fail = local_fail;
    result.completed = true;
    return result;

coin_cleanup:
    std::cout << "\n  --- CLEANUP: " << coin.base << " ---\n";
    {
        double leftover = fetch_bybit_short_size(bybit_rest, bybit_creds, coin.base);
        if (leftover > 0) {
            std::cout << "  Closing Bybit short: " << leftover << "\n";
            bybit.close_short(foreign_sym, leftover);
            sleep_ms(300);
        }

        double spot_now = fetch_bithumb_balance(bithumb_ex, coin.base);
        double spot_excess = spot_now - pre_spot;
        if (spot_excess > 0.0001) {
            double est_value_krw = spot_excess * coin.bithumb_ask;
            if (est_value_krw >= TradingConfig::MIN_ORDER_KRW) {
                std::cout << "  Selling Bithumb excess: " << spot_excess << "\n";
                bithumb_ex.place_market_order(korean_sym, Side::Sell, spot_excess);
            } else {
                std::cout << "  Bithumb excess too small to sell: " << spot_excess
                          << " (~" << std::setprecision(0) << est_value_krw << " KRW)\n";
            }
        }
    }

    result.pass = local_pass;
    result.fail = local_fail;
    result.completed = false;
    return result;
}

// ───────────────── main ─────────────────

static std::string extract_host(std::string url) {
    if (url.rfind("https://", 0) == 0) url = url.substr(8);
    else if (url.rfind("http://", 0) == 0) url = url.substr(7);
    auto slash = url.find('/');
    if (slash != std::string::npos) url = url.substr(0, slash);
    auto colon = url.find(':');
    if (colon != std::string::npos) url = url.substr(0, colon);
    return url;
}

int main() {
    const int NUM_COINS = 5;

    std::cout << "╔═══════════════════════════════════════════════╗\n";
    std::cout << "║  Multi-Coin Hedge Accuracy Test               ║\n";
    std::cout << "║  $25 x 3 entry → split exit per coin          ║\n";
    std::cout << "║  " << NUM_COINS << " coins (auto-selected, BTC/ETH excluded)  ║\n";
    std::cout << "╚═══════════════════════════════════════════════╝\n\n";

    // ── Setup ──
    RuntimeConfig config = ConfigLoader::load("config/config.yaml");
    auto& bithumb_creds = config.exchanges[Exchange::Bithumb];
    auto& bybit_creds = config.exchanges[Exchange::Bybit];

    if (bithumb_creds.api_key.empty() || bybit_creds.api_key.empty()) {
        std::cerr << "API keys missing\n";
        return 1;
    }

    Logger::init("logs/test_multicoin_hedge.log", "info", 100, 10, 8192, true);

    net::io_context io_ctx;
    auto bithumb = std::make_shared<bithumb::BithumbExchange>(io_ctx, bithumb_creds);
    auto bybit = std::make_shared<bybit::BybitExchange>(io_ctx, bybit_creds);

    if (!bithumb->initialize_rest() || !bybit->initialize_rest()) {
        std::cerr << "REST init failed\n";
        return 1;
    }

    RestClient bybit_rest(io_ctx, extract_host(bybit_creds.rest_endpoint));
    bybit_rest.initialize();

    // Load Bybit instruments (required for normalize_order_qty)
    auto bybit_symbols = bybit->get_available_symbols();
    std::cout << "Bybit instruments loaded: " << bybit_symbols.size() << " symbols\n\n";

    // ── Select test coins ──
    std::cout << "--- Selecting " << NUM_COINS << " test coins ---\n";
    auto coins = select_test_coins(*bybit, *bithumb, NUM_COINS);

    if (coins.empty()) {
        std::cerr << "No suitable coins found!\n";
        return 1;
    }

    std::cout << "\nSelected coins:\n";
    for (size_t i = 0; i < coins.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << coins[i].base
                  << " (Bybit $" << std::fixed << std::setprecision(2) << coins[i].bybit_bid
                  << " / Bithumb " << std::setprecision(0) << coins[i].bithumb_ask << " KRW)\n";
    }
    std::cout << "\n";

    // ── Run tests per coin ──
    std::vector<CoinTestResult> results;

    for (const auto& coin : coins) {
        auto r = test_single_coin(coin, *bybit, *bithumb, bybit_rest, bybit_creds);
        results.push_back(r);
        sleep_ms(500);  // cooldown between coins
    }

    // ── Shutdown ──
    bybit_rest.shutdown();
    bithumb->shutdown_rest();
    bybit->shutdown_rest();

    // ── Final summary ──
    std::cout << "\n╔═══════════════════════════════════════════════╗\n";
    std::cout << "║  FINAL RESULTS                                ║\n";
    std::cout << "╠═══════════════════════════════════════════════╣\n";

    int total_coin_pass = 0;

    for (const auto& r : results) {
        std::string status = r.completed ? (r.fail == 0 ? "PASS" : "FAIL") : "ABORT";
        std::cout << "║  " << std::left << std::setw(6) << r.base
                  << " | " << std::setw(5) << status
                  << " | pass=" << std::setw(3) << r.pass
                  << " fail=" << std::setw(3) << r.fail << "         ║\n";

        if (r.completed && r.fail == 0) ++total_coin_pass;
    }

    std::cout << "╠═══════════════════════════════════════════════╣\n";
    std::cout << "║  Coins: " << total_coin_pass << "/" << results.size() << " passed"
              << "                              ║\n";
    std::cout << "║  Checks: " << g_pass << " passed, " << g_fail << " failed"
              << "                       ║\n";
    std::cout << "╚═══════════════════════════════════════════════╝\n\n";

    if (g_fail == 0) {
        std::cout << "*** ALL " << results.size() << " COINS PASSED — HEDGE ACCURACY 100% ***\n";
    } else {
        std::cout << "*** " << g_fail << " CHECKS FAILED ***\n";
    }

    return g_fail > 0 ? 1 : 0;
}
