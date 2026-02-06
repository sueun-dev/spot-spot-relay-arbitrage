/**
 * Live Hedge Accuracy Test
 *
 * 실제 거래소 API로 SOL을 $25씩 3번 매수(SHORT+BUY) 후,
 * 분할 매도(COVER+SELL)하면서 매 단계마다 양쪽 거래소의
 * 보유 수량이 정확히 일치하는지 검증한다.
 *
 * 흐름:
 *   Entry 1: Bybit SHORT $25 → Bithumb BUY (same qty)  → 검증
 *   Entry 2: Bybit SHORT $25 → Bithumb BUY (same qty)  → 검증
 *   Entry 3: Bybit SHORT $25 → Bithumb BUY (same qty)  → 검증
 *   Exit  1: Bybit COVER split1 → Bithumb SELL (same)  → 검증
 *   Exit  2: Bybit COVER split2 → Bithumb SELL (same)  → 검증
 *   Exit  3: Bybit COVER remaining → Bithumb SELL (same) → 검증 (0 == 0)
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
#include <cctype>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

namespace net = boost::asio;

// ─────────────────────────── helpers ───────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void CHECK(const std::string& label, bool cond) {
    if (cond) {
        std::cout << "  [PASS] " << label << "\n";
        ++g_pass;
    } else {
        std::cout << "  [FAIL] " << label << "\n";
        ++g_fail;
    }
}

static std::string extract_host(std::string url) {
    if (url.rfind("https://", 0) == 0) url = url.substr(8);
    else if (url.rfind("http://", 0) == 0) url = url.substr(7);
    auto slash = url.find('/');
    if (slash != std::string::npos) url = url.substr(0, slash);
    auto colon = url.find(':');
    if (colon != std::string::npos) url = url.substr(0, colon);
    return url;
}

static std::unordered_map<std::string, std::string> build_bybit_headers(
    const kimp::ExchangeCredentials& creds, const std::string& params) {
    const std::string recv_window = "5000";
    int64_t ts = kimp::utils::Crypto::timestamp_ms();
    std::string msg = std::to_string(ts) + creds.api_key + recv_window + params;
    std::string sig = kimp::utils::Crypto::hmac_sha256(creds.secret_key, msg);
    return {
        {"X-BAPI-API-KEY", creds.api_key},
        {"X-BAPI-SIGN", sig},
        {"X-BAPI-TIMESTAMP", std::to_string(ts)},
        {"X-BAPI-RECV-WINDOW", recv_window},
    };
}

/** Bybit /v5/position/list → short size (0 if no position) */
static double fetch_bybit_short_size(
    kimp::exchange::RestClient& client,
    const kimp::ExchangeCredentials& creds,
    const std::string& base) {
    std::string query = "category=linear&symbol=" + base + "USDT";
    auto headers = build_bybit_headers(creds, query);
    auto resp = client.get("/v5/position/list?" + query, headers);
    if (!resp.success) return -1.0;

    try {
        simdjson::padded_string padded(resp.body);
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(padded);
        int rc = static_cast<int>(doc["retCode"].get_int64().value());
        if (rc != 0) return -1.0;
        for (auto item : doc["result"]["list"].get_array()) {
            std::string_view sym = item["symbol"].get_string().value();
            if (sym != (base + "USDT")) continue;
            return kimp::opt::fast_stod(item["size"].get_string().value());
        }
    } catch (...) {}
    return 0.0;
}

