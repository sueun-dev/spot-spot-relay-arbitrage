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
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace net = boost::asio;

namespace {
struct BybitPositionInfo {
    double size{0.0};
    double leverage{0.0};
    std::string side;
};

struct BybitLotSizeInfo {
    double min_qty{0.0};
    double qty_step{1.0};
    double min_notional{0.0};
};

std::string to_upper(std::string value) {
    for (auto &c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string extract_host(std::string url) {
    if (url.rfind("https://", 0) == 0) {
        url = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        url = url.substr(7);
    }

    auto slash = url.find('/');
    if (slash != std::string::npos) {
        url = url.substr(0, slash);
    }

    auto colon = url.find(':');
    if (colon != std::string::npos) {
        url = url.substr(0, colon);
    }

    return url;
}

std::unordered_map<std::string, std::string> build_bybit_headers(
    const kimp::ExchangeCredentials &creds,
    const std::string &params) {

    const std::string recv_window = "5000";
    int64_t timestamp = kimp::utils::Crypto::timestamp_ms();
    std::string message = std::to_string(timestamp) + creds.api_key + recv_window + params;
    std::string signature = kimp::utils::Crypto::hmac_sha256(creds.secret_key, message);

    return {
        {"X-BAPI-API-KEY", creds.api_key},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", std::to_string(timestamp)},
        {"X-BAPI-RECV-WINDOW", recv_window},
    };
}

std::optional<BybitPositionInfo> fetch_bybit_position(
    kimp::exchange::RestClient &client,
    const kimp::ExchangeCredentials &creds,
    const std::string &base) {

    std::string query = "category=linear&symbol=" + base + "USDT";
    std::string endpoint = "/v5/position/list?" + query;
    auto headers = build_bybit_headers(creds, query);
    auto response = client.get(endpoint, headers);

    if (!response.success) {
        kimp::Logger::error("[DIAG] Bybit position fetch failed: {}", response.error);
        return std::nullopt;
    }

    try {
        simdjson::padded_string padded(response.body);
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(padded);
        auto ret_code = doc["retCode"];
        if (!ret_code.error()) {
            int rc = static_cast<int>(ret_code.get_int64().value());
            if (rc != 0) {
                std::string_view msg = doc["retMsg"].get_string().value();
                kimp::Logger::error("[DIAG] Bybit position API error: {}", msg);
                return std::nullopt;
            }
        }

        auto result = doc["result"];
        if (result.error()) {
            return std::nullopt;
        }
        auto list = result["list"].get_array();

        for (auto item : list) {
            std::string_view symbol_view = item["symbol"].get_string().value();
            if (symbol_view != (base + "USDT")) {
                continue;
            }

            BybitPositionInfo info;
            std::string_view size_str = item["size"].get_string().value();
            info.size = kimp::opt::fast_stod(size_str);

            std::string_view lev_str = item["leverage"].get_string().value();
            info.leverage = kimp::opt::fast_stod(lev_str);

            std::string_view side_str = item["side"].get_string().value();
            info.side = std::string(side_str);
            return info;
        }
    } catch (const simdjson::simdjson_error &e) {
        kimp::Logger::error("[DIAG] Failed to parse Bybit position: {}", e.what());
    }

    return std::nullopt;
}

std::optional<BybitLotSizeInfo> fetch_bybit_lot_size(
    kimp::exchange::RestClient &client,
    const std::string &base) {

    std::string endpoint = "/v5/market/instruments-info?category=linear&symbol=" + base + "USDT";
    auto response = client.get(endpoint);
    if (!response.success) {
        kimp::Logger::error("[DIAG] Failed to fetch lot size: {}", response.error);
        return std::nullopt;
    }

    try {
        simdjson::padded_string padded(response.body);
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(padded);
        auto ret_code = doc["retCode"];
        if (!ret_code.error()) {
            int rc = static_cast<int>(ret_code.get_int64().value());
            if (rc != 0) {
                std::string_view msg = doc["retMsg"].get_string().value();
                kimp::Logger::error("[DIAG] Bybit instruments API error: {}", msg);
                return std::nullopt;
            }
        }

        auto list = doc["result"]["list"].get_array();
        for (auto item : list) {
            std::string_view symbol_view = item["symbol"].get_string().value();
            if (symbol_view != (base + "USDT")) continue;

            BybitLotSizeInfo info;
            auto lot = item["lotSizeFilter"];
            if (!lot.error()) {
                auto min_qty_res = lot["minOrderQty"];
                if (!min_qty_res.error()) {
                    info.min_qty = kimp::opt::fast_stod(min_qty_res.get_string().value());
                }
                auto step_res = lot["qtyStep"];
                if (!step_res.error()) {
                    info.qty_step = kimp::opt::fast_stod(step_res.get_string().value());
                }
                auto min_amt_res = lot["minOrderAmt"];
                if (!min_amt_res.error()) {
                    info.min_notional = kimp::opt::fast_stod(min_amt_res.get_string().value());
                }
                auto min_val_res = lot["minNotionalValue"];
                if (!min_val_res.error()) {
                    info.min_notional = std::max(info.min_notional,
                                                 kimp::opt::fast_stod(min_val_res.get_string().value()));
                }
            }
            return info;
        }
    } catch (const simdjson::simdjson_error &e) {
        kimp::Logger::error("[DIAG] Failed to parse lot size: {}", e.what());
    }

    return std::nullopt;
}

double round_down_step(double qty, double step) {
    if (step <= 0.0) return qty;
    return std::floor(qty / step) * step;
}

double round_up_step(double qty, double step) {
    if (step <= 0.0) return qty;
    return std::ceil(qty / step) * step;
}

bool find_price(const std::vector<kimp::Ticker> &tickers,
                const std::string &base,
                double &bid,
                double &ask) {
    for (const auto &t : tickers) {
        if (t.symbol.get_base() == base) {
            bid = (t.bid > 0 ? t.bid : t.last);
            ask = (t.ask > 0 ? t.ask : t.last);
            return bid > 0 && ask > 0;
        }
    }
    return false;
}

void usage(const char *prog) {
    std::cout << "Usage: " << prog << " --symbol <BASE> [--notional <USD>] [--config <path>]\n";
}
} // namespace

int main(int argc, char *argv[]) {
    std::string config_path = "config/config.yaml";
    std::string symbol_base;
    double notional_usd = 5.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--symbol" && i + 1 < argc) {
            symbol_base = argv[++i];
        } else if (arg == "--notional" && i + 1 < argc) {
            notional_usd = std::stod(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
    }

    if (symbol_base.empty()) {
        usage(argv[0]);
        return 1;
    }

    symbol_base = to_upper(symbol_base);

    kimp::RuntimeConfig config = kimp::ConfigLoader::load(config_path);

    if (!config.exchanges.count(kimp::Exchange::Bithumb) ||
        !config.exchanges.count(kimp::Exchange::Bybit)) {
        std::cerr << "Missing exchange configuration\n";
        return 1;
    }

    auto &bithumb_creds = config.exchanges[kimp::Exchange::Bithumb];
    auto &bybit_creds = config.exchanges[kimp::Exchange::Bybit];

    if (bithumb_creds.api_key.empty() || bithumb_creds.secret_key.empty() ||
        bybit_creds.api_key.empty() || bybit_creds.secret_key.empty()) {
        std::cerr << "API keys missing in config/env\n";
        return 1;
    }

    kimp::Logger::init("logs/diag_trade.log", "info", 100, 10, 8192, true);
    kimp::Logger::info("[DIAG] Starting diagnostic trade for {}", symbol_base);

    net::io_context io_context;

    auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
        io_context, bithumb_creds);
    auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
        io_context, bybit_creds);

