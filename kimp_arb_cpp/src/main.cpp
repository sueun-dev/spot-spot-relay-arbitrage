#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/latency_probe.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/price_format.hpp"
#include "kimp/core/simd_premium.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/okx/okx.hpp"
#include "kimp/exchange/upbit/upbit.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/strategy/spot_relay_scanner.hpp"
#include "kimp/execution/lifecycle_executor.hpp"
#include "kimp/execution/order_manager.hpp"
#include "kimp/network/ws_broadcast_server.hpp"

#include <boost/asio.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <future>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <cctype>
#include <optional>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <simdjson.h>
#include <fmt/format.h>

namespace net = boost::asio;

std::atomic<bool> g_shutdown{false};

// Non-blocking stdin read that respects g_shutdown (Ctrl+C)
bool read_stdin_line(std::string& line) {
    struct pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    while (!g_shutdown) {
        int ret = ::poll(&pfd, 1, 500);  // 500ms timeout
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (!std::getline(std::cin, line)) return false;
            return true;
        }
    }
    return false;
}
std::atomic<int> g_active_loops{0};

struct LoopGuard {
    std::atomic<int>& counter;
    kimp::strategy::ArbitrageEngine* engine{nullptr};

    explicit LoopGuard(std::atomic<int>& c, kimp::strategy::ArbitrageEngine* e)
        : counter(c), engine(e) {}

    ~LoopGuard() {
        const int remaining = counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (engine && remaining < kimp::TradingConfig::MAX_POSITIONS) {
            engine->set_entry_suppressed(false);
        }
    }
};

struct LifecycleTask {
    kimp::ArbitrageSignal signal;
    std::optional<kimp::Position> initial_position;
};

struct LiveMonitorGuard {
    explicit LiveMonitorGuard(bool enable) : active_(enable && (::isatty(STDOUT_FILENO) == 1)) {
        if (active_) {
            // Use alternate screen so monitor redraw does not flood scrollback.
            std::cout << "\033[?1049h\033[?25l\033[H\033[2J";
            std::cout.flush();
        }
    }

    ~LiveMonitorGuard() {
        if (active_) {
            std::cout << "\033[?25h\033[?1049l";
            std::cout.flush();
        }
    }

    bool active() const { return active_; }

    void clear_frame() const {
        if (active_) {
            std::cout << "\033[H\033[2J";
        }
    }

private:
    bool active_{false};
};

void signal_handler(int) {
    static const char msg[] = "\nShutdown signal received...\n";
    (void)::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    g_shutdown.store(true, std::memory_order_release);
}

std::string format_time(const kimp::SystemTimestamp& ts) {
    auto tt = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void append_trade_log(const kimp::Position& pos,
                      double pnl_usd,
                      double usdt_rate,
                      double capital_before,
                      double capital_after) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    std::filesystem::create_directories("trade_logs");
    const std::string path = "trade_logs/trades.csv";

    bool need_header = !std::filesystem::exists(path) ||
                       std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    if (need_header) {
        out << "entry_time,exit_time,symbol,entry_premium,exit_premium,"
               "korean_entry_price,korean_exit_price,foreign_entry_price,foreign_exit_price,"
               "korean_amount,foreign_amount,pnl_krw,pnl_usd,usdt_rate,"
               "position_size_usd,notional_usd,return_pct,capital_before,capital_after,partial_exit\n";
    }

    double notional_usd = pos.position_size_usd * 2.0;
    double return_pct = notional_usd > 0.0 ? (pnl_usd / notional_usd) * 100.0 : 0.0;

    out << format_time(pos.entry_time) << ','
        << format_time(pos.exit_time) << ','
        << pos.symbol.to_string() << ','
        << std::fixed << std::setprecision(6) << pos.entry_premium << ','
        << std::fixed << std::setprecision(6) << pos.exit_premium << ','
        << std::fixed << std::setprecision(8) << pos.korean_entry_price << ','
        << std::fixed << std::setprecision(8) << pos.korean_exit_price << ','
        << std::fixed << std::setprecision(6) << pos.foreign_entry_price << ','
        << std::fixed << std::setprecision(6) << pos.foreign_exit_price << ','
        << std::fixed << std::setprecision(8) << pos.korean_amount << ','
        << std::fixed << std::setprecision(8) << pos.foreign_amount << ','
        << std::fixed << std::setprecision(2) << pos.realized_pnl_krw << ','
        << std::fixed << std::setprecision(2) << pnl_usd << ','
        << std::fixed << std::setprecision(2) << usdt_rate << ','
        << std::fixed << std::setprecision(2) << pos.position_size_usd << ','
        << std::fixed << std::setprecision(2) << notional_usd << ','
        << std::fixed << std::setprecision(4) << return_pct << ','
        << std::fixed << std::setprecision(2) << capital_before << ','
        << std::fixed << std::setprecision(2) << capital_after << ','
        << (pos.is_active ? "1" : "0")
        << '\n';
}

// =========================================================================
// Position persistence for crash recovery
// =========================================================================
const std::string ACTIVE_POSITION_PATH = "trade_logs/active_position.json";

void save_active_position(const kimp::Position& pos) {
    std::error_code ec;
    std::filesystem::create_directories("trade_logs", ec);
    if (ec) {
        spdlog::error("[PERSIST] Failed to create trade_logs dir: {}", ec.message());
        return;
    }

    auto entry_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        pos.entry_time.time_since_epoch()).count();

    std::string json = fmt::format(
        "{{\n"
        "  \"symbol_base\": \"{}\",\n"
        "  \"symbol_quote\": \"{}\",\n"
        "  \"korean_exchange\": {},\n"
        "  \"foreign_exchange\": {},\n"
        "  \"entry_time_ms\": {},\n"
        "  \"entry_premium\": {:.6f},\n"
        "  \"position_size_usd\": {:.2f},\n"
        "  \"korean_amount\": {:.8f},\n"
        "  \"foreign_amount\": {:.8f},\n"
        "  \"korean_entry_price\": {},\n"
        "  \"foreign_entry_price\": {:.8f},\n"
        "  \"realized_pnl_krw\": {:.2f},\n"
        "  \"is_active\": true\n"
        "}}",
        pos.symbol.get_base(), pos.symbol.get_quote(),
        static_cast<int>(pos.korean_exchange), static_cast<int>(pos.foreign_exchange),
        entry_ms, pos.entry_premium, pos.position_size_usd,
        pos.korean_amount, pos.foreign_amount,
        kimp::format::format_decimal_trimmed(pos.korean_entry_price), pos.foreign_entry_price,
        pos.realized_pnl_krw
    );

    // Atomic write: temp file → rename
    const std::string tmp_path = ACTIVE_POSITION_PATH + ".tmp";
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out) {
        spdlog::error("[PERSIST] Failed to open temp file: {}", tmp_path);
        return;
    }
    out << json;
    out.close();
    if (out.fail()) {
        spdlog::error("[PERSIST] Failed to write position data");
        return;
    }

    // fsync to ensure data is durable on disk before rename (crash safety)
    int fd = ::open(tmp_path.c_str(), O_WRONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }

    std::filesystem::rename(tmp_path, ACTIVE_POSITION_PATH, ec);
    if (ec) {
        spdlog::error("[PERSIST] Failed to rename temp file: {}", ec.message());
        return;
    }
    spdlog::debug("[PERSIST] Saved active position: {} ({:.8f} coins)",
                  pos.symbol.to_string(), pos.korean_amount);
}

std::optional<kimp::Position> load_active_position() {
    if (!std::filesystem::exists(ACTIVE_POSITION_PATH)) return std::nullopt;

    try {
        simdjson::dom::parser parser;
        simdjson::dom::element doc = parser.load(ACTIVE_POSITION_PATH);

        kimp::Position pos;
        std::string_view base = doc["symbol_base"];
        std::string_view quote = doc["symbol_quote"];
        pos.symbol = kimp::SymbolId(base, quote);
        pos.korean_exchange = static_cast<kimp::Exchange>(int64_t(doc["korean_exchange"]));
        pos.foreign_exchange = static_cast<kimp::Exchange>(int64_t(doc["foreign_exchange"]));

        int64_t entry_ms = doc["entry_time_ms"];
        pos.entry_time = kimp::SystemTimestamp(std::chrono::milliseconds(entry_ms));

        pos.entry_premium = double(doc["entry_premium"]);
        pos.position_size_usd = double(doc["position_size_usd"]);
        pos.korean_amount = double(doc["korean_amount"]);
        pos.foreign_amount = double(doc["foreign_amount"]);
        pos.korean_entry_price = double(doc["korean_entry_price"]);
        pos.foreign_entry_price = double(doc["foreign_entry_price"]);
        // Load interim P&L (may be 0 or absent for older files)
        auto pnl_field = doc.at_key("realized_pnl_krw");
        if (!pnl_field.error()) {
            pos.realized_pnl_krw = double(pnl_field.value());
        }
        pos.is_active = true;

        return pos;
    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("[PERSIST] Failed to load position file: {}", e.what());
        return std::nullopt;
    }
}

void delete_active_position() {
    std::error_code ec;
    bool removed = std::filesystem::remove(ACTIVE_POSITION_PATH, ec);
    if (ec) {
        spdlog::warn("[PERSIST] Failed to delete position file: {}", ec.message());
    } else if (removed) {
        spdlog::debug("[PERSIST] Deleted active position file");
    }
    // removed=false && !ec means file didn't exist, which is fine
}

std::string expand_env(const std::string& val) {
    if (val.size() > 3 && val[0] == '$' && val[1] == '{' && val.back() == '}') {
        std::string env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        if (!env_val) return {};
        return env_val;
    }
    return val;
}

std::string trim_ascii(std::string_view value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string unquote_env_value(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

bool load_dotenv_file(const std::filesystem::path& path, size_t& loaded_count) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.rfind("export ", 0) == 0) {
            trimmed = trim_ascii(trimmed.substr(7));
        }

        const size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            std::cerr << "Skipping malformed .env line " << line_no << ": " << path << std::endl;
            continue;
        }

        std::string key = trim_ascii(std::string_view(trimmed).substr(0, eq_pos));
        std::string value = trim_ascii(std::string_view(trimmed).substr(eq_pos + 1));
        if (key.empty()) {
            continue;
        }

        value = unquote_env_value(value);
        if (std::getenv(key.c_str()) == nullptr) {
            ::setenv(key.c_str(), value.c_str(), 0);
            ++loaded_count;
        }
    }
    return true;
}