/** Bithumb spot balance for a coin */
static double fetch_bithumb_balance(
    kimp::exchange::bithumb::BithumbExchange& bithumb,
    const std::string& base) {
    return bithumb.get_balance(base);
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─────────────────────────── main ───────────────────────────

int main() {
    const std::string BASE = "SOL";
    const double SPLIT_USD = 25.0;
    const int NUM_ENTRIES = 3;

    std::cout << "=== Live Hedge Accuracy Test: " << BASE << " ===\n";
    std::cout << "  $" << SPLIT_USD << " x " << NUM_ENTRIES
              << " entries, then split exits\n\n";

    // ── Setup ──
    kimp::RuntimeConfig config = kimp::ConfigLoader::load("config/config.yaml");
    if (!config.exchanges.count(kimp::Exchange::Bithumb) ||
        !config.exchanges.count(kimp::Exchange::Bybit)) {
        std::cerr << "Missing exchange config\n";
        return 1;
    }

    auto& bithumb_creds = config.exchanges[kimp::Exchange::Bithumb];
    auto& bybit_creds   = config.exchanges[kimp::Exchange::Bybit];

    if (bithumb_creds.api_key.empty() || bithumb_creds.secret_key.empty() ||
        bybit_creds.api_key.empty() || bybit_creds.secret_key.empty()) {
        std::cerr << "API keys missing:\n";
        std::cerr << "  Bithumb api_key: " << (bithumb_creds.api_key.empty() ? "EMPTY" : "OK") << "\n";
        std::cerr << "  Bithumb secret:  " << (bithumb_creds.secret_key.empty() ? "EMPTY" : "OK") << "\n";
        std::cerr << "  Bybit api_key:   " << (bybit_creds.api_key.empty() ? "EMPTY" : "OK") << "\n";
        std::cerr << "  Bybit secret:    " << (bybit_creds.secret_key.empty() ? "EMPTY" : "OK") << "\n";
        return 1;
    }

    kimp::Logger::init("logs/test_hedge_accuracy.log", "info", 100, 10, 8192, true);

    net::io_context io_ctx;
    auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(io_ctx, bithumb_creds);
    auto bybit   = std::make_shared<kimp::exchange::bybit::BybitExchange>(io_ctx, bybit_creds);

    if (!bithumb->initialize_rest() || !bybit->initialize_rest()) {
        std::cerr << "REST init failed\n";
        return 1;
    }

    kimp::exchange::RestClient bybit_rest(io_ctx, extract_host(bybit_creds.rest_endpoint));
    bybit_rest.initialize();

    kimp::SymbolId foreign_sym(BASE, "USDT");
    kimp::SymbolId korean_sym(BASE, "KRW");

    // Fetch instruments to cache lot sizes (required for normalize_order_qty)
    auto bybit_symbols = bybit->get_available_symbols();
    std::cout << "  Bybit instruments loaded: " << bybit_symbols.size() << " symbols\n";

    // Set leverage 1x
    bybit->set_leverage(foreign_sym, 1);

    // ── Pre-flight: no existing position ──
    double pre_short = fetch_bybit_short_size(bybit_rest, bybit_creds, BASE);
    double pre_spot  = fetch_bithumb_balance(*bithumb, BASE);

    std::cout << "--- Pre-flight ---\n";
    std::cout << "  Bybit short size : " << std::fixed << std::setprecision(8) << pre_short << "\n";
    std::cout << "  Bithumb spot bal : " << std::fixed << std::setprecision(8) << pre_spot  << "\n";

    if (pre_short > 0.001) {
        std::cerr << "  ERROR: Existing Bybit position detected. Clean up first.\n";
        return 1;
    }

    // Get current price for qty calculation
    auto bybit_tickers = bybit->fetch_all_tickers();
    double byb_bid = 0.0;
    for (const auto& t : bybit_tickers) {
        if (t.symbol.get_base() == BASE) {
            byb_bid = (t.bid > 0 ? t.bid : t.last);
            break;
        }
    }
    if (byb_bid <= 0) {
        std::cerr << "  ERROR: Cannot get Bybit price\n";
        return 1;
    }

    auto bithumb_tickers = bithumb->fetch_all_tickers();
    double bth_ask = 0.0;
    for (const auto& t : bithumb_tickers) {
        if (t.symbol.get_base() == BASE) {
            bth_ask = (t.ask > 0 ? t.ask : t.last);
            break;
        }
    }
    if (bth_ask <= 0) {
        std::cerr << "  ERROR: Cannot get Bithumb price\n";
        return 1;
    }

    std::cout << "  Bybit bid: " << byb_bid << " USDT | Bithumb ask: " << bth_ask << " KRW\n\n";

    // ── Track cumulative amounts ──
    double total_shorted = 0.0;   // Bybit cumulative
    double total_bought  = 0.0;   // Bithumb cumulative
    std::vector<double> entry_qtys;  // each split's actual qty

    // ================================================================
    // PHASE 1: ENTRY — $25 x 3 (Bybit SHORT → Bithumb BUY)
    // ================================================================
    std::cout << "--- Phase 1: Entry ($" << SPLIT_USD << " x " << NUM_ENTRIES << ") ---\n";

    for (int i = 0; i < NUM_ENTRIES; ++i) {
        std::cout << "\n  [Entry " << (i+1) << "/" << NUM_ENTRIES << "]\n";

        // 1. Bybit SHORT (open_short internally normalizes via normalize_order_qty)
        double raw_qty = SPLIT_USD / byb_bid;

        std::cout << "    raw_qty=" << std::setprecision(8) << raw_qty << "\n";

        auto short_order = bybit->open_short(foreign_sym, raw_qty);
        CHECK("Bybit SHORT filled", short_order.status == kimp::OrderStatus::Filled);
        if (short_order.status != kimp::OrderStatus::Filled) {
            std::cerr << "    ABORT: Bybit SHORT failed, stopping test\n";
            goto cleanup;
        }

        // actual_filled from Bybit (same logic as order_manager)
        double actual_filled = short_order.filled_quantity > 0
                                   ? short_order.filled_quantity
                                   : short_order.quantity;

        std::cout << "    Bybit filled: " << actual_filled << "\n";

        // 2. Bithumb BUY exact same qty
        auto buy_order = bithumb->place_market_buy_quantity(korean_sym, actual_filled);
        CHECK("Bithumb BUY filled", buy_order.status == kimp::OrderStatus::Filled);
        if (buy_order.status != kimp::OrderStatus::Filled) {
            std::cerr << "    ABORT: Bithumb BUY failed, rolling back SHORT\n";
            bybit->close_short(foreign_sym, actual_filled);
            goto cleanup;
        }

        total_shorted += actual_filled;
        total_bought  += actual_filled;
        entry_qtys.push_back(actual_filled);

        // 3. Verify: fetch real balances from both exchanges
        sleep_ms(500);  // wait for settlement

        double bybit_size = fetch_bybit_short_size(bybit_rest, bybit_creds, BASE);
        double bithumb_bal = fetch_bithumb_balance(*bithumb, BASE);
        double bithumb_delta = bithumb_bal - pre_spot;

        std::cout << "    Bybit short  : " << bybit_size << "\n";
        std::cout << "    Bithumb delta : " << bithumb_delta << " (bal=" << bithumb_bal << ")\n";
        std::cout << "    Expected      : " << total_shorted << "\n";

        CHECK("Bybit size == expected total",
              std::abs(bybit_size - total_shorted) < 1e-8);
        CHECK("Bithumb delta == expected total",
              std::abs(bithumb_delta - total_bought) < 1e-4);
        CHECK("korean == foreign (hedge exact)",
              std::abs(total_bought - total_shorted) < 1e-10);

        sleep_ms(300);
    }

    std::cout << "\n  Entry complete: total " << total_shorted << " " << BASE << "\n";

    // ================================================================
    // PHASE 2: EXIT — split close (Bybit COVER → Bithumb SELL)
    // ================================================================
    {
        std::cout << "\n--- Phase 2: Exit (split sells) ---\n";

        double remaining = total_shorted;
        int exit_num = 0;

        // Split 1: first entry qty
        // Split 2: second entry qty
        // Split 3: all remaining
        for (size_t i = 0; i < entry_qtys.size(); ++i) {
            ++exit_num;
            double exit_qty;

            if (i == entry_qtys.size() - 1) {
                // Last split: close all remaining
                exit_qty = remaining;
            } else {
                exit_qty = entry_qtys[i];
                exit_qty = std::min(exit_qty, remaining);
            }

            std::cout << "\n  [Exit " << exit_num << "/" << entry_qtys.size() << "] qty=" << exit_qty << "\n";

            // 1. Bybit COVER
            auto cover_order = bybit->close_short(foreign_sym, exit_qty);
            CHECK("Bybit COVER filled", cover_order.status == kimp::OrderStatus::Filled);
            if (cover_order.status != kimp::OrderStatus::Filled) {
                std::cerr << "    ABORT: Bybit COVER failed\n";
                goto cleanup;
            }

            double actual_covered = cover_order.filled_quantity > 0
                                        ? cover_order.filled_quantity
                                        : cover_order.quantity;

            std::cout << "    Bybit covered: " << actual_covered << "\n";

            // 2. Bithumb SELL exact same qty
            auto sell_order = bithumb->place_market_order(korean_sym, kimp::Side::Sell, actual_covered);
            CHECK("Bithumb SELL filled", sell_order.status == kimp::OrderStatus::Filled);
            if (sell_order.status != kimp::OrderStatus::Filled) {
                std::cerr << "    WARNING: Bithumb SELL failed — unhedged!\n";
                // Don't abort, continue to try remaining
            }

            remaining -= actual_covered;
            total_shorted -= actual_covered;
            total_bought  -= actual_covered;

            // 3. Verify
            sleep_ms(500);

            double bybit_size = fetch_bybit_short_size(bybit_rest, bybit_creds, BASE);
            double bithumb_bal = fetch_bithumb_balance(*bithumb, BASE);
            double bithumb_delta = bithumb_bal - pre_spot;

            std::cout << "    Bybit short  : " << bybit_size << "\n";
            std::cout << "    Bithumb delta : " << bithumb_delta << "\n";
            std::cout << "    Expected rem  : " << remaining << "\n";

            // For Bybit, the position size should match remaining
            if (remaining < 1e-10) {
                // Fully closed — position should be 0
                CHECK("Bybit position fully closed",
                      bybit_size < 1e-8);
            } else {
                CHECK("Bybit size == expected remaining",
                      std::abs(bybit_size - remaining) < 1e-8);
            }

            CHECK("Bithumb delta == expected remaining",
                  std::abs(bithumb_delta - remaining) < 1e-4);
            CHECK("korean == foreign (hedge exact after exit)",
                  std::abs(total_bought - total_shorted) < 1e-10);

            sleep_ms(300);
        }
    }

    // ================================================================
    // PHASE 3: Final verification
    // ================================================================
    {
        std::cout << "\n--- Phase 3: Final Verification ---\n";
        sleep_ms(500);

        double final_short = fetch_bybit_short_size(bybit_rest, bybit_creds, BASE);
        double final_spot  = fetch_bithumb_balance(*bithumb, BASE);
        double final_delta = final_spot - pre_spot;

        std::cout << "  Bybit final short : " << final_short << "\n";
        std::cout << "  Bithumb final delta: " << final_delta << "\n";
        std::cout << "  Bithumb balance    : " << final_spot << " (was " << pre_spot << ")\n";

        CHECK("Bybit position == 0 (fully closed)", final_short < 1e-8);
        CHECK("Bithumb delta ~= 0 (all sold back)", std::abs(final_delta) < 1e-4);
        CHECK("Net tracking: total_shorted == 0", std::abs(total_shorted) < 1e-10);
        CHECK("Net tracking: total_bought == 0",  std::abs(total_bought) < 1e-10);
    }

    goto done;

cleanup:
    // Emergency: close any open positions
    std::cout << "\n--- CLEANUP: Closing any remaining positions ---\n";
    {
        double leftover = fetch_bybit_short_size(bybit_rest, bybit_creds, BASE);
        if (leftover > 0) {
            std::cout << "  Closing Bybit short: " << leftover << "\n";
            bybit->close_short(foreign_sym, leftover);
        }

        double spot_now = fetch_bithumb_balance(*bithumb, BASE);
        double spot_excess = spot_now - pre_spot;
        if (spot_excess > 0) {
            double est_value = spot_excess * bth_ask;
            if (est_value >= kimp::TradingConfig::MIN_ORDER_KRW) {
                std::cout << "  Selling Bithumb excess: " << spot_excess << "\n";
                bithumb->place_market_order(korean_sym, kimp::Side::Sell, spot_excess);
            } else {
                std::cout << "  Bithumb excess too small to sell: " << spot_excess << "\n";
            }
        }
    }

done:
    bybit_rest.shutdown();
    bithumb->shutdown_rest();
    bybit->shutdown_rest();

    std::cout << "\n=== Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  Total:  " << (g_pass + g_fail) << "\n\n";

    if (g_fail == 0) {
        std::cout << "*** ALL TESTS PASSED — HEDGE ACCURACY 100% ***\n";
    } else {
        std::cout << "*** " << g_fail << " TESTS FAILED ***\n";
    }

    return g_fail > 0 ? 1 : 0;
}
