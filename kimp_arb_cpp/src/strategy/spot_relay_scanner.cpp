#include "kimp/strategy/spot_relay_scanner.hpp"

#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/exchange/exchange_base.hpp"
#include "kimp/utils/crypto.hpp"

#include <simdjson.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace kimp::strategy {

namespace {

using exchange::HttpResponse;
using exchange::RestClient;

struct BithumbQuote {
    double bid{0.0};
    double ask{0.0};
};

struct BithumbAssetStatus {
    bool deposit_enabled{false};
    bool withdraw_enabled{false};
};

struct BybitSpotInstrument {
    std::string symbol;
    std::string base;
    std::string quote;
    bool trading{false};
    bool margin_enabled{false};
    std::string margin_mode;
};

struct BybitSpotQuote {
    double bid{0.0};
    double ask{0.0};
};

struct BybitChainInfo {
    std::string chain;
    bool deposit_enabled{false};
    bool withdraw_enabled{false};
};

struct BorrowCheck {
    bool available{false};
    bool shortable{false};
    double max_trade_qty{0.0};
    double max_trade_amount{0.0};
};

std::string extract_host(const std::string& url) {
    std::string host = url;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    else if (host.rfind("http://", 0) == 0) host = host.substr(7);

    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) host.resize(slash_pos);

    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos) host.resize(colon_pos);

    return host;
}

bool looks_unresolved_env(const std::string& value) {
    return value.size() >= 4 && value[0] == '$' && value[1] == '{' && value.back() == '}';
}

bool has_usable_bybit_auth(const ExchangeCredentials& creds) {
    return !creds.api_key.empty() &&
           !creds.secret_key.empty() &&
           !looks_unresolved_env(creds.api_key) &&
           !looks_unresolved_env(creds.secret_key);
}