    if (!bithumb->initialize_rest() || !bybit->initialize_rest()) {
        kimp::Logger::error("[DIAG] Failed to initialize REST clients");
        return 1;
    }

    auto bithumb_tickers = bithumb->fetch_all_tickers();
    auto bybit_tickers = bybit->fetch_all_tickers();

    double bth_bid = 0.0;
    double bth_ask = 0.0;
    double byb_bid = 0.0;
    double byb_ask = 0.0;

    if (!find_price(bithumb_tickers, symbol_base, bth_bid, bth_ask)) {
        kimp::Logger::error("[DIAG] Bithumb price not found for {}", symbol_base);
        return 1;
    }
    if (!find_price(bybit_tickers, symbol_base, byb_bid, byb_ask)) {
        kimp::Logger::error("[DIAG] Bybit price not found for {}", symbol_base);
        return 1;
    }

    kimp::Logger::info("[DIAG] Prices: Bithumb ask {:.2f} KRW | Bybit bid {:.6f} USDT",
                       bth_ask, byb_bid);

    double spot_before = bithumb->get_balance(symbol_base);

    kimp::exchange::RestClient bybit_client(io_context, extract_host(bybit_creds.rest_endpoint));
    bybit_client.initialize();

    auto lot_info = fetch_bybit_lot_size(bybit_client, symbol_base);
    if (lot_info) {
        kimp::Logger::info("[DIAG] Bybit lot size: min_qty {:.8f}, step {:.8f}, min_notional {:.4f}",
                           lot_info->min_qty, lot_info->qty_step, lot_info->min_notional);
    }

