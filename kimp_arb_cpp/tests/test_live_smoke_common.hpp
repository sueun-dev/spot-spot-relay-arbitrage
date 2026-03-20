#pragma once

#include "kimp/core/config.hpp"
#include "kimp/core/dotenv.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/types.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/okx/okx.hpp"
#include "kimp/exchange/upbit/upbit.hpp"

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace live_smoke {

namespace net = boost::asio;

using kimp::Exchange;
using kimp::ExchangeCredentials;
using kimp::RuntimeConfig;
using kimp::SymbolId;
using kimp::exchange::AccountBalance;

inline std::string find_exchange_name(Exchange ex) {
    return std::string(kimp::exchange_name(ex));
}

inline std::optional<std::filesystem::path> find_config_path() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (ec) {
        return std::nullopt;
    }

    for (int depth = 0; depth < 8; ++depth) {
        const auto direct = current / "config" / "config.yaml";
        if (std::filesystem::exists(direct)) {
            return direct;
        }

        const auto nested = current / "kimp_arb_cpp" / "config" / "config.yaml";
        if (std::filesystem::exists(nested)) {
            return nested;
        }

        if (!current.has_parent_path()) {
            break;
        }
        auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = std::move(parent);
    }

    return std::nullopt;
}

inline void init_test_logger(const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    const std::string log_path = "logs/" + name + ".log";
    kimp::Logger::init(log_path, "info", 10, 3, 2048, true);
}

inline RuntimeConfig load_runtime_config_or_throw() {
    std::size_t loaded_count = 0;
    kimp::load_dotenv_if_present(&loaded_count, &std::cerr);

    const auto config_path = find_config_path();
    if (!config_path) {
        throw std::runtime_error("config/config.yaml not found");
    }

    RuntimeConfig config = kimp::ConfigLoader::load(config_path->string());
    if (config.exchanges.empty()) {
        throw std::runtime_error("failed to load runtime config");
    }

    return config;
}

inline const ExchangeCredentials& require_exchange_creds(
    const RuntimeConfig& config,
    Exchange ex,
    bool require_secret = true,
    bool require_passphrase = false) {
    auto it = config.exchanges.find(ex);
    if (it == config.exchanges.end()) {
        throw std::runtime_error(find_exchange_name(ex) + " config missing");
    }

    const auto& creds = it->second;
    if (!creds.enabled) {
        throw std::runtime_error(find_exchange_name(ex) + " disabled in config");
    }
    if (creds.api_key.empty()) {
        throw std::runtime_error(find_exchange_name(ex) + " api_key missing after dotenv load");
    }
    if (require_secret && creds.secret_key.empty()) {
        throw std::runtime_error(find_exchange_name(ex) + " secret_key missing after dotenv load");
    }
    if (require_passphrase && creds.passphrase.empty()) {
        throw std::runtime_error(find_exchange_name(ex) + " passphrase missing after dotenv load");
    }
    return creds;
}

template <typename ExchangePtr>
std::vector<AccountBalance> fetch_balances_or_throw(const std::string& name, const ExchangePtr& ex) {
    if (!ex) {
        throw std::runtime_error(name + " exchange not constructed");
    }
    if (!ex->initialize_rest()) {
        throw std::runtime_error(name + " REST init failed");
    }
    auto balances = ex->get_all_balances();
    ex->shutdown_rest();
    return balances;
}

template <typename ExchangePtr>
std::vector<SymbolId> fetch_symbols_or_throw(const std::string& name, const ExchangePtr& ex) {
    if (!ex) {
        throw std::runtime_error(name + " exchange not constructed");
    }
    if (!ex->initialize_rest()) {
        throw std::runtime_error(name + " REST init failed");
    }
    auto symbols = ex->get_available_symbols();
    ex->shutdown_rest();
    if (symbols.empty()) {
        throw std::runtime_error(name + " returned 0 symbols");
    }
    return symbols;
}

inline double available_of(const std::vector<AccountBalance>& balances, const std::string& currency) {
    for (const auto& balance : balances) {
        if (balance.currency == currency) {
            return balance.available;
        }
    }
    return 0.0;
}

inline std::unordered_set<std::string> base_set(const std::vector<SymbolId>& symbols) {
    std::unordered_set<std::string> out;
    out.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        out.insert(std::string(symbol.get_base()));
    }
    return out;
}

inline std::unordered_set<std::string> set_union(
    const std::unordered_set<std::string>& lhs,
    const std::unordered_set<std::string>& rhs) {
    std::unordered_set<std::string> out = lhs;
    out.insert(rhs.begin(), rhs.end());
    return out;
}

inline std::vector<std::string> set_intersection(
    const std::unordered_set<std::string>& lhs,
    const std::unordered_set<std::string>& rhs) {
    std::vector<std::string> out;
    out.reserve(std::min(lhs.size(), rhs.size()));
    const auto& smaller = lhs.size() <= rhs.size() ? lhs : rhs;
    const auto& larger = lhs.size() <= rhs.size() ? rhs : lhs;
    for (const auto& item : smaller) {
        if (larger.contains(item)) {
            out.push_back(item);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline void print_balance_line(const std::string& venue,
                               const std::vector<AccountBalance>& balances,
                               const std::string& currency) {
    std::cout << venue << " " << currency << " available: "
              << available_of(balances, currency) << "\n";
}

} // namespace live_smoke
