#include "kimp/core/config.hpp"
#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <fstream>
#include <regex>

namespace kimp {

std::string ConfigLoader::get_env(const std::string& name, const std::string& default_value) {
    const char* value = std::getenv(name.c_str());
    return value ? value : default_value;
}

std::string ConfigLoader::expand_env_vars(const std::string& value) {
    std::regex env_regex(R"(\$\{([^}]+)\})");
    std::string result = value;
    std::smatch match;

    while (std::regex_search(result, match, env_regex)) {
        std::string env_name = match[1].str();
        std::string env_value = get_env(env_name);
        result = match.prefix().str() + env_value + match.suffix().str();
    }

    return result;
}

RuntimeConfig ConfigLoader::load(const std::string& path) {
    RuntimeConfig config = get_default();

    try {
        YAML::Node yaml = YAML::LoadFile(path);

        // Trading config
        if (yaml["trading"]) {
            auto t = yaml["trading"];
            if (t["max_positions"]) config.max_positions = t["max_positions"].as<int>();
            if (t["position_size_usd"]) config.position_size_usd = t["position_size_usd"].as<double>();
            if (t["split_orders"]) config.split_orders = t["split_orders"].as<int>();
            if (t["order_size_usd"]) config.order_size_usd = t["order_size_usd"].as<double>();
            if (t["order_interval_ms"]) config.order_interval_ms = t["order_interval_ms"].as<int>();
            if (t["entry_premium_threshold"]) config.entry_premium_threshold = t["entry_premium_threshold"].as<double>();
            if (t["exit_premium_threshold"]) config.exit_premium_threshold = t["exit_premium_threshold"].as<double>();
            if (t["max_price_diff_percent"]) config.max_price_diff_percent = t["max_price_diff_percent"].as<double>();
            if (t["usdt_update_interval_ms"]) config.usdt_update_interval_ms = t["usdt_update_interval_ms"].as<int>();
        }

        // Exchange configs
        auto load_exchange = [&](const std::string& name, Exchange ex) {
            if (yaml["exchanges"][name]) {
                auto e = yaml["exchanges"][name];
                ExchangeCredentials creds;

                creds.enabled = e["enabled"] ? e["enabled"].as<bool>() : true;
                if (e["ws_endpoint"]) creds.ws_endpoint = e["ws_endpoint"].as<std::string>();
                if (e["ws_private_endpoint"]) creds.ws_private_endpoint = e["ws_private_endpoint"].as<std::string>();
                if (e["ws_trade_endpoint"]) creds.ws_trade_endpoint = e["ws_trade_endpoint"].as<std::string>();
                if (e["rest_endpoint"]) creds.rest_endpoint = e["rest_endpoint"].as<std::string>();
                if (e["api_key"]) creds.api_key = expand_env_vars(e["api_key"].as<std::string>());
                if (e["secret_key"]) creds.secret_key = expand_env_vars(e["secret_key"].as<std::string>());

                config.exchanges[ex] = std::move(creds);
            }
        };

        load_exchange("upbit", Exchange::Upbit);
        load_exchange("bithumb", Exchange::Bithumb);
        load_exchange("bybit", Exchange::Bybit);
        load_exchange("gateio", Exchange::GateIO);

        // Threading config
        if (yaml["threading"]) {
            auto th = yaml["threading"];
            if (th["io_threads"]) config.io_threads = th["io_threads"].as<int>();
            if (th["market_data_threads"]) config.market_data_threads = th["market_data_threads"].as<int>();
            if (th["strategy_threads"]) config.strategy_threads = th["strategy_threads"].as<int>();
            if (th["order_exec_threads"]) config.order_exec_threads = th["order_exec_threads"].as<int>();
            if (th["use_cpu_affinity"]) config.use_cpu_affinity = th["use_cpu_affinity"].as<bool>();
        }

        // Logging config
        if (yaml["logging"]) {
            auto log = yaml["logging"];
            if (log["level"]) config.log_level = log["level"].as<std::string>();
            if (log["file"]) config.log_file = log["file"].as<std::string>();
            if (log["max_size_mb"]) config.log_max_size_mb = log["max_size_mb"].as<int>();
            if (log["max_files"]) config.log_max_files = log["max_files"].as<int>();
        }

        // Performance config
        if (yaml["performance"]) {
            auto perf = yaml["performance"];
            if (perf["use_io_uring"]) config.use_io_uring = perf["use_io_uring"].as<bool>();
            if (perf["preallocate_buffers"]) config.preallocate_buffers = perf["preallocate_buffers"].as<bool>();
            if (perf["buffer_pool_size"]) config.buffer_pool_size = perf["buffer_pool_size"].as<int>();
            if (perf["ring_buffer_size"]) config.ring_buffer_size = perf["ring_buffer_size"].as<int>();
        }

    } catch (const YAML::Exception& e) {
        // Return default config on error
    }

    return config;
}

RuntimeConfig ConfigLoader::load_from_env() {
    RuntimeConfig config = get_default();

    // Load from environment variables
    config.exchanges[Exchange::Upbit] = {
        get_env("UPBIT_ACCESS_KEY"),
        get_env("UPBIT_SECRET_KEY"),
        endpoints::UPBIT_WS,
        "",
        "",
        endpoints::UPBIT_REST,
        true
    };

    config.exchanges[Exchange::Bithumb] = {
        get_env("BITHUMB_API_KEY"),
        get_env("BITHUMB_SECRET_KEY"),
        endpoints::BITHUMB_WS,
        "",
        "",
        endpoints::BITHUMB_REST,
        true
    };

    config.exchanges[Exchange::Bybit] = {
        get_env("BYBIT_API_KEY"),
        get_env("BYBIT_SECRET_KEY"),
        endpoints::BYBIT_WS_PUBLIC,
        endpoints::BYBIT_WS_PRIVATE,
        endpoints::BYBIT_WS_TRADE,
        endpoints::BYBIT_REST,
        true
    };

    config.exchanges[Exchange::GateIO] = {
        get_env("GATE_API_KEY"),
        get_env("GATE_SECRET_KEY"),
        endpoints::GATEIO_WS,
        "",
        "",
        endpoints::GATEIO_REST,
        true
    };

    return config;
}

RuntimeConfig ConfigLoader::get_default() {
    RuntimeConfig config;

    // Default exchange endpoints
    config.exchanges[Exchange::Upbit] = {
        "", "", endpoints::UPBIT_WS, "", "", endpoints::UPBIT_REST, false
    };
    config.exchanges[Exchange::Bithumb] = {
        "", "", endpoints::BITHUMB_WS, "", "", endpoints::BITHUMB_REST, false
    };
    config.exchanges[Exchange::Bybit] = {
        "", "", endpoints::BYBIT_WS_PUBLIC, endpoints::BYBIT_WS_PRIVATE, endpoints::BYBIT_WS_TRADE, endpoints::BYBIT_REST, false
    };
    config.exchanges[Exchange::GateIO] = {
        "", "", endpoints::GATEIO_WS, "", "", endpoints::GATEIO_REST, false
    };

    return config;
}

} // namespace kimp