std::optional<std::filesystem::path> find_dotenv_path() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (ec) {
        return std::nullopt;
    }

    for (int depth = 0; depth < 6; ++depth) {
        const auto candidate = current / ".env";
        if (std::filesystem::exists(candidate)) {
            return candidate;
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

void load_dotenv_if_present() {
    auto dotenv_path = find_dotenv_path();
    if (!dotenv_path) {
        return;
    }

    size_t loaded_count = 0;
    if (load_dotenv_file(*dotenv_path, loaded_count)) {
        std::cerr << "Loaded " << loaded_count << " env vars from " << dotenv_path->string() << std::endl;
    }
}

std::optional<std::string> env_placeholder_name(const std::string& val) {
    if (val.size() > 3 && val[0] == '$' && val[1] == '{' && val.back() == '}') {
        return val.substr(2, val.size() - 3);
    }
    return std::nullopt;
}

// NOTE: Trading parameters (thresholds, position sizes, fees) are compile-time
// constants in TradingConfig (types.hpp). YAML only controls credentials,
// logging, and threading. Rebuild to change trading parameters.
std::optional<kimp::RuntimeConfig> load_config(const std::string& path,
                                               bool require_private_keys = true) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Config file not found: " << path << std::endl;
        return std::nullopt;
    }

    kimp::RuntimeConfig config;

    try {
        YAML::Node yaml = YAML::LoadFile(path);

        // Logging
        if (yaml["logging"]) {
            auto l = yaml["logging"];
            if (l["level"]) config.log_level = l["level"].as<std::string>();
            if (l["file"]) config.log_file = l["file"].as<std::string>();
        }

        // Threading
        if (yaml["threading"]) {
            auto t = yaml["threading"];
            if (t["io_threads"]) config.io_threads = t["io_threads"].as<int>();
        }

        // Exchanges
        if (!yaml["exchanges"]) {
            std::cerr << "No 'exchanges' section in config" << std::endl;
            return std::nullopt;
        }

        auto load_exchange = [&](const std::string& name, kimp::Exchange ex) -> bool {
            if (!yaml["exchanges"][name]) {
                std::cerr << "Exchange '" << name << "' not found in config" << std::endl;
                return false;
            }

            auto e = yaml["exchanges"][name];
            kimp::ExchangeCredentials creds;
            creds.enabled = e["enabled"] ? e["enabled"].as<bool>() : true;

            if (!creds.enabled) {
                config.exchanges[ex] = std::move(creds);
                return true;
            }

            if (e["ws_endpoint"]) creds.ws_endpoint = e["ws_endpoint"].as<std::string>();
            if (e["ws_private_endpoint"]) creds.ws_private_endpoint = e["ws_private_endpoint"].as<std::string>();
            if (e["ws_trade_endpoint"]) creds.ws_trade_endpoint = e["ws_trade_endpoint"].as<std::string>();
            if (e["rest_endpoint"]) creds.rest_endpoint = e["rest_endpoint"].as<std::string>();
            if (e["api_key"]) {
                std::string raw = e["api_key"].as<std::string>();
                creds.api_key = require_private_keys ? expand_env(raw) : expand_env(raw);
            }
            if (e["secret_key"]) {
                std::string raw = e["secret_key"].as<std::string>();
                creds.secret_key = require_private_keys ? expand_env(raw) : expand_env(raw);
            }

            if (require_private_keys &&
                (creds.api_key.empty() || creds.secret_key.empty())) {
                std::vector<std::string> missing_vars;
                if (e["api_key"]) {
                    if (auto env_name = env_placeholder_name(e["api_key"].as<std::string>());
                        env_name && creds.api_key.empty()) {
                        missing_vars.push_back(*env_name);
                    }
                }
                if (e["secret_key"]) {
                    if (auto env_name = env_placeholder_name(e["secret_key"].as<std::string>());
                        env_name && creds.secret_key.empty()) {
                        missing_vars.push_back(*env_name);
                    }
                }

                std::cerr << "Exchange '" << name << "': private API credentials are missing";
                if (!missing_vars.empty()) {
                    std::cerr << " (set " << missing_vars.front();
                    for (std::size_t i = 1; i < missing_vars.size(); ++i) {
                        std::cerr << ", " << missing_vars[i];
                    }
                    std::cerr << ")";
                }
                std::cerr << ". Real trading requires exchange keys." << std::endl;
                return false;
            }

            config.exchanges[ex] = std::move(creds);
            return true;
        };

        if (!load_exchange("bithumb", kimp::Exchange::Bithumb)) return std::nullopt;
        if (!load_exchange("bybit", kimp::Exchange::Bybit)) return std::nullopt;

        // OKX is optional — load if present in config, skip silently if not
        if (yaml["exchanges"]["okx"]) {
            auto okx_node = yaml["exchanges"]["okx"];
            kimp::ExchangeCredentials okx_creds;
            okx_creds.enabled = okx_node["enabled"] ? okx_node["enabled"].as<bool>() : true;

            if (okx_creds.enabled) {
                if (okx_node["ws_endpoint"]) okx_creds.ws_endpoint = okx_node["ws_endpoint"].as<std::string>();
                if (okx_node["ws_private_endpoint"]) okx_creds.ws_private_endpoint = okx_node["ws_private_endpoint"].as<std::string>();
                if (okx_node["ws_trade_endpoint"]) okx_creds.ws_trade_endpoint = okx_node["ws_trade_endpoint"].as<std::string>();
                if (okx_node["rest_endpoint"]) okx_creds.rest_endpoint = okx_node["rest_endpoint"].as<std::string>();
                if (okx_node["api_key"]) okx_creds.api_key = expand_env(okx_node["api_key"].as<std::string>());
                if (okx_node["secret_key"]) okx_creds.secret_key = expand_env(okx_node["secret_key"].as<std::string>());
                if (okx_node["passphrase"]) okx_creds.passphrase = expand_env(okx_node["passphrase"].as<std::string>());

                if (!require_private_keys ||
                    (!okx_creds.api_key.empty() && !okx_creds.secret_key.empty() && !okx_creds.passphrase.empty())) {
                    config.exchanges[kimp::Exchange::OKX] = std::move(okx_creds);
                } else {
                    std::cerr << "OKX credentials incomplete (api_key, secret_key, passphrase required). Skipping OKX." << std::endl;
                    okx_creds.enabled = false;
                    config.exchanges[kimp::Exchange::OKX] = std::move(okx_creds);
                }
            } else {
                config.exchanges[kimp::Exchange::OKX] = std::move(okx_creds);
            }
        }

        // Upbit is optional — load if present in config, skip silently if not
        if (yaml["exchanges"]["upbit"]) {
            auto upbit_node = yaml["exchanges"]["upbit"];
            kimp::ExchangeCredentials upbit_creds;
            upbit_creds.enabled = upbit_node["enabled"] ? upbit_node["enabled"].as<bool>() : true;

            if (upbit_creds.enabled) {
                if (upbit_node["ws_endpoint"]) upbit_creds.ws_endpoint = upbit_node["ws_endpoint"].as<std::string>();
                if (upbit_node["rest_endpoint"]) upbit_creds.rest_endpoint = upbit_node["rest_endpoint"].as<std::string>();
                if (upbit_node["api_key"]) upbit_creds.api_key = expand_env(upbit_node["api_key"].as<std::string>());
                if (upbit_node["secret_key"]) upbit_creds.secret_key = expand_env(upbit_node["secret_key"].as<std::string>());
                // Upbit doesn't need credentials for public data (monitor-only)
                config.exchanges[kimp::Exchange::Upbit] = std::move(upbit_creds);
            } else {
                config.exchanges[kimp::Exchange::Upbit] = std::move(upbit_creds);
            }
        }

    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to parse config '" << path << "': "
                  << e.what() << std::endl;
        return std::nullopt;
    }

    return config;
}

