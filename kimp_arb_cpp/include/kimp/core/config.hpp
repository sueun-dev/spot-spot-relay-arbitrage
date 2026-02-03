#pragma once

#include "types.hpp"
#include <string>
#include <unordered_map>
#include <optional>

namespace kimp {

// Exchange credentials
struct ExchangeCredentials {
    std::string api_key;
    std::string secret_key;
    std::string ws_endpoint;
    std::string ws_private_endpoint;
    std::string rest_endpoint;
    bool enabled{true};
};

// Runtime configuration (loaded from YAML)
struct RuntimeConfig {
    // Trading parameters (can override TradingConfig defaults)
    int max_positions{TradingConfig::MAX_POSITIONS};
    double position_size_usd{TradingConfig::POSITION_SIZE_USD};
    int split_orders{TradingConfig::SPLIT_ORDERS};
    double order_size_usd{TradingConfig::ORDER_SIZE_USD};
    int order_interval_ms{TradingConfig::ORDER_INTERVAL_MS};
    double entry_premium_threshold{TradingConfig::ENTRY_PREMIUM_THRESHOLD};
    double exit_premium_threshold{TradingConfig::EXIT_PREMIUM_THRESHOLD};
    double max_price_diff_percent{TradingConfig::MAX_PRICE_DIFF_PERCENT};
    int usdt_update_interval_ms{TradingConfig::USDT_UPDATE_INTERVAL_MS};

    // Exchange credentials
    std::unordered_map<Exchange, ExchangeCredentials> exchanges;

    // Threading
    int io_threads{4};
    int market_data_threads{1};
    int strategy_threads{1};
    int order_exec_threads{2};
    bool use_cpu_affinity{false};

    // Logging
    std::string log_level{"info"};
    std::string log_file{"logs/kimp_bot.log"};
    int log_max_size_mb{100};
    int log_max_files{10};

    // Performance options
    bool use_io_uring{false};
    bool preallocate_buffers{true};
    int buffer_pool_size{256};
    int ring_buffer_size{4096};
};

// Configuration loader
class ConfigLoader {
public:
    // Load from YAML file
    static RuntimeConfig load(const std::string& path);

    // Load from environment variables (fallback)
    static RuntimeConfig load_from_env();

    // Get default configuration
    static RuntimeConfig get_default();

private:
    static std::string get_env(const std::string& name, const std::string& default_value = "");
    static std::string expand_env_vars(const std::string& value);
};

// Default WebSocket endpoints
namespace endpoints {

// Upbit
constexpr const char* UPBIT_WS = "wss://api.upbit.com/websocket/v1";
constexpr const char* UPBIT_REST = "https://api.upbit.com";

// Bithumb
constexpr const char* BITHUMB_WS = "wss://pubwss.bithumb.com/pub/ws";
constexpr const char* BITHUMB_REST = "https://api.bithumb.com";

// Bybit
constexpr const char* BYBIT_WS_PUBLIC = "wss://stream.bybit.com/v5/public/linear";
constexpr const char* BYBIT_WS_PRIVATE = "wss://stream.bybit.com/v5/private";
constexpr const char* BYBIT_REST = "https://api.bybit.com";

// Gate.io
constexpr const char* GATEIO_WS = "wss://fx-ws.gateio.ws/v4/ws/usdt";
constexpr const char* GATEIO_REST = "https://api.gateio.ws";

} // namespace endpoints

} // namespace kimp