std::string normalize_chain(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

double parse_json_double(simdjson::simdjson_result<simdjson::ondemand::value> value,
                         double fallback = 0.0) {
    auto double_result = value.get_double();
    if (!double_result.error()) {
        return double_result.value();
    }
    auto int_result = value.get_int64();
    if (!int_result.error()) {
        return static_cast<double>(int_result.value());
    }
    auto uint_result = value.get_uint64();
    if (!uint_result.error()) {
        return static_cast<double>(uint_result.value());
    }
    auto string_result = value.get_string();
    if (!string_result.error()) {
        return opt::fast_stod(string_result.value());
    }
    return fallback;
}

std::optional<bool> parse_json_boolish(simdjson::simdjson_result<simdjson::ondemand::value> value) {
    auto bool_result = value.get_bool();
    if (!bool_result.error()) {
        return bool_result.value();
    }
    auto int_result = value.get_int64();
    if (!int_result.error()) {
        return int_result.value() != 0;
    }
    auto uint_result = value.get_uint64();
    if (!uint_result.error()) {
        return uint_result.value() != 0;
    }
    auto string_result = value.get_string();
    if (!string_result.error()) {
        std::string raw(string_result.value());
        std::transform(raw.begin(), raw.end(), raw.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (raw == "1" || raw == "true" || raw == "y" || raw == "yes") return true;
        if (raw == "0" || raw == "false" || raw == "n" || raw == "no") return false;
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> build_bybit_auth_headers(
    const ExchangeCredentials& creds,
    const std::string& params) {
    int64_t timestamp = utils::Crypto::timestamp_ms();
    constexpr const char* recv_window = "5000";
    std::string message = std::to_string(timestamp) + creds.api_key + recv_window + params;
    std::string signature = utils::Crypto::hmac_sha256(creds.secret_key, message);

    return {
        {"X-BAPI-API-KEY", creds.api_key},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", std::to_string(timestamp)},
        {"X-BAPI-RECV-WINDOW", recv_window},
    };
}

bool parse_bithumb_orderbooks(
    const HttpResponse& response,
    std::unordered_map<std::string, BithumbQuote>& quotes) {
    if (!response.success) {
        Logger::error("[SpotRelay] Bithumb orderbook fetch failed: {}", response.error);
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto status = doc["status"].get_string();
    if (status.error() || status.value() != "0000") {
        Logger::error("[SpotRelay] Bithumb orderbook status error");
        return false;
    }

    for (auto field : doc["data"].get_object()) {
        std::string key(field.unescaped_key().value());
        if (key == "timestamp" || key == "payment_currency") continue;

        auto item = field.value().get_object();
        double bid = 0.0;
        double ask = 0.0;

        auto bids = item["bids"].get_array();
        if (!bids.error()) {
            for (auto bid_item : bids.value()) {
                bid = parse_json_double(bid_item["price"]);
                if (bid > 0.0) break;
            }
        }

        auto asks = item["asks"].get_array();
        if (!asks.error()) {
            for (auto ask_item : asks.value()) {
                ask = parse_json_double(ask_item["price"]);
                if (ask > 0.0) break;
            }
        }

        if (bid > 0.0 && ask > 0.0) {
            quotes.emplace(std::move(key), BithumbQuote{bid, ask});
        }
    }

    return !quotes.empty();
}

bool parse_bithumb_assetsstatus_all(
    const HttpResponse& response,
    std::unordered_map<std::string, BithumbAssetStatus>& statuses) {
    if (!response.success) {
        Logger::error("[SpotRelay] Bithumb assetsstatus fetch failed: {}", response.error);
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto status = doc["status"].get_string();
    if (status.error() || status.value() != "0000") {
        Logger::error("[SpotRelay] Bithumb assetsstatus status error");
        return false;
    }

    for (auto field : doc["data"].get_object()) {
        std::string currency(field.unescaped_key().value());
        auto item = field.value().get_object();
        bool deposit_enabled = false;
        bool withdraw_enabled = false;

        auto deposit_result = parse_json_boolish(item["deposit_status"]);
        if (deposit_result) deposit_enabled = *deposit_result;

        auto withdraw_result = parse_json_boolish(item["withdrawal_status"]);
        if (withdraw_result) withdraw_enabled = *withdraw_result;

        statuses.emplace(std::move(currency), BithumbAssetStatus{deposit_enabled, withdraw_enabled});
    }

    return !statuses.empty();
}

std::vector<std::string> parse_bithumb_multichain_withdrawable_networks(const HttpResponse& response) {
    std::vector<std::string> networks;
    if (!response.success) {
        return networks;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto status = doc["status"].get_string();
    if (status.error() || status.value() != "0000") {
        return networks;
    }

    auto data = doc["data"].get_array();
    if (data.error()) {
        return networks;
    }

    for (auto item : data.value()) {
        bool withdraw_enabled = parse_json_boolish(item["withdrawal_status"]).value_or(false);
        if (!withdraw_enabled) continue;
        auto net_type = item["net_type"].get_string();
        if (net_type.error()) continue;
        networks.push_back(normalize_chain(std::string(net_type.value())));
    }

    std::sort(networks.begin(), networks.end());
    networks.erase(std::unique(networks.begin(), networks.end()), networks.end());
    return networks;
}

bool parse_bybit_spot_instruments(
    const HttpResponse& response,
    std::unordered_map<std::string, BybitSpotInstrument>& instruments) {
    if (!response.success) {
        Logger::error("[SpotRelay] Bybit spot instruments fetch failed: {}", response.error);
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto ret_code = doc["retCode"].get_int64();
    if (ret_code.error() || ret_code.value() != 0) {
        Logger::error("[SpotRelay] Bybit spot instruments returned non-zero retCode");
        return false;
    }

    auto list = doc["result"]["list"].get_array();
    if (list.error()) {
        return false;
    }

    for (auto item : list.value()) {
        auto quote = item["quoteCoin"].get_string();
        if (quote.error() || quote.value() != "USDT") continue;

        BybitSpotInstrument info;
        info.symbol = std::string(item["symbol"].get_string().value());
        info.base = std::string(item["baseCoin"].get_string().value());
        info.quote = "USDT";
        auto status = item["status"].get_string();
        info.trading = !status.error() && status.value() == "Trading";

        auto margin_mode = item["marginTrading"].get_string();
        if (!margin_mode.error()) {
            info.margin_mode = std::string(margin_mode.value());
            std::string lowered = info.margin_mode;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            info.margin_enabled = lowered != "none";
        }

        instruments.emplace(info.base, std::move(info));
    }

    return !instruments.empty();
}

bool parse_bybit_spot_tickers(
    const HttpResponse& response,
    std::unordered_map<std::string, BybitSpotQuote>& quotes) {
    if (!response.success) {
        Logger::error("[SpotRelay] Bybit spot tickers fetch failed: {}", response.error);
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto ret_code = doc["retCode"].get_int64();
    if (ret_code.error() || ret_code.value() != 0) {
        Logger::error("[SpotRelay] Bybit spot tickers returned non-zero retCode");
        return false;
    }

    auto list = doc["result"]["list"].get_array();
    if (list.error()) {
        return false;
    }

    for (auto item : list.value()) {
        std::string symbol(item["symbol"].get_string().value());
        if (symbol.size() <= 4 || symbol.substr(symbol.size() - 4) != "USDT") continue;

        std::string base = symbol.substr(0, symbol.size() - 4);
        double bid = parse_json_double(item["bid1Price"]);
        double ask = parse_json_double(item["ask1Price"]);
        if (bid <= 0.0 || ask <= 0.0) continue;
        quotes.emplace(std::move(base), BybitSpotQuote{bid, ask});
    }

    return !quotes.empty();
}

bool parse_bybit_coin_info(
    const HttpResponse& response,
    std::unordered_map<std::string, std::vector<BybitChainInfo>>& coin_info) {
    if (!response.success) {
        Logger::warn("[SpotRelay] Bybit coin info fetch failed: {}", response.error);
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto ret_code = doc["retCode"].get_int64();
    if (ret_code.error() || ret_code.value() != 0) {
        Logger::warn("[SpotRelay] Bybit coin info returned non-zero retCode");
        return false;
    }

    auto rows = doc["result"]["rows"].get_array();
    if (rows.error()) {
        return false;
    }

    for (auto row : rows.value()) {
        auto coin = row["coin"].get_string();
        if (coin.error()) continue;

        std::vector<BybitChainInfo> chains;
        auto chain_arr = row["chains"].get_array();
        if (!chain_arr.error()) {
            for (auto chain_item : chain_arr.value()) {
                BybitChainInfo info;
                auto chain_value = chain_item["chain"].get_string();
                if (!chain_value.error()) {
                    info.chain = normalize_chain(std::string(chain_value.value()));
                } else {
                    auto chain_type_value = chain_item["chainType"].get_string();
                    if (!chain_type_value.error()) {
                        info.chain = normalize_chain(std::string(chain_type_value.value()));
                    }
                }
                info.deposit_enabled = parse_json_boolish(chain_item["chainDeposit"]).value_or(false);
                info.withdraw_enabled = parse_json_boolish(chain_item["chainWithdraw"]).value_or(false);
                if (!info.chain.empty()) {
                    chains.push_back(std::move(info));
                }
            }
        }

        coin_info.emplace(std::string(coin.value()), std::move(chains));
    }

    return !coin_info.empty();
}

BorrowCheck parse_bybit_borrow_check(const HttpResponse& response) {
    BorrowCheck check;
    if (!response.success) {
        return check;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc = parser.iterate(padded);
    auto ret_code = doc["retCode"].get_int64();
    if (ret_code.error() || ret_code.value() != 0) {
        return check;
    }

    auto result = doc["result"];
    if (result.error()) {
        return check;
    }

    check.available = true;
    check.max_trade_qty = parse_json_double(result["maxTradeQty"]);
    check.max_trade_amount = parse_json_double(result["maxTradeAmount"]);
    check.shortable = (check.max_trade_qty > 0.0) || (check.max_trade_amount > 0.0);
    return check;
}

std::vector<std::string> intersect_networks(
    const std::vector<std::string>& bithumb_networks,
    const std::vector<BybitChainInfo>& bybit_chains) {
    std::set<std::string> bybit_deposit_networks;
    for (const auto& chain : bybit_chains) {
        if (chain.deposit_enabled) {
            bybit_deposit_networks.insert(chain.chain);
        }
    }

    std::vector<std::string> shared;
    for (const auto& net : bithumb_networks) {
        if (bybit_deposit_networks.count(net)) {
            shared.push_back(net);
        }
    }

    std::sort(shared.begin(), shared.end());
    shared.erase(std::unique(shared.begin(), shared.end()), shared.end());
    return shared;
}

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

bool write_candidates_json(
    const std::string& path,
    const std::vector<SpotRelayCandidate>& candidates,
    bool auth_enriched) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        Logger::error("[SpotRelay] Failed to open JSON output: {}", path);
        return false;
    }

    out << "{\n";
    out << "  \"generatedAtMs\": " << utils::Crypto::timestamp_ms() << ",\n";
    out << "  \"authEnriched\": " << (auth_enriched ? "true" : "false") << ",\n";
    out << "  \"candidates\": [\n";

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        out << "    {\n";
        out << fmt::format("      \"base\": \"{}\",\n", json_escape(c.base));
        out << fmt::format("      \"bybitSymbol\": \"{}\",\n", json_escape(c.bybit_symbol));
        out << fmt::format("      \"bithumbBidKrw\": {:.8f},\n", c.bithumb_bid_krw);
        out << fmt::format("      \"bithumbAskKrw\": {:.8f},\n", c.bithumb_ask_krw);
        out << fmt::format("      \"bybitBidUsdt\": {:.8f},\n", c.bybit_bid_usdt);
        out << fmt::format("      \"bybitAskUsdt\": {:.8f},\n", c.bybit_ask_usdt);
        out << fmt::format("      \"usdtBidKrw\": {:.8f},\n", c.usdt_bid_krw);
        out << fmt::format("      \"usdtAskKrw\": {:.8f},\n", c.usdt_ask_krw);
        out << fmt::format("      \"grossEdgePct\": {:.6f},\n", c.gross_edge_pct);
        out << fmt::format("      \"bithumbWithdrawEnabled\": {},\n", c.bithumb_withdraw_enabled ? "true" : "false");
        out << fmt::format("      \"bybitSpotTrading\": {},\n", c.bybit_spot_trading ? "true" : "false");
        out << fmt::format("      \"bybitMarginEnabled\": {},\n", c.bybit_margin_enabled ? "true" : "false");
        out << fmt::format("      \"bybitMarginMode\": \"{}\",\n", json_escape(c.bybit_margin_mode));
        out << fmt::format("      \"bybitShortable\": {},\n", c.bybit_shortable ? "true" : "false");
        out << fmt::format("      \"borrowCheckAvailable\": {},\n", c.borrow_check_available ? "true" : "false");
        out << fmt::format("      \"bybitMaxShortQty\": {:.8f},\n", c.bybit_max_short_qty);
        out << fmt::format("      \"bybitMaxShortUsdt\": {:.8f},\n", c.bybit_max_short_usdt);
        out << fmt::format("      \"bybitDepositEnabled\": {},\n", c.bybit_deposit_enabled ? "true" : "false");
        out << fmt::format("      \"bybitWithdrawEnabled\": {},\n", c.bybit_withdraw_enabled ? "true" : "false");
        out << fmt::format("      \"transferReady\": {},\n", c.transfer_ready() ? "true" : "false");
        out << "      \"sharedNetworks\": [";
        for (size_t j = 0; j < c.shared_networks.size(); ++j) {
            if (j > 0) out << ", ";
            out << fmt::format("\"{}\"", json_escape(c.shared_networks[j]));
        }
        out << "]\n";
        out << "    }";
        if (i + 1 != candidates.size()) {
            out << ",";
        }
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    return true;
}

} // namespace

bool SpotRelayScanner::run(const RuntimeConfig& config, const Options& options, std::ostream& out) {
    const auto bithumb_it = config.exchanges.find(Exchange::Bithumb);
    const auto bybit_it = config.exchanges.find(Exchange::Bybit);
    if (bithumb_it == config.exchanges.end() || bybit_it == config.exchanges.end()) {
        out << "Bithumb/Bybit config is missing\n";
        return false;
    }

    const auto& bithumb_creds = bithumb_it->second;
    const auto& bybit_creds = bybit_it->second;

    if (!bithumb_creds.enabled || !bybit_creds.enabled) {
        out << "Bithumb and Bybit must both be enabled for spot relay scan\n";
        return false;
    }

    boost::asio::io_context io_context;
    RestClient bithumb_rest(io_context, extract_host(bithumb_creds.rest_endpoint));
    RestClient bybit_rest(io_context, extract_host(bybit_creds.rest_endpoint));

    if (!bithumb_rest.initialize()) {
        out << "Failed to initialize Bithumb REST client\n";
        return false;
    }
    if (!bybit_rest.initialize()) {
        out << "Failed to initialize Bybit REST client\n";
        return false;
    }

    const bool bybit_auth = has_usable_bybit_auth(bybit_creds);
    out << "[spot-relay] fetching public universe...\n";

    auto bithumb_orderbooks_future = bithumb_rest.get_async("/public/orderbook/ALL_KRW?count=1");
    auto bithumb_assets_future = bithumb_rest.get_async("/public/assetsstatus/ALL");
    auto bybit_instruments_future = bybit_rest.get_async("/v5/market/instruments-info?category=spot&limit=1000");
    auto bybit_tickers_future = bybit_rest.get_async("/v5/market/tickers?category=spot");

    std::unordered_map<std::string, BithumbQuote> bithumb_quotes;
    std::unordered_map<std::string, BithumbAssetStatus> bithumb_statuses;
    std::unordered_map<std::string, BybitSpotInstrument> bybit_instruments;
    std::unordered_map<std::string, BybitSpotQuote> bybit_quotes;

    if (!parse_bithumb_orderbooks(bithumb_orderbooks_future.get(), bithumb_quotes)) {
        out << "Failed to load Bithumb orderbooks\n";
        return false;
    }
    if (!parse_bithumb_assetsstatus_all(bithumb_assets_future.get(), bithumb_statuses)) {
        out << "Failed to load Bithumb asset statuses\n";
        return false;
    }
    if (!parse_bybit_spot_instruments(bybit_instruments_future.get(), bybit_instruments)) {
        out << "Failed to load Bybit spot instruments\n";
        return false;
    }
    if (!parse_bybit_spot_tickers(bybit_tickers_future.get(), bybit_quotes)) {
        out << "Failed to load Bybit spot tickers\n";
        return false;
    }

    auto usdt_it = bithumb_quotes.find("USDT");
    if (usdt_it == bithumb_quotes.end()) {
        out << "Bithumb USDT/KRW quote is missing\n";
        return false;
    }

    std::unordered_map<std::string, std::vector<BybitChainInfo>> bybit_coin_info;
    if (bybit_auth) {
        out << "[spot-relay] fetching Bybit auth-enriched coin info...\n";
        auto headers = build_bybit_auth_headers(bybit_creds, "");
        parse_bybit_coin_info(bybit_rest.get("/v5/asset/coin/query-info", headers), bybit_coin_info);
    } else {
        out << "[spot-relay] Bybit API key/secret missing; borrow/deposit enrichment skipped\n";
    }

    std::vector<std::string> common_bases;
    common_bases.reserve(bithumb_quotes.size());
    for (const auto& [base, quote] : bithumb_quotes) {
        if (base == "USDT") continue;
        if (!bybit_instruments.count(base)) continue;
        if (!bybit_quotes.count(base)) continue;
        common_bases.push_back(base);
    }
    std::sort(common_bases.begin(), common_bases.end());

    std::unordered_map<std::string, std::vector<std::string>> bithumb_networks;
    if (bybit_auth && !common_bases.empty()) {
        out << fmt::format("[spot-relay] fetching Bithumb multichain status for {} common symbols...\n",
                           common_bases.size());

        constexpr size_t batch_size = 8;
        for (size_t start = 0; start < common_bases.size(); start += batch_size) {
            size_t end = std::min(start + batch_size, common_bases.size());
            std::vector<std::pair<std::string, std::future<HttpResponse>>> pending;
            pending.reserve(end - start);

            for (size_t i = start; i < end; ++i) {
                const auto& coin = common_bases[i];
                pending.emplace_back(
                    coin,
                    bithumb_rest.get_async("/public/assetsstatus/multichain/" + coin));
            }

            for (auto& [coin, future] : pending) {
                bithumb_networks.emplace(coin, parse_bithumb_multichain_withdrawable_networks(future.get()));
            }
        }
    }

    std::vector<SpotRelayCandidate> candidates;
    candidates.reserve(common_bases.size());

    for (const auto& base : common_bases) {
        const auto& kr = bithumb_quotes.at(base);
        const auto& spot = bybit_quotes.at(base);
        const auto& instr = bybit_instruments.at(base);

        SpotRelayCandidate candidate;
        candidate.base = base;
        candidate.bybit_symbol = instr.symbol;
        candidate.bithumb_bid_krw = kr.bid;
        candidate.bithumb_ask_krw = kr.ask;
        candidate.bybit_bid_usdt = spot.bid;
        candidate.bybit_ask_usdt = spot.ask;
        candidate.usdt_bid_krw = usdt_it->second.bid;
        candidate.usdt_ask_krw = usdt_it->second.ask;
        candidate.bybit_spot_trading = instr.trading;
        candidate.bybit_margin_enabled = instr.margin_enabled;
        candidate.bybit_margin_mode = instr.margin_mode;

        const auto status_it = bithumb_statuses.find(base);
        if (status_it != bithumb_statuses.end()) {
            candidate.bithumb_withdraw_enabled = status_it->second.withdraw_enabled;
        }

        const double conservative_krw_out = candidate.bybit_bid_usdt * candidate.usdt_bid_krw;
        if (candidate.bithumb_ask_krw > 0.0) {
            candidate.gross_edge_pct =
                ((conservative_krw_out - candidate.bithumb_ask_krw) / candidate.bithumb_ask_krw) * 100.0;
        }

        if (bybit_auth) {
            const auto coin_it = bybit_coin_info.find(base);
            if (coin_it != bybit_coin_info.end()) {
                candidate.shared_networks =
                    intersect_networks(bithumb_networks[base], coin_it->second);

                for (const auto& chain : coin_it->second) {
                    candidate.bybit_deposit_enabled = candidate.bybit_deposit_enabled || chain.deposit_enabled;
                    candidate.bybit_withdraw_enabled = candidate.bybit_withdraw_enabled || chain.withdraw_enabled;
                }
            }

            if (candidate.bybit_margin_enabled) {
                const std::string query =
                    "category=spot&symbol=" + candidate.bybit_symbol + "&side=Sell";
                auto headers = build_bybit_auth_headers(bybit_creds, query);
                BorrowCheck borrow = parse_bybit_borrow_check(
                    bybit_rest.get("/v5/order/spot-borrow-check?" + query, headers));
                candidate.borrow_check_available = borrow.available;
                candidate.bybit_shortable = borrow.shortable;
                candidate.bybit_max_short_qty = borrow.max_trade_qty;
                candidate.bybit_max_short_usdt = borrow.max_trade_amount;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }

        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const SpotRelayCandidate& a, const SpotRelayCandidate& b) {
                  if (a.transfer_ready() != b.transfer_ready()) {
                      return a.transfer_ready() > b.transfer_ready();
                  }
                  if (a.bybit_shortable != b.bybit_shortable) {
                      return a.bybit_shortable > b.bybit_shortable;
                  }
                  return a.gross_edge_pct > b.gross_edge_pct;
              });

    const auto output_path = std::filesystem::path(options.json_output_path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    write_candidates_json(options.json_output_path, candidates, bybit_auth);

    out << fmt::format(
        "[spot-relay] common={} auth_enriched={} json={}\n",
        candidates.size(),
        bybit_auth ? "true" : "false",
        options.json_output_path);
    out << "base        edge%    transfer  bybitShort  marginMode    sharedNetworks\n";
    out << "-----------------------------------------------------------------------\n";

    const size_t max_rows = std::min<size_t>(50, candidates.size());
    for (size_t i = 0; i < max_rows; ++i) {
        const auto& c = candidates[i];
        std::string nets = c.shared_networks.empty() ? "-" : c.shared_networks.front();
        if (c.shared_networks.size() > 1) {
            nets += fmt::format("+{}", c.shared_networks.size() - 1);
        }
        out << fmt::format(
            "{:<10} {:>7.3f} {:>10} {:>11} {:<13} {}\n",
            c.base,
            c.gross_edge_pct,
            c.transfer_ready() ? "Y" : "N",
            c.bybit_shortable ? "Y" : "N",
            c.bybit_margin_mode.empty() ? "-" : c.bybit_margin_mode,
            nets);
    }

    bithumb_rest.shutdown();
    bybit_rest.shutdown();
    return true;
}

} // namespace kimp::strategy