int main(int argc, char* argv[]) {
    load_dotenv_if_present();

    std::string config_path = "config/config.yaml";
    bool monitor_mode = true;
    bool monitor_only = false;
    bool scan_spot_relay = false;
    std::optional<bool> dashboard_stream_override;
    std::optional<bool> latency_probe_override;
    std::optional<bool> latency_summary_override;
    kimp::LatencyOutputMode latency_output_mode = kimp::LatencyOutputMode::MmapBinary;
    int monitor_interval_sec = 1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a path argument\n";
                return 1;
            }
            config_path = argv[++i];
        } else if (arg == "-m" || arg == "--monitor") {
            monitor_mode = true;
        } else if (arg == "--no-monitor") {
            monitor_mode = false;
        } else if (arg == "--monitor-only") {
            monitor_only = true;
            monitor_mode = true;
        } else if (arg == "--scan-spot-relay") {
            scan_spot_relay = true;
            monitor_only = true;
            monitor_mode = false;
        } else if (arg == "--dashboard-stream") {
            dashboard_stream_override = true;
        } else if (arg == "--no-dashboard-stream") {
            dashboard_stream_override = false;
        } else if (arg == "--latency-probe") {
            latency_probe_override = true;
        } else if (arg == "--no-latency-probe") {
            latency_probe_override = false;
        } else if (arg == "--latency-probe-summary") {
            latency_summary_override = true;
        } else if (arg == "--no-latency-probe-summary") {
            latency_summary_override = false;
        } else if (arg == "--latency-probe-output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --latency-probe-output requires csv|binary|mmap\n";
                return 1;
            }
            std::string mode = argv[++i];
            if (mode == "csv") {
                latency_output_mode = kimp::LatencyOutputMode::CsvText;
            } else if (mode == "binary") {
                latency_output_mode = kimp::LatencyOutputMode::Binary;
            } else if (mode == "mmap") {
                latency_output_mode = kimp::LatencyOutputMode::MmapBinary;
            } else {
                std::cerr << "Error: --latency-probe-output must be csv, binary, or mmap\n";
                return 1;
            }
        } else if (arg == "--monitor-interval-sec") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --monitor-interval-sec requires a numeric argument\n";
                return 1;
            }
            monitor_interval_sec = std::stoi(argv[++i]);
            if (monitor_interval_sec <= 0) {
                std::cerr << "Error: --monitor-interval-sec must be > 0\n";
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "KIMP Arbitrage Bot - C++ HFT Version\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  -c, --config <path>  Config file (default: config/config.yaml)\n"
                      << "  -m, --monitor        Enable full console monitor (default: ON)\n"
                      << "      --no-monitor     Disable console monitor\n"
                      << "      --monitor-only   Monitor only (no position prompts, no auto-trading)\n"
                      << "      --dashboard-stream  Enable JSON exporter + local relay WS output\n"
                      << "      --no-dashboard-stream  Disable JSON exporter + local relay WS output\n"
                      << "      --latency-probe  Enable async latency event recording\n"
                      << "      --no-latency-probe  Disable async latency event recording\n"
                      << "      --latency-probe-output <csv|binary|mmap>  Latency event export format (default: mmap)\n"
                      << "      --latency-probe-summary  Enable latency summary export (default: OFF)\n"
                      << "      --no-latency-probe-summary  Disable latency summary export\n"
                      << "      --scan-spot-relay  Scan Bithumb↔Bybit spot-transfer candidates\n"
                      << "      --monitor-interval-sec <n>  Monitor refresh interval (default: 2)\n"
                      << "  -h, --help           Show this help\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\nUse -h for help\n";
            return 1;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const bool latency_probe_enabled = latency_probe_override.value_or(!monitor_only && !scan_spot_relay);
    const bool latency_summary_enabled = latency_summary_override.value_or(false);
    kimp::LatencyProbeStartOptions latency_probe_options;
    latency_probe_options.enabled = latency_probe_enabled;
    latency_probe_options.output_mode = latency_output_mode;
    latency_probe_options.summary_enabled = latency_summary_enabled;
    switch (latency_output_mode) {
        case kimp::LatencyOutputMode::CsvText:
            latency_probe_options.events_path = "trade_logs/latency_events.csv";
            break;
        case kimp::LatencyOutputMode::Binary:
            latency_probe_options.events_path = "trade_logs/latency_events.bin";
            break;
        case kimp::LatencyOutputMode::MmapBinary:
            latency_probe_options.events_path = "trade_logs/latency_events.mmapbin";
            break;
    }
    latency_probe_options.summary_path = "trade_logs/latency_summary.csv";
    kimp::LatencyProbe::instance().start(std::move(latency_probe_options));
    struct LatencyProbeGuard {
        ~LatencyProbeGuard() {
            kimp::LatencyProbe::instance().stop();
        }
    } latency_probe_guard;

    auto config_opt = load_config(config_path, !(monitor_only || scan_spot_relay));
    if (!config_opt) {
        return 1;
    }
    auto config = std::move(*config_opt);
    const bool dashboard_stream_enabled =
        dashboard_stream_override.value_or(monitor_only);

    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    if (ec) {
        std::cerr << "Failed to create logs directory: " << ec.message() << std::endl;
        return 1;
    }
    std::filesystem::create_directories("data", ec);
    if (ec) {
        std::cerr << "Failed to create data directory: " << ec.message() << std::endl;
        return 1;
    }

    // In monitor mode, logs go to file only (no console spam)
    if (!kimp::Logger::init(config.log_file, config.log_level,
                            config.log_max_size_mb, config.log_max_files,
                            8192, !monitor_mode)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }
    spdlog::info("=== KIMP Arbitrage Bot Starting ===");
    spdlog::info("Premium calculation: {} (4-8x faster batch processing)",
                 kimp::SIMDPremiumCalculator::get_simd_type());
    if (latency_probe_enabled) {
        spdlog::info("[LatencyProbe] enabled with {} ({:.2f} ns/read), output={}, summary={}",
                     kimp::latency_clock_source_name(kimp::LatencyProbe::instance().clock_source()),
                     kimp::LatencyProbe::instance().clock_cost_ns(),
                     kimp::latency_output_mode_name(kimp::LatencyProbe::instance().output_mode()),
                     kimp::LatencyProbe::instance().summary_enabled() ? "on" : "off");
    }

    if (scan_spot_relay) {
        kimp::strategy::SpotRelayScanner scanner;
        kimp::strategy::SpotRelayScanner::Options options;
        if (!scanner.run(config, options, std::cout)) {
            spdlog::error("Spot relay scan failed");
            kimp::Logger::shutdown();
            return 1;
        }
        kimp::Logger::shutdown();
        return 0;
    }

    net::io_context io_context;
    auto work_guard = net::make_work_guard(io_context);

    // Create exchanges (Bithumb: Korean spot, Bybit + OKX: spot margin short venues)
    // load_config guarantees Bithumb + Bybit entries exist; check enabled flag only
    auto& bithumb_creds = config.exchanges[kimp::Exchange::Bithumb];
    auto& bybit_creds = config.exchanges[kimp::Exchange::Bybit];

    if (!bithumb_creds.enabled || !bybit_creds.enabled) {
        spdlog::error("Both Bithumb and Bybit must be enabled for arbitrage");
        return 1;
    }

    auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
        io_context, std::move(bithumb_creds));
    auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
        io_context, std::move(bybit_creds));

    // OKX: optional foreign exchange (enabled only when credentials are configured)
    std::shared_ptr<kimp::exchange::okx::OkxExchange> okx;
    bool okx_enabled = false;
    {
        auto it = config.exchanges.find(kimp::Exchange::OKX);
        if (it != config.exchanges.end() && it->second.enabled) {
            okx = std::make_shared<kimp::exchange::okx::OkxExchange>(
                io_context, std::move(it->second));
            okx_enabled = true;
            spdlog::info("OKX exchange enabled as additional foreign venue");
        } else {
            spdlog::info("OKX exchange not configured or disabled — running with Bybit only");
        }
    }

    // Upbit: optional Korean exchange (price comparison with Bithumb)
    std::shared_ptr<kimp::exchange::upbit::UpbitExchange> upbit;
    bool upbit_enabled = false;
    bool upbit_trade_enabled = false;
    {
        auto it = config.exchanges.find(kimp::Exchange::Upbit);
        if (it != config.exchanges.end() && it->second.enabled) {
            upbit_trade_enabled = !it->second.api_key.empty() && !it->second.secret_key.empty();
            upbit = std::make_shared<kimp::exchange::upbit::UpbitExchange>(
                io_context, std::move(it->second));
            upbit_enabled = true;
            if (upbit_trade_enabled) {
                spdlog::info("Upbit exchange enabled as additional Korean venue (trading + monitoring)");
            } else {
                spdlog::info("Upbit exchange enabled as additional Korean venue (monitoring only; API keys missing)");
            }
        } else {
            spdlog::info("Upbit exchange not configured or disabled — running with Bithumb only");
        }
    }

    // Strategy engine
    kimp::strategy::ArbitrageEngine engine;
    engine.set_exchange(kimp::Exchange::Bithumb, bithumb);
    engine.set_exchange(kimp::Exchange::Bybit, bybit);
    engine.add_exchange_pair(kimp::Exchange::Bithumb, kimp::Exchange::Bybit);
    if (okx_enabled) {
        engine.set_exchange(kimp::Exchange::OKX, okx);
        engine.add_exchange_pair(kimp::Exchange::Bithumb, kimp::Exchange::OKX);
    }
    if (upbit_enabled) {
        engine.set_exchange(kimp::Exchange::Upbit, upbit);
        engine.add_exchange_pair(kimp::Exchange::Upbit, kimp::Exchange::Bybit);
        if (okx_enabled) {
            engine.add_exchange_pair(kimp::Exchange::Upbit, kimp::Exchange::OKX);
        }
        if (!upbit_trade_enabled) {
            engine.set_exchange_pair_entry_enabled(kimp::Exchange::Upbit, kimp::Exchange::Bybit, false);
            if (okx_enabled) {
                engine.set_exchange_pair_entry_enabled(kimp::Exchange::Upbit, kimp::Exchange::OKX, false);
            }
        }
    }

    // Order manager for auto-trading
    kimp::execution::OrderManager order_manager;
    order_manager.set_engine(&engine);
    order_manager.set_exchange(kimp::Exchange::Bithumb, bithumb);
    order_manager.set_exchange(kimp::Exchange::Bybit, bybit);
    if (upbit_trade_enabled) {
        order_manager.set_exchange(kimp::Exchange::Upbit, upbit);
    }
    if (okx_enabled) {
        order_manager.set_exchange(kimp::Exchange::OKX, okx);
    }

    // Position persistence callback (crash recovery)
    order_manager.set_position_update_callback([](const kimp::Position* pos) {
        if (pos) {
            save_active_position(*pos);
        } else {
            delete_active_position();
        }
    });

    kimp::execution::LifecycleExecutor<LifecycleTask> lifecycle_executor(
        [&](LifecycleTask&& task, std::size_t worker_index) {
            LoopGuard guard(g_active_loops, &engine);

            const uint64_t pickup_now_ns = kimp::LatencyProbe::instance().capture_now_ns();
            const uint64_t pickup_ns =
                task.signal.trace_start_ns > 0 && pickup_now_ns >= task.signal.trace_start_ns
                    ? (pickup_now_ns - task.signal.trace_start_ns)
                    : 0;
            kimp::LatencyProbe::instance().record_at_ns(
                task.signal.trace_id,
                task.signal.trace_symbol,
                kimp::LatencyStage::LifecyclePickedUp,
                task.signal.trace_start_ns,
                pickup_now_ns,
                static_cast<int64_t>(worker_index),
                0,
                static_cast<double>(pickup_ns),
                0.0);
            spdlog::debug("[LIFECYCLE] {} picked up by worker {} in {}us",
                          task.signal.symbol.to_string(), worker_index,
                          static_cast<long long>(pickup_ns / 1000ULL));

            auto result = order_manager.execute_spot_relay_entry(
                task.signal, task.initial_position);

            spdlog::debug("[LIFECYCLE] {} worker {} finished",
                          task.signal.symbol.to_string(), worker_index);
            if (!result.error_message.empty()) {
                spdlog::error("Lifecycle loop error: {}", result.error_message);
            }
        });

    auto try_claim_lifecycle_slot = [&engine]() -> bool {
        int current = g_active_loops.load(std::memory_order_acquire);
        while (current < kimp::TradingConfig::MAX_POSITIONS) {
            if (g_active_loops.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel)) {
                if (current + 1 >= kimp::TradingConfig::MAX_POSITIONS) {
                    engine.set_entry_suppressed(true);
                }
                return true;
            }
        }
        return false;
    };

    auto release_claimed_lifecycle_slot = [&engine]() {
        const int remaining = g_active_loops.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining < kimp::TradingConfig::MAX_POSITIONS) {
            engine.set_entry_suppressed(false);
        }
    };

    // Auto-trading callbacks (disabled in monitor-only mode)
    // Entry sequence:
    //   1. SELL borrowed spot on Bybit margin
    //   2. BUY spot on Bithumb with the same coin amount
    //
    // Exit sequence:
    //   1. BUY spot on Bybit to cover the borrowed amount
    //   2. SELL spot on Bithumb with the same coin amount
    if (!monitor_only) {
        // Trade complete callback: fired each time a full enter→exit cycle completes inside the loop
        order_manager.set_trade_complete_callback(
            [&](const kimp::Position& closed_pos, double pnl_krw, double usdt_rate) {
                double pnl_usd = usdt_rate > 0 ? pnl_krw / usdt_rate : 0.0;
                double capital_before = engine.get_current_capital();
                engine.add_realized_pnl(pnl_usd);
                double capital_after = engine.get_current_capital();
                append_trade_log(closed_pos, pnl_usd, usdt_rate, capital_before, capital_after);
                spdlog::info("[TRADE COMPLETE] {} P&L: {:.0f} KRW ({:.2f} USD), capital: ${:.2f}",
                             closed_pos.symbol.to_string(), pnl_krw, pnl_usd, capital_after);
            });

        engine.set_entry_callback([&](const kimp::ArbitrageSignal& signal) {
            auto existing = engine.get_position(signal.symbol);
            if (existing) {
                return;
            }

            if (!try_claim_lifecycle_slot()) {
                return;
            }

            if (!lifecycle_executor.enqueue(LifecycleTask{signal, std::nullopt})) {
                release_claimed_lifecycle_slot();
                spdlog::error("[LIFECYCLE] enqueue failed for {}", signal.symbol.to_string());
                return;
            }
            kimp::LatencyProbe::instance().record(
                signal.trace_id,
                signal.trace_symbol,
                kimp::LatencyStage::LifecycleEnqueued,
                signal.trace_start_ns,
                static_cast<int64_t>(lifecycle_executor.pending()),
                0,
                signal.net_edge_pct,
                signal.max_tradable_usdt_at_best);

            spdlog::debug("[LIFECYCLE] queued {} (slot {}/{}, pending={})",
                          signal.symbol.to_string(),
                          g_active_loops.load(std::memory_order_acquire),
                          kimp::TradingConfig::MAX_POSITIONS,
                          lifecycle_executor.pending());
        });

        // Exit callback not needed — lifecycle loop handles exits internally
    } else {
        spdlog::info("Monitor-only mode enabled: auto-trading callbacks are disabled");
    }

    // Relay runtime:
    // - entry is event-driven from the spot relay gate
    // - each claimed symbol runs inside a dedicated lifecycle worker
    // - exits are handled inside the lifecycle loop, not by a separate dispatcher

    // Get optimal thread configuration based on hardware
    auto thread_config = kimp::opt::ThreadConfig::optimal();
    spdlog::info("CPU cores detected: {}", std::thread::hardware_concurrency());

    // Start IO threads with CPU pinning and RT priority
    std::vector<std::thread> io_threads;
    for (int i = 0; i < config.io_threads; ++i) {
        io_threads.emplace_back([&io_context, i, &thread_config]() {
            // Apply CPU core pinning based on thread index
            int core_id = -1;
            if (i == 0) core_id = thread_config.io_bithumb_core;
            else if (i == 1) core_id = thread_config.io_bybit_core;
            else core_id = thread_config.io_bybit_core;

            if (core_id >= 0) {
                if (kimp::opt::pin_to_core(core_id)) {
                    spdlog::info("IO thread {} pinned to core {}", i, core_id);
                }
            }

            // Apply realtime priority for low-latency IO
            if (kimp::opt::set_realtime_priority()) {
                spdlog::info("IO thread {} set to realtime priority", i);
            }

            io_context.run();
        });
    }
    spdlog::info("Started {} IO threads with CPU pinning", config.io_threads);

    auto stop_io_threads = [&]() {
        work_guard.reset();
        io_context.stop();
        for (auto& t : io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    };

    bithumb->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });
    bybit->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });
    if (okx_enabled) {
        okx->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
            engine.on_ticker_update(ticker);
        });
    }
    if (upbit_enabled) {
        upbit->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
            engine.on_ticker_update(ticker);
        });
    }

    // Connect
    spdlog::info("Connecting to exchanges...");
    bithumb->connect();
    bybit->connect();
    if (okx_enabled) {
        okx->connect();
    }
    if (upbit_enabled) {
        upbit->connect();
    }

    // Wait for connections (poll instead of fixed sleep, max 10 seconds)
    spdlog::info("Waiting for connections...");
    int wait_count = 0;
    while (!g_shutdown && wait_count < 50) {
        bool all_connected = bithumb->is_connected() && bybit->is_connected();
        if (okx_enabled) all_connected = all_connected && okx->is_connected();
        if (upbit_enabled) all_connected = all_connected && upbit->is_connected();
        if (all_connected) {
            spdlog::info("All exchanges connected");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ++wait_count;
    }

    if (!bithumb->is_connected() || !bybit->is_connected()) {
        spdlog::error("Failed to connect to required exchanges (Bithumb/Bybit)");
        bithumb->disconnect();
        bybit->disconnect();
        if (okx_enabled) okx->disconnect();
        if (upbit_enabled) upbit->disconnect();
        stop_io_threads();
        kimp::Logger::shutdown();
        return 1;
    }
    if (okx_enabled && !okx->is_connected()) {
        spdlog::warn("OKX connection failed — continuing with Bybit only");
        okx_enabled = false;
        okx.reset();
    }
    if (upbit_enabled && !upbit->is_connected()) {
        spdlog::warn("Upbit connection failed — continuing with Bithumb only");
        upbit_enabled = false;
        upbit.reset();
    }

    // Find common symbols: (Bithumb ∪ Upbit) ∩ (Bybit ∪ OKX)
    // A coin is tradeable if it's on at least one Korean exchange AND one foreign exchange with margin short.
    auto bithumb_symbols = bithumb->get_available_symbols();
    auto bybit_symbols = bybit->get_available_symbols();

    std::unordered_set<std::string> bithumb_bases;
    for (const auto& s : bithumb_symbols) {
        bithumb_bases.insert(std::string(s.get_base()));
    }

    std::unordered_set<std::string> upbit_bases;
    if (upbit_enabled) {
        auto upbit_symbols = upbit->get_available_symbols();
        for (const auto& s : upbit_symbols) {
            upbit_bases.insert(std::string(s.get_base()));
        }
        spdlog::info("Upbit KRW markets: {}", upbit_bases.size());
    }

    // Union of Korean exchanges
    std::unordered_set<std::string> korean_bases = bithumb_bases;
    for (const auto& b : upbit_bases) {
        korean_bases.insert(b);
    }

    std::unordered_set<std::string> foreign_bases;
    for (const auto& s : bybit_symbols) {
        foreign_bases.insert(std::string(s.get_base()));
    }

    std::unordered_set<std::string> okx_bases;
    if (okx_enabled) {
        auto okx_symbols = okx->get_available_symbols();
        for (const auto& s : okx_symbols) {
            foreign_bases.insert(std::string(s.get_base()));
            okx_bases.insert(std::string(s.get_base()));
        }
        spdlog::info("OKX margin-shortable symbols: {}", okx_bases.size());
    }

    std::unordered_set<std::string> bybit_bases;
    for (const auto& s : bybit_symbols) {
        bybit_bases.insert(std::string(s.get_base()));
    }

    // Common = (Bithumb ∪ Upbit) ∩ (Bybit ∪ OKX)
    std::vector<kimp::SymbolId> common_symbols;
    for (const auto& base : korean_bases) {
        if (foreign_bases.count(base)) {
            common_symbols.emplace_back(base, "KRW");
            engine.add_symbol(common_symbols.back());
        }
    }
    // Sort for deterministic output
    std::sort(common_symbols.begin(), common_symbols.end(),
        [](const auto& a, const auto& b) { return a.to_string() < b.to_string(); });

    spdlog::info("Found {} common symbols ((Bithumb ∪ Upbit) ∩ (Bybit ∪ OKX))", common_symbols.size());
    {
        size_t bi_only = 0, up_only = 0, both_korean = 0;
        size_t by_only = 0, ok_only = 0, both_foreign = 0;
        for (const auto& s : common_symbols) {
            std::string base(s.get_base());
            bool on_bi = bithumb_bases.count(base) > 0;
            bool on_up = upbit_bases.count(base) > 0;
            if (on_bi && on_up) ++both_korean;
            else if (on_bi) ++bi_only;
            else if (on_up) ++up_only;

            bool on_by = bybit_bases.count(base) > 0;
            bool on_ok = okx_bases.count(base) > 0;
            if (on_by && on_ok) ++both_foreign;
            else if (on_by) ++by_only;
            else if (on_ok) ++ok_only;
        }
        spdlog::info("  Korean: Bi-only:{} Up-only:{} Both:{}", bi_only, up_only, both_korean);
        spdlog::info("  Foreign: By-only:{} Ok-only:{} Both:{}", by_only, ok_only, both_foreign);
    }

    if (common_symbols.empty()) {
        spdlog::error("No common symbols found between Korean and foreign exchanges");
        bithumb->disconnect();
        bybit->disconnect();
        if (okx_enabled) okx->disconnect();
        if (upbit_enabled) upbit->disconnect();
        stop_io_threads();
        kimp::Logger::shutdown();
        return 1;
    }

    // Create lookup set for fast matching
    std::unordered_set<std::string> common_bases;
    for (const auto& s : common_symbols) {
        common_bases.insert(std::string(s.get_base()));
    }

    if (!monitor_only) {
        // Prepare Bybit spot margin account once at startup (avoid first-trade setup latency)
        if (!order_manager.prepare_bybit_shorting(common_symbols)) {
            spdlog::error("Bybit spot margin setup failed");
            bithumb->disconnect();
            bybit->disconnect();
            if (okx_enabled) okx->disconnect();
            stop_io_threads();
            kimp::Logger::shutdown();
            return 1;
        }

        // Prepare OKX spot margin account (optional — non-fatal if it fails)
        if (okx_enabled) {
            if (!order_manager.prepare_okx_shorting(common_symbols)) {
                spdlog::warn("OKX spot margin setup failed — OKX will not be used for entries");
                engine.set_exchange_pair_entry_enabled(kimp::Exchange::Bithumb, kimp::Exchange::OKX, false);
                engine.set_exchange_pair_entry_enabled(kimp::Exchange::Upbit, kimp::Exchange::OKX, false);
            }
        }

        // Build external position blacklist (prevents trading coins with existing manual positions)
        // Exclude bot-managed positions (from saved position file) so they don't get blacklisted
        std::unordered_set<kimp::SymbolId> bot_managed_symbols;
        {
            auto saved_pos = load_active_position();
            if (saved_pos) {
                bot_managed_symbols.insert(saved_pos->symbol);
                spdlog::info("[BLACKLIST] Excluding bot-managed position: {}", saved_pos->symbol.to_string());
            }
        }
        order_manager.refresh_external_positions(common_symbols, bot_managed_symbols);
    } else {
        spdlog::info("Monitor-only mode: skipping spot-margin setup and external position blacklist");
    }

    std::shared_ptr<kimp::network::WsBroadcastServer> ws_server;
    std::atomic<bool> broadcast_running{false};
    std::atomic<int> broadcast_count{0};
    std::thread broadcast_thread;

    if (dashboard_stream_enabled) {
        // Start async JSON exporter FIRST (before any price loading or trading)
        // 200ms interval for file-based updates (fallback)
        engine.start_async_exporter("data/premiums.json", std::chrono::milliseconds(200));
        spdlog::info("Dashboard stream enabled: JSON exporter started (200ms interval)");

        // =========================================================================
        // WebSocket Broadcast Server for REAL-TIME dashboard updates (<10ms latency)
        // =========================================================================
        ws_server = std::make_shared<kimp::network::WsBroadcastServer>(io_context, 8765);
        ws_server->start();

        // =========================================================================
        // DEDICATED BROADCAST THREAD - Exactly 50ms interval for perfect latency
        // =========================================================================
        broadcast_running = true;
        broadcast_thread = std::thread([&]() {
            spdlog::info("[WS-Broadcast] Dedicated broadcast thread started (50ms interval)");

            while (broadcast_running && !g_shutdown) {
                auto start = std::chrono::steady_clock::now();

                auto conn_count = ws_server->connection_count();
                // Only broadcast if clients connected
                if (conn_count > 0) {
                    auto premiums = engine.get_all_premiums();
                    if (!premiums.empty()) {
                        // Log every 100 broadcasts
                        if (++broadcast_count % 100 == 1) {
                            spdlog::info("[WS-Broadcast] Sending to {} clients, {} premiums", conn_count, premiums.size());
                        }
                        // Build JSON quickly using fmt
                        std::string json;
                        json.reserve(premiums.size() * 200 + 500);
                        json = "{\"type\":\"premiums\",\"ts\":";
                        json += std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                        json += ",\"data\":[";

                        bool first = true;
                        for (const auto& p : premiums) {
                            if (!first) json += ",";
                            first = false;
                            json += fmt::format(
                                "{{\"s\":\"{}/{}\",\"kb\":{},\"ka\":{},\"fb\":{:.8f},\"fa\":{:.8f},\"kp\":{},\"fp\":{:.8f},\"r\":{:.4f},\"ep\":{:.4f},\"xp\":{:.4f},\"sp\":{:.4f},\"pm\":{:.4f},\"sg\":{}}}",
                                p.symbol.get_base(), p.symbol.get_quote(),
                                kimp::format::format_decimal_trimmed(p.korean_bid),
                                kimp::format::format_decimal_trimmed(p.korean_ask),
                                p.foreign_bid, p.foreign_ask,
                                kimp::format::format_decimal_trimmed(p.korean_price),
                                p.foreign_price, p.usdt_rate,
                                p.entry_premium, p.exit_premium, p.premium_spread, p.premium,
                                p.entry_signal ? "1" : (p.exit_signal ? "2" : "0")
                            );
                        }
                        json += "]}";

                        ws_server->broadcast(std::move(json));
                    }
                }

                // Sleep exactly 50ms (minus processing time for precision)
                auto elapsed = std::chrono::steady_clock::now() - start;
                auto sleep_time = std::chrono::milliseconds(50) - elapsed;
                if (sleep_time > std::chrono::milliseconds(0)) {
                    std::this_thread::sleep_for(sleep_time);
                }
            }

            spdlog::info("[WS-Broadcast] Broadcast thread stopped");
        });
    } else {
        spdlog::info("Dashboard stream disabled for this run: execution path stays isolated from JSON export/local WS broadcast");
    }

    // =========================================================================
    // STEP 12: WebSocket Subscriptions (real-time streaming)
    // =========================================================================

    // Bithumb: subscribe only symbols that exist on Bithumb
    std::vector<kimp::SymbolId> bithumb_subs;
    for (const auto& s : common_symbols) {
        if (bithumb_bases.count(std::string(s.get_base()))) {
            bithumb_subs.push_back(s);
        }
    }
    bithumb_subs.emplace_back("USDT", "KRW");  // For exchange rate
    bithumb->subscribe_ticker(bithumb_subs);
    spdlog::info("Subscribed to {} Bithumb tickers (including USDT/KRW)", bithumb_subs.size());

    // Fetch orderbook snapshots and subscribe to orderbookdepth for real bid/ask
    bithumb->fetch_all_orderbook_snapshots(bithumb_subs);
    bithumb->subscribe_orderbook(bithumb_subs);
    spdlog::info("Bithumb orderbook snapshots primed; live cache will be driven by WebSocket only");

    // Fetch per-coin per-network withdrawal fees from Bithumb (public API, no auth)
    {
        auto withdraw_fees = bithumb->fetch_withdrawal_fees();
        if (!withdraw_fees.empty()) {
            auto& pc = engine.get_price_cache();
            for (auto& [coin, net_fees] : withdraw_fees) {
                std::vector<kimp::strategy::PriceCache::NetworkFee> converted;
                converted.reserve(net_fees.size());
                for (auto& nf : net_fees) {
                    converted.push_back({std::move(nf.network), nf.fee_coins});
                }
                pc.set_withdraw_network_fees(kimp::Exchange::Bithumb, coin, std::move(converted));
            }
            spdlog::info("Loaded {} Bithumb withdrawal fees (per-network) into price cache", withdraw_fees.size());
        } else {
            spdlog::warn("Failed to load Bithumb withdrawal fees — NetKRW will not include transfer costs");
        }
    }

    // Upbit: subscribe only symbols that exist on Upbit
    std::vector<kimp::SymbolId> upbit_subs;
    if (upbit_enabled) {
        for (const auto& s : common_symbols) {
            if (upbit_bases.count(std::string(s.get_base()))) {
                upbit_subs.push_back(s);
            }
        }
        upbit_subs.emplace_back("USDT", "KRW");  // For Upbit's own USDT/KRW rate
        upbit->subscribe_ticker(upbit_subs);
        upbit->subscribe_orderbook(upbit_subs);
        spdlog::info("Subscribed to {} Upbit symbols (orderbook + ticker)", upbit_subs.size());
    }

    std::vector<kimp::SymbolId> bybit_subs;
    for (const auto& s : common_symbols) {
        std::string base(s.get_base());
        if (bybit_bases.count(base)) {
            bybit_subs.emplace_back(base, "USDT");
        }
    }
    bybit->subscribe_orderbook(bybit_subs);
    spdlog::info("Subscribed to {} Bybit spot symbols (orderbook-only BBO path)", bybit_subs.size());

    if (okx_enabled) {
        std::vector<kimp::SymbolId> okx_subs;
        for (const auto& s : common_symbols) {
            std::string base(s.get_base());
            if (okx_bases.count(base)) {
                okx_subs.emplace_back(base, "USDT");
            }
        }
        okx->subscribe_orderbook(okx_subs);
        spdlog::info("Subscribed to {} OKX spot symbols (bbo-tbt 10ms path)", okx_subs.size());
    }

    std::mutex transfer_refresh_mutex;
    auto refresh_transfer_routes = [&](std::string_view reason) {
        std::lock_guard guard(transfer_refresh_mutex);
        auto& pc = engine.get_price_cache();
        bool updated_any = false;

        auto bithumb_fees = bithumb->fetch_withdrawal_fees();
        if (!bithumb_fees.empty()) {
            pc.clear_withdraw_network_fees(kimp::Exchange::Bithumb);
            for (auto& [coin, net_fees] : bithumb_fees) {
                std::vector<kimp::strategy::PriceCache::NetworkFee> converted;
                converted.reserve(net_fees.size());
                for (auto& nf : net_fees) {
                    converted.push_back({std::move(nf.network), nf.fee_coins});
                }
                pc.set_withdraw_network_fees(kimp::Exchange::Bithumb, coin, std::move(converted));
            }
            updated_any = true;
        } else {
            spdlog::warn("[TransferRoutes:{}] Failed to refresh Bithumb withdrawal fees", reason);
        }

        auto bithumb_statuses = bithumb->fetch_asset_statuses();
        if (!bithumb_statuses.empty()) {
            pc.clear_korean_withdraw_status(kimp::Exchange::Bithumb);
            for (const auto& [coin, status] : bithumb_statuses) {
                pc.set_korean_withdraw_enabled(kimp::Exchange::Bithumb, coin, status.withdraw_enabled);
            }
            updated_any = true;
        } else {
            spdlog::warn("[TransferRoutes:{}] Failed to refresh Bithumb asset statuses", reason);
        }

        if (upbit_enabled) {
            std::vector<std::string> upbit_coin_list;
            upbit_coin_list.reserve(upbit_subs.size());
            for (const auto& s : upbit_subs) {
                std::string base(s.get_base());
                if (base != "USDT") upbit_coin_list.push_back(base);
            }
            auto upbit_fees = upbit->fetch_withdrawal_fees(upbit_coin_list);
            if (!upbit_fees.empty()) {
                pc.clear_withdraw_network_fees(kimp::Exchange::Upbit);
                for (auto& [coin, net_fees] : upbit_fees) {
                    std::vector<kimp::strategy::PriceCache::NetworkFee> converted;
                    converted.reserve(net_fees.size());
                    for (auto& nf : net_fees) {
                        converted.push_back({std::move(nf.network), nf.fee_coins});
                    }
                    pc.set_withdraw_network_fees(kimp::Exchange::Upbit, coin, std::move(converted));
                }
                updated_any = true;
            } else {
                spdlog::warn("[TransferRoutes:{}] Failed to refresh Upbit withdrawal fees", reason);
            }
        }

        auto bybit_nets = bybit->fetch_deposit_networks();
        if (!bybit_nets.empty()) {
            pc.clear_foreign_deposit_networks(kimp::Exchange::Bybit);
            for (auto& [coin, nets] : bybit_nets) {
                pc.set_foreign_deposit_networks(kimp::Exchange::Bybit, coin, std::move(nets));
            }
            updated_any = true;
        } else {
            spdlog::warn("[TransferRoutes:{}] Failed to refresh Bybit deposit networks", reason);
        }

        if (okx_enabled) {
            auto okx_nets = okx->fetch_deposit_networks();
            if (!okx_nets.empty()) {
                pc.clear_foreign_deposit_networks(kimp::Exchange::OKX);
                for (auto& [coin, nets] : okx_nets) {
                    pc.set_foreign_deposit_networks(kimp::Exchange::OKX, coin, std::move(nets));
                }
                updated_any = true;
            } else {
                spdlog::warn("[TransferRoutes:{}] Failed to refresh OKX deposit networks", reason);
            }
        }

        if (updated_any) {
            pc.finalize_withdraw_fees();
            engine.refresh_entry_filters();
            spdlog::info("[TransferRoutes:{}] available routes={} fee entries={}",
                         reason,
                         pc.available_transfer_route_count(),
                         pc.precomputed_fee_count());
        }
    };

    refresh_transfer_routes("startup");

    // =========================================================================
    // STEP 13: Warm-up
    // Market data updates are WebSocket-only after subscriptions.
    // =========================================================================
    spdlog::info("Market data path: WebSocket-only after startup subscriptions");

    std::thread warmup_thread([&engine, common_symbols, okx_enabled, upbit_enabled,
                               &bithumb_bases, &upbit_bases]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (g_shutdown) {
            return;
        }

        size_t bithumb_ready = 0;
        size_t upbit_ready = 0;
        size_t bybit_ready = 0;
        size_t okx_ready = 0;
        size_t any_korean_ready = 0;
        size_t any_foreign_ready = 0;
        std::vector<std::string> missing_korean;
        std::vector<std::string> missing_foreign;
        missing_korean.reserve(5);
        missing_foreign.reserve(5);
        auto join_samples = [](const std::vector<std::string>& samples) {
            std::string joined;
            for (size_t i = 0; i < samples.size(); ++i) {
                if (i > 0) {
                    joined += ", ";
                }
                joined += samples[i];
            }
            return joined;
        };

        for (const auto& symbol : common_symbols) {
            std::string base(symbol.get_base());
            const kimp::SymbolId foreign_symbol(base, "USDT");

            bool bi_ok = false;
            if (bithumb_bases.count(base)) {
                const auto p = engine.get_price_cache().get_price(kimp::Exchange::Bithumb, symbol);
                bi_ok = p.valid && p.ask > 0.0;
                if (bi_ok) ++bithumb_ready;
            }

            bool up_ok = false;
            if (upbit_enabled && upbit_bases.count(base)) {
                const auto p = engine.get_price_cache().get_price(kimp::Exchange::Upbit, symbol);
                up_ok = p.valid && p.ask > 0.0;
                if (up_ok) ++upbit_ready;
            }

            if (bi_ok || up_ok) {
                ++any_korean_ready;
            } else if (missing_korean.size() < 5) {
                missing_korean.push_back(symbol.to_string());
            }

            const auto bybit_price = engine.get_price_cache().get_price(kimp::Exchange::Bybit, foreign_symbol);
            const bool bybit_ok = bybit_price.valid && bybit_price.bid > 0.0;
            if (bybit_ok) ++bybit_ready;

            bool okx_ok = false;
            if (okx_enabled) {
                const auto okx_price = engine.get_price_cache().get_price(kimp::Exchange::OKX, foreign_symbol);
                okx_ok = okx_price.valid && okx_price.bid > 0.0;
                if (okx_ok) ++okx_ready;
            }

            if (bybit_ok || okx_ok) {
                ++any_foreign_ready;
            } else if (missing_foreign.size() < 5) {
                missing_foreign.push_back(foreign_symbol.to_string());
            }
        }

        const double usdt_bi = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Bithumb);
        const double usdt_up = upbit_enabled ? engine.get_price_cache().get_usdt_krw(kimp::Exchange::Upbit) : 0.0;
        spdlog::info("[MarketData] 5s snapshot: bi {}/{} | up {}/{} | by {}/{} | ok {}/{} | korean {}/{} | foreign {}/{} | usdt bi:{:.2f} up:{:.2f} | cache {}",
                     bithumb_ready, common_symbols.size(),
                     upbit_ready, common_symbols.size(),
                     bybit_ready, common_symbols.size(),
                     okx_ready, common_symbols.size(),
                     any_korean_ready, common_symbols.size(),
                     any_foreign_ready, common_symbols.size(),
                     usdt_bi, usdt_up,
                     engine.get_price_cache().size());

        if (any_korean_ready != common_symbols.size() || any_foreign_ready != common_symbols.size() || usdt_bi <= 0.0) {
            spdlog::warn("[MarketData] sample missing korean=[{}] foreign=[{}]",
                         join_samples(missing_korean),
                         join_samples(missing_foreign));
        }
    });

    // =========================================================================
    // STARTUP RECOVERY: Check for existing positions (trade mode only)
    // =========================================================================
    std::optional<kimp::Position> recovered_position;  // For launching lifecycle loop after engine.start()
    if (!monitor_only) {
        bool resumed_position = false;
        auto normalize_input = [](std::string value) {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            for (auto& c : value) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return value;
        };
        auto is_yes = [&](const std::string& raw) {
            auto v = normalize_input(raw);
            return v == "Y" || v == "YES";
        };

        if (!g_shutdown && !resumed_position) {
            std::cout << "\n시작 시 기존 포지션 복구를 진행할까요? (y/n): ";
            std::cout.flush();

            std::string recover_input;
            if (read_stdin_line(recover_input) && is_yes(recover_input)) {
                struct RecoveryCandidate {
                    kimp::Position pos;
                    double spot_balance{0.0};
                    double short_amount{0.0};
                    double matched_amount{0.0};
                    double matched_usd{0.0};
                    bool from_saved_file{false};
                };

                std::vector<RecoveryCandidate> candidates;
                std::unordered_map<std::string, kimp::Position> bybit_short_by_coin;

                // Query Bybit short positions once, then scan all candidates from that set.
                auto bybit_positions = bybit->get_short_positions();
                for (const auto& p : bybit_positions) {
                    std::string coin = std::string(p.symbol.get_base());
                    if (coin.empty()) continue;
                    if (!common_bases.count(coin)) continue;
                    if (p.foreign_amount <= 0.0001) continue;

                    auto it = bybit_short_by_coin.find(coin);
                    if (it == bybit_short_by_coin.end() || p.foreign_amount > it->second.foreign_amount) {
                        bybit_short_by_coin[coin] = p;
                    }
                }

                if (!bybit_short_by_coin.empty()) {
                    std::cout << fmt::format("\n복구 스캔 시작: Bybit 숏 {}개 코인 확인\n", bybit_short_by_coin.size());
                }

                // Helper: scan short positions from a foreign exchange and add recovery candidates
                auto scan_foreign_shorts = [&](const std::unordered_map<std::string, kimp::Position>& short_by_coin,
                                               kimp::Exchange foreign_ex) {
                    for (const auto& [coin, short_pos] : short_by_coin) {
                        kimp::SymbolId krw_symbol(coin, "KRW");
                        kimp::SymbolId usdt_symbol(coin, "USDT");

                        double spot_balance = bithumb->get_balance(coin);
                        double short_amount = short_pos.foreign_amount;
                        if (spot_balance <= 0.0001 || short_amount <= 0.0001) continue;

                        double amount = std::min(spot_balance, short_amount);

                        auto korean_price = engine.get_price_cache().get_price(
                            kimp::Exchange::Bithumb, krw_symbol);
                        double korean_entry = 0.0;
                        if (korean_price.valid) {
                            if (korean_price.ask > 0) korean_entry = korean_price.ask;
                            else if (korean_price.last > 0) korean_entry = korean_price.last;
                        }

                        auto foreign_price = engine.get_price_cache().get_price(foreign_ex, usdt_symbol);
                        double foreign_fallback = 0.0;
                        if (foreign_price.valid) {
                            if (foreign_price.bid > 0) foreign_fallback = foreign_price.bid;
                            else if (foreign_price.last > 0) foreign_fallback = foreign_price.last;
                        }
                        double effective_foreign_entry =
                            (short_pos.foreign_entry_price > 0.0) ? short_pos.foreign_entry_price : foreign_fallback;

                        double usdt_rate = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Bithumb);
                        double recovered_entry_pm = 0.0;
                        if (korean_entry > 0.0 && effective_foreign_entry > 0.0 && usdt_rate > 0.0) {
                            double foreign_krw = effective_foreign_entry * usdt_rate;
                            recovered_entry_pm = ((korean_entry - foreign_krw) / foreign_krw) * 100.0;
                        }

                        kimp::Position pos;
                        pos.symbol = krw_symbol;
                        pos.korean_exchange = kimp::Exchange::Bithumb;
                        pos.foreign_exchange = foreign_ex;
                        pos.entry_time = std::chrono::system_clock::now();
                        pos.entry_premium = recovered_entry_pm;
                        pos.position_size_usd = amount * (effective_foreign_entry > 0.0 ? effective_foreign_entry : 0.0);
                        pos.korean_amount = amount;
                        pos.foreign_amount = amount;
                        pos.korean_entry_price = korean_entry;
                        pos.foreign_entry_price = effective_foreign_entry;
                        pos.is_active = true;

                        RecoveryCandidate c;
                        c.pos = pos;
                        c.spot_balance = spot_balance;
                        c.short_amount = short_amount;
                        c.matched_amount = amount;
                        c.matched_usd = pos.position_size_usd;
                        if (c.matched_usd <= 0.0 && foreign_fallback > 0.0) {
                            c.matched_usd = amount * foreign_fallback;
                        }
                        candidates.push_back(c);
                    }
                };

                scan_foreign_shorts(bybit_short_by_coin, kimp::Exchange::Bybit);

                // Also scan OKX short positions
                if (okx_enabled) {
                    std::unordered_map<std::string, kimp::Position> okx_short_by_coin;
                    auto okx_positions = okx->get_short_positions();
                    for (const auto& p : okx_positions) {
                        std::string coin = std::string(p.symbol.get_base());
                        if (coin.empty()) continue;
                        if (!common_bases.count(coin)) continue;
                        if (p.foreign_amount <= 0.0001) continue;
                        auto it = okx_short_by_coin.find(coin);
                        if (it == okx_short_by_coin.end() || p.foreign_amount > it->second.foreign_amount) {
                            okx_short_by_coin[coin] = p;
                        }
                    }
                    if (!okx_short_by_coin.empty()) {
                        std::cout << fmt::format("OKX 숏 {}개 코인 확인\n", okx_short_by_coin.size());
                    }
                    scan_foreign_shorts(okx_short_by_coin, kimp::Exchange::OKX);
                }

                // Include saved position as fallback candidate if it wasn't discovered via live scan.
                auto loaded_pos = load_active_position();
                if (loaded_pos) {
                    bool already_present = std::any_of(
                        candidates.begin(), candidates.end(),
                        [&](const RecoveryCandidate& c) { return c.pos.symbol == loaded_pos->symbol; });

                    if (!already_present) {
                        RecoveryCandidate c;
                        c.pos = *loaded_pos;
                        c.spot_balance = loaded_pos->korean_amount;
                        c.short_amount = loaded_pos->foreign_amount;
                        c.matched_amount = std::min(c.spot_balance, c.short_amount);
                        c.matched_usd = loaded_pos->position_size_usd;
                        if (c.matched_usd <= 0.0) {
                            c.matched_usd = loaded_pos->korean_amount * loaded_pos->foreign_entry_price;
                        }
                        c.from_saved_file = true;
                        candidates.push_back(c);
                    }
                }

                if (!candidates.empty()) {
                    // Sort: bot-managed (from saved file or matching saved symbol) first, then by USD
                    std::string saved_symbol;
                    if (loaded_pos) saved_symbol = loaded_pos->symbol.to_string();

                    // Mark candidates that match the saved position and restore original entry data
                    for (auto& c : candidates) {
                        if (!saved_symbol.empty() && c.pos.symbol.to_string() == saved_symbol) {
                            c.from_saved_file = true;
                            // Preserve original entry prices/premium from saved file
                            // (live scan recalculates with current prices which corrupts dynamic exit threshold)
                            c.pos.entry_premium = loaded_pos->entry_premium;
                            c.pos.korean_entry_price = loaded_pos->korean_entry_price;
                            c.pos.foreign_entry_price = loaded_pos->foreign_entry_price;
                            c.pos.entry_time = loaded_pos->entry_time;
                            c.pos.position_size_usd = loaded_pos->position_size_usd;
                            c.pos.realized_pnl_krw = loaded_pos->realized_pnl_krw;
                        }
                    }

                    std::sort(candidates.begin(), candidates.end(),
                        [](const RecoveryCandidate& a, const RecoveryCandidate& b) {
                            // Bot-managed positions first
                            if (a.from_saved_file != b.from_saved_file) return a.from_saved_file;
                            if (a.matched_usd != b.matched_usd) return a.matched_usd > b.matched_usd;
                            return a.pos.entry_premium < b.pos.entry_premium;
                        });

                    std::cout << "\n";
                    std::cout << "=========================================\n";
                    std::cout << "  RECOVERY CANDIDATES\n";
                    std::cout << "=========================================\n";
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        const auto& c = candidates[i];
                        std::cout << fmt::format("  [{}] {} — {:.8f} coins (${:.2f}) pm:{:.2f}%{}\n",
                                                  i + 1,
                                                  c.pos.symbol.to_string(),
                                                  c.matched_amount,
                                                  c.matched_usd,
                                                  c.pos.entry_premium,
                                                  c.from_saved_file ? " ★봇포지션" : "");
                    }
                    std::cout << "=========================================\n";
                    std::cout << "\n복구할 번호를 입력하세요 (0=건너뛰기): ";
                    std::cout.flush();

                    std::string choice_str;
                    int choice = 0;
                    if (read_stdin_line(choice_str)) {
                        try { choice = std::stoi(normalize_input(choice_str)); } catch (...) {}
                    }

                    if (choice >= 1 && choice <= static_cast<int>(candidates.size())) {
                        const auto& selected = candidates[choice - 1];
                        engine.open_position(selected.pos);
                        resumed_position = true;
                        recovered_position = selected.pos;
                        save_active_position(selected.pos);
                        spdlog::info("[RECOVERY] Restored position: {} ({:.8f} coins, ${:.2f})",
                                     selected.pos.symbol.to_string(), selected.matched_amount, selected.matched_usd);
                    } else {
                        spdlog::info("[RECOVERY] Candidate declined, starting fresh");
                    }
                } else {
                    std::cout << "\n복구 가능한 포지션이 없습니다. 새로 시작합니다.\n";
                }
            } else {
                spdlog::info("[RECOVERY] Startup recovery skipped by user input");
            }
        }
    } else {
        spdlog::info("Monitor-only mode: skipping position recovery prompts");
    }

    if (g_shutdown) {
        spdlog::info("Shutdown during startup, exiting");
        // Fall through to shutdown
    }

    // =========================================================================
    // MAX_POSITIONS configuration (1~4)
    // =========================================================================
    if (!g_shutdown && !monitor_only) {
        std::cout << fmt::format("\n최대 동시 포지션 수 (1~4, 현재: {}, Enter=유지): ",
                                  kimp::TradingConfig::MAX_POSITIONS);
        std::cout.flush();

        std::string pos_input;
        if (read_stdin_line(pos_input)) {
            // Trim
            while (!pos_input.empty() && std::isspace(static_cast<unsigned char>(pos_input.back()))) pos_input.pop_back();
            while (!pos_input.empty() && std::isspace(static_cast<unsigned char>(pos_input.front()))) pos_input.erase(pos_input.begin());

            if (!pos_input.empty()) {
                try {
                    int val = std::stoi(pos_input);
                    if (val >= 1 && val <= 4) {
                        kimp::TradingConfig::MAX_POSITIONS = val;
                        spdlog::info("MAX_POSITIONS set to {}", val);
                    } else {
                        std::cout << "  범위 초과, 기본값 " << kimp::TradingConfig::MAX_POSITIONS << " 유지\n";
                    }
                } catch (...) {
                    std::cout << "  잘못된 입력, 기본값 " << kimp::TradingConfig::MAX_POSITIONS << " 유지\n";
                }
            }
        }
    }

    std::thread transfer_refresh_thread;
    if (!g_shutdown) {
        if (!monitor_only) {
            kimp::execution::LifecycleExecutorOptions executor_options;
            executor_options.worker_count =
                static_cast<std::size_t>(std::max(1, kimp::TradingConfig::MAX_POSITIONS));
            executor_options.push_spin_count = 512;
            executor_options.empty_spin_count = 4096;
            executor_options.idle_wait = std::chrono::microseconds(150);

            lifecycle_executor.start(executor_options, [thread_config](std::size_t worker_index) {
                int core_id = thread_config.execution_core;
                const int total_cores = static_cast<int>(std::thread::hardware_concurrency());
                if (core_id >= 0 && total_cores > 0) {
                    core_id = std::min(core_id + static_cast<int>(worker_index), total_cores - 1);
                    if (kimp::opt::pin_to_core(core_id)) {
                        spdlog::info("Lifecycle worker {} pinned to core {}", worker_index, core_id);
                    }
                }
                if (kimp::opt::set_realtime_priority()) {
                    spdlog::info("Lifecycle worker {} set to realtime priority", worker_index);
                }
            });
            spdlog::info("Started {} low-latency lifecycle worker(s)",
                         lifecycle_executor.worker_count());
        }

        // Start engine
        engine.start();

        transfer_refresh_thread = std::thread([&]() {
            constexpr auto refresh_interval = std::chrono::minutes(30);
            while (!g_shutdown) {
                const auto started = std::chrono::steady_clock::now();
                while (!g_shutdown &&
                       (std::chrono::steady_clock::now() - started) < refresh_interval) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (g_shutdown) {
                    break;
                }
                refresh_transfer_routes("periodic");
            }
        });

        if (monitor_only) {
            spdlog::info("=== Bot Running (MONITOR-ONLY, Auto-Trading DISABLED) ===");
            spdlog::info("Monitor interval: {}s", monitor_interval_sec);
            spdlog::info("Press Ctrl+C to stop");
        } else {
            spdlog::info("=== Bot Running (Auto-Trading ENABLED) ===");
            spdlog::info("Mode: relay gate (70 USDT, 1-tick only) | Max positions: {}",
                         kimp::TradingConfig::MAX_POSITIONS);
            spdlog::info("Entry: negative EntryPM (KR ask < Foreign bid*FX) + projected NetKRW >= {:.0f} + both venues can fill {:.2f} USDT at top-of-book",
                         kimp::TradingConfig::MIN_ENTRY_NET_PROFIT_KRW,
                         kimp::TradingConfig::TARGET_ENTRY_USDT);
            spdlog::info("Entry fee model: Bithumb {}x + Bybit {}x = {:.2f}%",
                         kimp::TradingConfig::BITHUMB_FEE_EVENTS,
                         kimp::TradingConfig::BYBIT_FEE_EVENTS,
                         kimp::TradingConfig::ENTRY_TOTAL_FEE_PCT);
            spdlog::info("Exit: dynamic max(entry_pm + {:.2f}% fees + {:.2f}% profit, +{:.2f}% floor)",
                         kimp::TradingConfig::ENTRY_TOTAL_FEE_PCT,
                         kimp::TradingConfig::MIN_NET_PROFIT_PCT,
                         kimp::TradingConfig::EXIT_PREMIUM_THRESHOLD);
            spdlog::info("Press Ctrl+C to stop");
        }

        // Launch lifecycle loop for recovered position (if any)
        if (!monitor_only && recovered_position.has_value()) {
            const auto& rpos = *recovered_position;
            double actual_usd = rpos.foreign_amount * rpos.foreign_entry_price;
            spdlog::info("[RECOVERY] Launching lifecycle loop for {} (${:.2f}/${:.2f} filled)",
                         rpos.symbol.to_string(), actual_usd, rpos.position_size_usd);

            if (!try_claim_lifecycle_slot()) {
                spdlog::error("[RECOVERY] No lifecycle slot available for {}", rpos.symbol.to_string());
            } else {
                kimp::ArbitrageSignal signal;
                signal.trace_id = kimp::LatencyProbe::instance().next_trace_id();
                signal.trace_start_ns = kimp::LatencyProbe::instance().capture_now_ns();
                signal.trace_symbol = kimp::LatencyProbe::format_symbol_fast(rpos.symbol);
                signal.symbol = rpos.symbol;
                signal.korean_exchange = rpos.korean_exchange;
                signal.foreign_exchange = rpos.foreign_exchange;
                signal.premium = rpos.entry_premium;
                signal.korean_ask = rpos.korean_entry_price;
                signal.foreign_bid = rpos.foreign_entry_price;
                signal.usdt_krw_rate = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Bithumb);

                if (!lifecycle_executor.enqueue(LifecycleTask{signal, rpos})) {
                    release_claimed_lifecycle_slot();
                    spdlog::error("[RECOVERY] enqueue failed for {}", rpos.symbol.to_string());
                } else {
                    kimp::LatencyProbe::instance().record(
                        signal.trace_id,
                        signal.trace_symbol,
                        kimp::LatencyStage::LifecycleEnqueued,
                        signal.trace_start_ns,
                        static_cast<int64_t>(lifecycle_executor.pending()),
                        1,
                        signal.premium,
                        0.0);
                    spdlog::info("[RECOVERY] Queued lifecycle loop for {}",
                                 rpos.symbol.to_string());
                }
            }
        }

        // Main loop with console output
        LiveMonitorGuard live_monitor_guard(monitor_mode);
        const auto monitor_interval = std::chrono::seconds(monitor_interval_sec);
        auto next_monitor_due = std::chrono::steady_clock::now();
        while (!g_shutdown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Full monitor: all symbols + both-side prices + entry/exit premiums
            const auto now = std::chrono::steady_clock::now();
            if (monitor_mode && now >= next_monitor_due) {
                next_monitor_due = now + monitor_interval;
                auto premiums = engine.get_all_premiums();
                std::sort(premiums.begin(), premiums.end(),
                    [](const auto& a, const auto& b) { return a.net_edge_pct > b.net_edge_pct; });
                std::vector<kimp::strategy::ArbitrageEngine::PremiumInfo> visible_premiums;
                visible_premiums.reserve(premiums.size());
                for (const auto& p : premiums) {
                    const bool displayable =
                        p.entry_premium < 0.0 &&
                        p.net_edge_pct > kimp::TradingConfig::MIN_NET_EDGE_PCT &&
                        kimp::TradingConfig::meets_entry_profit_floor(p.net_profit_krw);
                    if (displayable) {
                        visible_premiums.push_back(p);
                    }
                }

                    if (live_monitor_guard.active()) {
                        live_monitor_guard.clear_frame();
                    } else if (::isatty(STDOUT_FILENO) == 1) {
                        std::cout << "\033[2J\033[H";
                    }
                    double bi_rate = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Bithumb);
                    double up_rate = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Upbit);
                    std::string rate_str;
                    if (up_rate > 0.0) {
                        rate_str = fmt::format("Bi:{:.2f} Up:{:.2f}", bi_rate, up_rate);
                    } else {
                        rate_str = fmt::format("{:.2f}", bi_rate);
                    }
                    const size_t universe_count = common_symbols.size();
                    size_t bithumb_quote_ready = 0, upbit_quote_ready = 0;
                    size_t bybit_quote_ready = 0, okx_quote_ready = 0;
                    size_t any_korean_quote_ready = 0, any_foreign_quote_ready = 0;
                    for (const auto& symbol : common_symbols) {
                        const std::string base(symbol.get_base());
                        const kimp::SymbolId foreign_symbol(base, "USDT");

                        bool bi_quote_ok = false;
                        if (bithumb_bases.count(base)) {
                            const auto p = engine.get_price_cache().get_price(kimp::Exchange::Bithumb, symbol);
                            bi_quote_ok = p.valid && p.ask > 0.0;
                            if (bi_quote_ok) ++bithumb_quote_ready;
                        }

                        bool up_quote_ok = false;
                        if (upbit_enabled && upbit_bases.count(base)) {
                            const auto p = engine.get_price_cache().get_price(kimp::Exchange::Upbit, symbol);
                            up_quote_ok = p.valid && p.ask > 0.0;
                            if (up_quote_ok) ++upbit_quote_ready;
                        }

                        if (bi_quote_ok || up_quote_ok) {
                            ++any_korean_quote_ready;
                        }

                        bool by_quote_ok = false;
                        if (bybit_bases.count(base)) {
                            const auto p = engine.get_price_cache().get_price(kimp::Exchange::Bybit, foreign_symbol);
                            by_quote_ok = p.valid && p.bid > 0.0;
                            if (by_quote_ok) ++bybit_quote_ready;
                        }

                        bool ok_quote_ok = false;
                        if (okx_enabled && okx_bases.count(base)) {
                            const auto p = engine.get_price_cache().get_price(kimp::Exchange::OKX, foreign_symbol);
                            ok_quote_ok = p.valid && p.bid > 0.0;
                            if (ok_quote_ok) ++okx_quote_ready;
                        }

                        if (by_quote_ok || ok_quote_ok) {
                            ++any_foreign_quote_ready;
                        }
                    }

                    std::cout << fmt::format("=== Spot Relay Monitor | negative-entry carry enterable {} / usable {} / common {} | 환율: {} KRW/USDT | target: {:.2f} USDT | every {}s ===\n",
                        std::count_if(visible_premiums.begin(), visible_premiums.end(), [](const auto& p) {
                            return p.entry_premium < 0.0 && p.both_can_fill_target && p.match_qty > 0.0;
                        }),
                        premiums.size(),
                        universe_count,
                        rate_str,
                        kimp::TradingConfig::TARGET_ENTRY_USDT, monitor_interval_sec);
                    std::cout << fmt::format(
                        "Columns: EntryPM 음수 + NetKRW {:.0f}원 이상 코인만 표시 | 전략: KR 현물 매수 -> Foreign 현물숏/차후 릴레이\n",
                        kimp::TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
                    std::cout << fmt::format(
                        "{:<10} {:<6} {:>14} {:>12} {:>12} {:>12} {:>12} {:>12} {:>11} {:>11} {:>11} {:>11} {:>24} {:>11} {:>12}\n",
                        "Symbol", "Pair", "K_ask", "K_qty", "K_KRW", "F_bid", "F_qty", "MatchQty",
                        "EntryPM", "Net%", "KFeeKRW", "FFeeU", "Commission", "NetKRW", "Age/1Tick");
                    std::cout << std::string(230, '-') << "\n";

                    if (visible_premiums.empty()) {
                        std::cout << fmt::format("현재 EntryPM 음수이면서 NetKRW {:.0f}원 이상인 코인이 없습니다.\n",
                                                 kimp::TradingConfig::MIN_ENTRY_NET_PROFIT_KRW);
                    }

                    for (const auto& p : visible_premiums) {
                        const char* one_tick = p.both_can_fill_target ? "YES" : "NO";
                        const std::string korean_ask = kimp::format::format_decimal_trimmed(p.korean_ask);
                        const std::string commission = fmt::format(
                            "{} / {:.0f}krw",
                            kimp::format::format_decimal_trimmed(p.withdraw_fee_coins),
                            p.withdraw_fee_krw);
                        const char* k_venue = (p.best_korean_exchange == kimp::Exchange::Upbit) ? "Up" : "Bi";
                        const char* f_venue = (p.best_foreign_exchange == kimp::Exchange::OKX) ? "Ok" : "By";
                        std::string pair = fmt::format("{}-{}", k_venue, f_venue);
                        std::cout << fmt::format(
                            "{:<10} {:<6} {:>14} {:>12.6f} {:>12.0f} {:>12.6f} {:>12.6f} {:>12.6f} {:>10.4f}% {:>10.4f}% {:>11.2f} {:>11.4f} {:>24} {:>11.2f} {:>12}\n",
                            p.symbol.get_base(),
                            pair,
                            korean_ask, p.korean_ask_qty,
                            p.bithumb_top_krw,
                            p.foreign_bid, p.foreign_bid_qty,
                            p.match_qty,
                            p.entry_premium, p.net_edge_pct,
                            p.bithumb_total_fee_krw,
                            p.bybit_total_fee_usdt,
                            commission,
                            p.net_profit_krw,
                            fmt::format("{}ms/{}", p.age_ms, one_tick));
                    }

                    std::cout << std::string(215, '-') << "\n";

                    // ===== TOP 5 NEGATIVE-ENTRY CARRY CANDIDATES =====
                    {
                        // Surface the intended carry setup explicitly:
                        // buy cheap on Korea, hedge/short rich on foreign, so EntryPM must be negative.
                        std::vector<const kimp::strategy::ArbitrageEngine::PremiumInfo*> top_candidates;
                        top_candidates.reserve(premiums.size());
                        for (const auto& p : premiums) {
                            if (p.korean_ask_qty > 0.0 &&
                                p.foreign_bid_qty > 0.0 &&
                                p.match_qty > 0.0 &&
                                p.entry_premium < 0.0 &&
                                p.net_profit_krw > 0.0) {
                                top_candidates.push_back(&p);
                            }
                        }
                        std::sort(top_candidates.begin(), top_candidates.end(),
                            [](const auto* a, const auto* b) {
                                if (a->net_profit_krw != b->net_profit_krw) {
                                    return a->net_profit_krw > b->net_profit_krw;
                                }
                                return a->entry_premium < b->entry_premium;
                            });
                        size_t top_n = std::min<size_t>(top_candidates.size(), 5);

                        std::cout << fmt::format("\n\033[1m=== Top {} Negative-Entry Carry Candidates (EntryPM < 0, NetKRW > 0) ===\033[0m\n", top_n);
                        std::cout << fmt::format(
                            "{:<10} {:<6} {:>14} {:>12} {:>12} {:>10} {:>10} {:>24} {:>11} {:>11}\n",
                            "Symbol", "Pair", "K_ask", "F_bid", "MatchQty",
                            "EntryPM", "Net%", "Commission", "NetKRW", "Age/1Tick");
                        std::cout << std::string(136, '-') << "\n";
                        for (size_t i = 0; i < top_n; ++i) {
                            const auto& p = *top_candidates[i];
                            const char* one_tick = p.both_can_fill_target ? "YES" : "NO";
                            const std::string korean_ask = kimp::format::format_decimal_trimmed(p.korean_ask);
                            const std::string commission = fmt::format(
                                "{} / {:.0f}krw",
                                kimp::format::format_decimal_trimmed(p.withdraw_fee_coins),
                                p.withdraw_fee_krw);
                            const char* k_venue = (p.best_korean_exchange == kimp::Exchange::Upbit) ? "Up" : "Bi";
                            const char* f_venue = (p.best_foreign_exchange == kimp::Exchange::OKX) ? "Ok" : "By";
                            std::string pair = fmt::format("{}-{}", k_venue, f_venue);
                            std::cout << fmt::format(
                                "{:<10} {:<6} {:>14} {:>12.6f} {:>12.6f} {:>9.4f}% {:>9.4f}% {:>24} {:>11.2f} {:>11}\n",
                                p.symbol.get_base(),
                                pair,
                                korean_ask, p.foreign_bid, p.match_qty,
                                p.entry_premium, p.net_edge_pct,
                                commission,
                                p.net_profit_krw,
                                fmt::format("{}ms/{}", p.age_ms, one_tick));
                        }
                        if (top_n == 0) {
                            std::cout << "EntryPM 음수이면서 NetKRW가 양수인 코인이 없습니다.\n";
                        }
                        std::cout << std::string(136, '-') << "\n";
                    }

                    auto enterable_count = std::count_if(premiums.begin(), premiums.end(), [](const auto& p) {
                        return p.entry_premium < 0.0 &&
                            kimp::TradingConfig::entry_gate_passes(
                            p.both_can_fill_target,
                            p.match_qty,
                            p.net_edge_pct,
                            p.net_profit_krw);
                    });
                    // Count per Korean/foreign exchange winning
                    size_t bithumb_winning = 0, upbit_winning = 0;
                    size_t bybit_winning = 0, okx_winning = 0;
                    for (const auto& p : premiums) {
                        if (p.best_korean_exchange == kimp::Exchange::Upbit) ++upbit_winning;
                        else ++bithumb_winning;
                        if (p.best_foreign_exchange == kimp::Exchange::OKX) ++okx_winning;
                        else ++bybit_winning;
                    }
                    std::cout << fmt::format(
                        "quotes: korean {}/{} (Bi:{} Up:{}) | foreign {}/{} (By:{} Ok:{}) | usable {}/{} | bestKR(Bi:{} Up:{}) | bestFR(By:{} Ok:{}) | net>={:.0f} {} | enterable {} | fee: 국내 {}회 + 해외 {}회 | wdFee: Bi:{} Up:{}\n",
                        any_korean_quote_ready, universe_count, bithumb_quote_ready, upbit_quote_ready,
                        any_foreign_quote_ready, universe_count, bybit_quote_ready, okx_quote_ready,
                        premiums.size(), universe_count,
                        bithumb_winning, upbit_winning,
                        bybit_winning, okx_winning,
                        kimp::TradingConfig::MIN_ENTRY_NET_PROFIT_KRW,
                        visible_premiums.size(),
                        enterable_count,
                        kimp::TradingConfig::BITHUMB_FEE_EVENTS,
                        kimp::TradingConfig::BYBIT_FEE_EVENTS,
                        engine.get_price_cache().withdraw_fee_count(kimp::Exchange::Bithumb),
                        engine.get_price_cache().withdraw_fee_count(kimp::Exchange::Upbit));

                    const auto blocked_routes = engine.get_transfer_blocked_symbols();
                    size_t kr_withdraw_blocked = 0;
                    size_t foreign_deposit_blocked = 0;
                    size_t no_shared_network = 0;
                    size_t route_data_missing = 0;
                    for (const auto& blocked : blocked_routes) {
                        if (blocked.reason.find("KR withdraw") != std::string::npos) {
                            ++kr_withdraw_blocked;
                        } else if (blocked.reason.find("Foreign deposit") != std::string::npos) {
                            ++foreign_deposit_blocked;
                        } else if (blocked.reason.find("shared") != std::string::npos) {
                            ++no_shared_network;
                        } else {
                            ++route_data_missing;
                        }
                    }
                    std::cout << fmt::format(
                        "transfer blacklist: {} | KR withdraw off {} | foreign deposit off {} | no shared net {} | route data missing {}\n",
                        blocked_routes.size(),
                        kr_withdraw_blocked,
                        foreign_deposit_blocked,
                        no_shared_network,
                        route_data_missing);
                    if (!blocked_routes.empty()) {
                        std::string blocked_sample;
                        const size_t sample_n = std::min<size_t>(blocked_routes.size(), 6);
                        for (size_t i = 0; i < sample_n; ++i) {
                            const auto& blocked = blocked_routes[i];
                            if (!blocked_sample.empty()) blocked_sample += " | ";
                            const char* k_venue = (blocked.korean_exchange == kimp::Exchange::Upbit) ? "Up" : "Bi";
                            const char* f_venue = (blocked.foreign_exchange == kimp::Exchange::OKX) ? "Ok" : "By";
                            blocked_sample += fmt::format("{}({}-{}:{})",
                                blocked.symbol.get_base(), k_venue, f_venue, blocked.reason);
                        }
                        std::cout << fmt::format("blocked sample: {}\n", blocked_sample);
                    }

                    // ===== SELECTED (active positions) section =====
                    auto& pos_tracker = engine.get_position_tracker();
                    if (pos_tracker.get_position_count() > 0) {
                        std::cout << "\n\033[1m=== Active Positions ===\033[0m\n";
                        std::cout << fmt::format("{:<10} {:>11} {:>12} {:>12} {:>12} {:>11} {:>11}\n",
                            "Symbol", "EntryPM", "ExitPM", "Net%", "NetKRW", "HeldUSD", "Coins");
                        std::cout << std::string(94, '-') << "\n";

                        for (const auto& p : premiums) {
                            auto pos = pos_tracker.get_position(p.symbol);
                            if (!pos) continue;

                            double pos_usd = pos->foreign_amount * pos->foreign_entry_price;
                            std::cout << fmt::format("{:<10} {:>10.4f}% {:>11.4f}% {:>10.4f}% {:>11.2f} {:>11.2f} {:>11.8f}\n",
                                p.symbol.get_base(),
                                pos->entry_premium,
                                p.exit_premium,
                                p.net_edge_pct,
                                p.net_profit_krw,
                                pos_usd,
                                pos->korean_amount);
                        }
                        std::cout << std::string(94, '-') << "\n";
                    }

                std::cout << fmt::format("Entry: EntryPM<0 + NetKRW>={:.0f} + both fill {:.2f} USDT\n",
                    kimp::TradingConfig::MIN_ENTRY_NET_PROFIT_KRW,
                    kimp::TradingConfig::TARGET_ENTRY_USDT);
                std::cout << "Ctrl+C to stop\n";
                std::cout.flush();
            }

        }
    }

    // Shutdown
    spdlog::info("Shutting down...");

    // Stop broadcast thread first
    broadcast_running = false;
    if (broadcast_thread.joinable()) {
        broadcast_thread.join();
    }

    if (ws_server) {
        ws_server->stop();  // Stop WebSocket server
    }
        if (warmup_thread.joinable()) {
            warmup_thread.join();
        }
        if (transfer_refresh_thread.joinable()) {
            transfer_refresh_thread.join();
        }
        order_manager.request_shutdown();  // Break adaptive loops before stopping engine
        lifecycle_executor.stop();
        engine.stop_async_exporter();
    engine.stop();
    bithumb->disconnect();
    bybit->disconnect();
    if (okx) okx->disconnect();
    if (upbit) upbit->disconnect();

    stop_io_threads();

    spdlog::info("=== Bot Stopped ===");
    kimp::Logger::shutdown();

    return 0;
}