    auto pos_before = fetch_bybit_position(bybit_client, bybit_creds, symbol_base);
    if (pos_before && pos_before->size > 0.0) {
        kimp::Logger::error("[DIAG] Existing Bybit position detected (size {:.8f})", pos_before->size);
        return 1;
    }
    if (pos_before) {
        kimp::Logger::info("[DIAG] Bybit leverage before entry: {:.2f}x", pos_before->leverage);
    }

    kimp::SymbolId foreign_symbol(symbol_base, "USDT");
    kimp::SymbolId korean_symbol(symbol_base, "KRW");

    if (!bybit->set_leverage(foreign_symbol, 1)) {
        kimp::Logger::warn("[DIAG] Failed to set leverage to 1x (continuing)");
    }

    auto pos_after_set = fetch_bybit_position(bybit_client, bybit_creds, symbol_base);
    if (pos_after_set) {
        kimp::Logger::info("[DIAG] Bybit leverage after set: {:.2f}x", pos_after_set->leverage);
    }

    double qty_target = notional_usd / byb_bid;
    if (qty_target <= 0.0) {
        kimp::Logger::error("[DIAG] Invalid target qty");
        return 1;
    }

    if (lot_info) {
        qty_target = round_down_step(qty_target, lot_info->qty_step);
        if (qty_target < lot_info->min_qty) {
            qty_target = lot_info->min_qty;
        }
        if (lot_info->min_notional > 0.0 && qty_target * byb_bid < lot_info->min_notional) {
            qty_target = round_up_step(lot_info->min_notional / byb_bid, lot_info->qty_step);
        }
    }

    kimp::Logger::info("[DIAG] Opening short {} qty {:.8f} (notional ${:.2f})",
                       symbol_base, qty_target, notional_usd);
    auto short_order = bybit->open_short(foreign_symbol, qty_target);
    if (short_order.status != kimp::OrderStatus::Filled) {
        kimp::Logger::error("[DIAG] Bybit short order failed");
        return 1;
    }

    BybitPositionInfo position{};
    bool have_position = false;
    for (int i = 0; i < 10; ++i) {
        auto pos = fetch_bybit_position(bybit_client, bybit_creds, symbol_base);
        if (pos && pos->size > 0.0) {
            position = *pos;
            have_position = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (!have_position) {
        kimp::Logger::error("[DIAG] Bybit position not confirmed after short");
        return 1;
    }

    kimp::Logger::info("[DIAG] Bybit position size {:.8f}, leverage {:.2f}x",
                       position.size, position.leverage);

    double krw_amount = position.size * bth_ask;
    if (krw_amount < kimp::TradingConfig::MIN_ORDER_KRW) {
        kimp::Logger::error("[DIAG] KRW amount {:.0f} below minimum {}, closing short",
                            krw_amount, kimp::TradingConfig::MIN_ORDER_KRW);
        bybit->close_short(foreign_symbol, position.size);
        return 1;
    }

    kimp::Logger::info("[DIAG] Buying Bithumb spot {} qty {:.8f} (est {:.0f} KRW)",
                       symbol_base, position.size, krw_amount);
    auto buy_order = bithumb->place_market_buy_quantity(korean_symbol, position.size);
    if (buy_order.status != kimp::OrderStatus::Filled) {
        kimp::Logger::error("[DIAG] Bithumb buy failed, closing short");
        bybit->close_short(foreign_symbol, position.size);
        return 1;
    }

    double spot_after = spot_before;
    for (int i = 0; i < 10; ++i) {
        spot_after = bithumb->get_balance(symbol_base);
        if (spot_after > spot_before) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    double spot_delta = spot_after - spot_before;
    kimp::Logger::info("[DIAG] Spot delta {:.8f} (before {:.8f} -> after {:.8f})",
                       spot_delta, spot_before, spot_after);
    if (position.size > 0.0) {
        kimp::Logger::info("[DIAG] Spot/short match {:.4f}%%",
                           (spot_delta / position.size) * 100.0);
    }

    kimp::Logger::info("[DIAG] Holding position for 5 seconds before exit");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    kimp::Logger::info("[DIAG] Closing Bybit short {} qty {:.8f}", symbol_base, position.size);
    auto close_order = bybit->close_short(foreign_symbol, position.size);
    if (close_order.status != kimp::OrderStatus::Filled) {
        kimp::Logger::error("[DIAG] Bybit close failed; spot sell skipped");
        return 1;
    }

    bool closed = false;
    for (int i = 0; i < 10; ++i) {
        auto pos = fetch_bybit_position(bybit_client, bybit_creds, symbol_base);
        if (!pos || pos->size <= 0.0) {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (!closed) {
        kimp::Logger::error("[DIAG] Bybit short still open after close");
        return 1;
    }

    double spot_now = bithumb->get_balance(symbol_base);
    double sell_qty = (spot_delta > 0.0) ? std::min(spot_delta, spot_now) : 0.0;
    if (sell_qty <= 0.0) {
        kimp::Logger::warn("[DIAG] No spot qty to sell");
        return 0;
    }

    double est_sell_value = sell_qty * (bth_bid > 0 ? bth_bid : bth_ask);
    if (est_sell_value < kimp::TradingConfig::MIN_ORDER_KRW) {
        kimp::Logger::warn("[DIAG] Spot sell value {:.0f} below minimum {}, skipping",
                           est_sell_value, kimp::TradingConfig::MIN_ORDER_KRW);
        return 0;
    }

    kimp::Logger::info("[DIAG] Selling Bithumb spot {} qty {:.8f}", symbol_base, sell_qty);
    auto sell_order = bithumb->place_market_order(korean_symbol, kimp::Side::Sell, sell_qty);
    if (sell_order.status != kimp::OrderStatus::Filled) {
        kimp::Logger::error("[DIAG] Bithumb sell failed");
        return 1;
    }

    double spot_end = bithumb->get_balance(symbol_base);
    kimp::Logger::info("[DIAG] Completed; spot balance {:.8f} (start {:.8f})",
                       spot_end, spot_before);

    bybit_client.shutdown();
    bithumb->shutdown_rest();
    bybit->shutdown_rest();

    return 0;
}
