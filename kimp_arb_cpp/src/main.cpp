#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/simd_premium.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/execution/order_manager.hpp"
#include "kimp/network/ws_broadcast_server.hpp"

#include <boost/asio.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <cstdio>
#include <csignal>
#include <atomic>
#include <thread>
#include <filesystem>
#include <unordered_set>
#include <future>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <cctype>
#include <condition_variable>
#include <cmath>
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
std::atomic<bool> g_entry_in_flight{false};

struct EntryGuard {
    std::atomic<bool>& flag;
    explicit EntryGuard(std::atomic<bool>& f) : flag(f) {}
    ~EntryGuard() { flag.store(false, std::memory_order_release); }
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
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

uint64_t next_funding_time_from_interval_utc(int interval_hours) {
    if (interval_hours <= 0) {
        return 0;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    int seconds_since_midnight = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    int interval_sec = interval_hours * 3600;
    int next_sec = ((seconds_since_midnight / interval_sec) + 1) * interval_sec;
    std::time_t base = now - seconds_since_midnight;
    if (next_sec >= 24 * 3600) {
        base += 24 * 3600;
        next_sec -= 24 * 3600;
    }
    std::time_t next_time = base + next_sec;
    return static_cast<uint64_t>(next_time) * 1000;
}

std::string format_remaining_ms(int interval_hours, uint64_t next_funding_time_ms) {
    uint64_t target_ms = next_funding_time_ms;
    if (interval_hours > 0) {
        target_ms = next_funding_time_from_interval_utc(interval_hours);
    }
    if (target_ms == 0) {
        return "--";
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (target_ms <= static_cast<uint64_t>(now_ms)) {
        return "0m";
    }
    uint64_t diff_sec = (target_ms - static_cast<uint64_t>(now_ms)) / 1000;
    uint64_t hours = diff_sec / 3600;
    uint64_t mins = (diff_sec % 3600) / 60;
    char buf[16];
    if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "%lluh%02llum",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(mins));
    } else {
        std::snprintf(buf, sizeof(buf), "%llum",
                      static_cast<unsigned long long>(mins));
    }
    return std::string(buf);
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
        << std::fixed << std::setprecision(2) << pos.korean_entry_price << ','
        << std::fixed << std::setprecision(2) << pos.korean_exit_price << ','
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
        "  \"korean_entry_price\": {:.2f},\n"
        "  \"foreign_entry_price\": {:.8f},\n"
        "  \"realized_pnl_krw\": {:.2f},\n"
        "  \"is_active\": true\n"
        "}}",
        pos.symbol.get_base(), pos.symbol.get_quote(),
        static_cast<int>(pos.korean_exchange), static_cast<int>(pos.foreign_exchange),
        entry_ms, pos.entry_premium, pos.position_size_usd,
        pos.korean_amount, pos.foreign_amount,
        pos.korean_entry_price, pos.foreign_entry_price,
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
        if (!env_val) {
            std::cerr << "FATAL: Environment variable '" << env_name
                      << "' is not set" << std::endl;
            return {};
        }
        return env_val;
    }
    return val;
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
            if (e["rest_endpoint"]) creds.rest_endpoint = e["rest_endpoint"].as<std::string>();
            if (e["api_key"]) {
                std::string raw = e["api_key"].as<std::string>();
                creds.api_key = require_private_keys ? expand_env(raw) : raw;
            }
            if (e["secret_key"]) {
                std::string raw = e["secret_key"].as<std::string>();
                creds.secret_key = require_private_keys ? expand_env(raw) : raw;
            }

            if (require_private_keys &&
                (creds.api_key.empty() || creds.secret_key.empty())) {
                std::cerr << "Exchange '" << name
                          << "': api_key or secret_key is missing" << std::endl;
                return false;
            }

            config.exchanges[ex] = std::move(creds);
            return true;
        };

        if (!load_exchange("bithumb", kimp::Exchange::Bithumb)) return std::nullopt;
        if (!load_exchange("bybit", kimp::Exchange::Bybit)) return std::nullopt;

    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to parse config '" << path << "': "
                  << e.what() << std::endl;
        return std::nullopt;
    }

    return config;
}

int main(int argc, char* argv[]) {
    std::string config_path = "config/config.yaml";
    bool monitor_mode = true;
    bool monitor_only = false;
    int monitor_interval_sec = 2;

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

    auto config_opt = load_config(config_path, !monitor_only);
    if (!config_opt) {
        return 1;
    }
    auto config = std::move(*config_opt);

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

    net::io_context io_context;
    auto work_guard = net::make_work_guard(io_context);

    // Create exchanges (Bithumb: Korean spot, Bybit: Foreign futures)
    // load_config guarantees both entries exist; check enabled flag only
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

    // Strategy engine
    kimp::strategy::ArbitrageEngine engine;
    engine.set_exchange(kimp::Exchange::Bithumb, bithumb);
    engine.set_exchange(kimp::Exchange::Bybit, bybit);
    engine.add_exchange_pair(kimp::Exchange::Bithumb, kimp::Exchange::Bybit);

    // Order manager for auto-trading
    kimp::execution::OrderManager order_manager;
    order_manager.set_engine(&engine);
    order_manager.set_exchange(kimp::Exchange::Bithumb, bithumb);
    order_manager.set_exchange(kimp::Exchange::Bybit, bybit);

    // Position persistence callback (crash recovery)
    order_manager.set_position_update_callback([](const kimp::Position* pos) {
        if (pos) {
            save_active_position(*pos);
        } else {
            delete_active_position();
        }
    });

    // Auto-trading callbacks (disabled in monitor-only mode)
    // IMPORTANT: For perfect hedge, execute futures FIRST to get exact contract size
    //
    // Entry sequence:
    //   1. SHORT futures (Bybit) - get exact coin amount from contract size
    //   2. BUY spot (Bithumb) - buy exact same amount
    //
    // Exit sequence:
    //   1. COVER futures (Bybit) - close short position
    //   2. SELL spot (Bithumb) - sell exact same amount
    if (!monitor_only) {
        engine.set_entry_callback([&](const kimp::ArbitrageSignal& signal) {
        bool expected = false;
        if (!g_entry_in_flight.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel)) {
            spdlog::warn("Entry skipped: another entry is already in-flight");
            return;
        }
        EntryGuard entry_guard(g_entry_in_flight);

        if (engine.get_position_count() >= kimp::TradingConfig::MAX_POSITIONS) {
            spdlog::warn("Max positions reached, skipping entry for {}", signal.symbol.to_string());
            return;
        }

        kimp::execution::ExecutionResult result =
            order_manager.execute_entry_futures_first(signal);

        if (result.success) {
            engine.open_position(result.position);
            // Handle any realized P&L from exit splits during adaptive entry
            if (std::abs(result.position.realized_pnl_krw) > 0.01) {
                double usdt_rate = signal.usdt_krw_rate;
                double pnl_usd = result.position.realized_pnl_krw / usdt_rate;
                engine.add_realized_pnl(pnl_usd);
                spdlog::info("[ADAPTIVE] Entry had interim exit splits: P&L {:.2f} USD", pnl_usd);
            }
        } else if (std::abs(result.position.realized_pnl_krw) > 0.01) {
            // Fully exited during entry - P&L from exit splits
            double usdt_rate = signal.usdt_krw_rate;
            double pnl_usd = result.position.realized_pnl_krw / usdt_rate;
            double capital_before = engine.get_current_capital();
            engine.add_realized_pnl(pnl_usd);
            double capital_after = engine.get_current_capital();
            append_trade_log(result.position, pnl_usd, usdt_rate, capital_before, capital_after);
            spdlog::info("[ADAPTIVE] Entry fully reversed: P&L {:.0f} KRW ({:.2f} USD)",
                         result.position.realized_pnl_krw, pnl_usd);
        } else if (!result.error_message.empty()) {
            spdlog::error("Entry FAILED: {}", result.error_message);
        }
        });

        engine.set_exit_callback([&](const kimp::ExitSignal& signal) {
        kimp::Position closed_pos;
        if (!engine.close_position(signal.symbol, closed_pos)) {
            spdlog::warn("No position to close for {}", signal.symbol.to_string());
            return;
        }

        kimp::execution::ExecutionResult result =
            order_manager.execute_exit_futures_first(signal, closed_pos);

        if (result.success) {
            // Convert KRW P&L to USD and add to capital tracker (복리 성장)
            double usdt_rate = signal.usdt_krw_rate;
            double pnl_usd = result.position.realized_pnl_krw / usdt_rate;
            double capital_before = engine.get_current_capital();
            engine.add_realized_pnl(pnl_usd);
            double capital_after = engine.get_current_capital();

            append_trade_log(result.position, pnl_usd, usdt_rate, capital_before, capital_after);
        } else {
            // Exit failed or partial - RE-ADD position with UPDATED amounts (not original!)
            // result.position reflects actual remaining after any partial exits
            spdlog::error("Exit FAILED: {} - Re-adding position to tracking", result.error_message);
            engine.open_position(result.position);
        }
        });
    } else {
        spdlog::info("Monitor-only mode enabled: auto-trading callbacks are disabled");
    }

    // ==========================================================================
    // 병렬 포지션 관리: 조건 만족하는 모든 코인 동시 진입, 각각 0.8% 수익시 개별 청산
    // - Entry: 8시간 펀딩 + 펀딩비 양수 + 프리미엄 <= -0.99%
    // - Exit: 프리미엄 >= max(entry + 0.79%, +0.10%)
    // - 최대 동시 포지션: 16개
    // ==========================================================================

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
            else core_id = thread_config.io_gateio_core;  // Additional threads

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

    // Connect
    spdlog::info("Connecting to exchanges...");
    bithumb->connect();
    bybit->connect();

    // Wait for connections (poll instead of fixed sleep, max 10 seconds)
    spdlog::info("Waiting for connections...");
    int wait_count = 0;
    while (!g_shutdown && wait_count < 50) {
        if (bithumb->is_connected() && bybit->is_connected()) {
            spdlog::info("Both exchanges connected");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ++wait_count;
    }

    if (!bithumb->is_connected() || !bybit->is_connected()) {
        spdlog::error("Failed to connect to exchanges");
        bithumb->disconnect();
        bybit->disconnect();
        stop_io_threads();
        kimp::Logger::shutdown();
        return 1;
    }

    // Find common symbols using set intersection (O(n+m) instead of O(n*m))
    auto bithumb_symbols = bithumb->get_available_symbols();
    auto bybit_symbols = bybit->get_available_symbols();

    std::unordered_set<std::string> bybit_bases;
    for (const auto& s : bybit_symbols) {
        bybit_bases.insert(std::string(s.get_base()));
    }

    std::vector<kimp::SymbolId> common_symbols;
    for (const auto& s : bithumb_symbols) {
        if (bybit_bases.count(std::string(s.get_base()))) {
            common_symbols.emplace_back(std::string(s.get_base()), "KRW");
            engine.add_symbol(common_symbols.back());
        }
    }

    spdlog::info("Found {} common symbols", common_symbols.size());

    if (common_symbols.empty()) {
        spdlog::error("No common symbols found between Bithumb and Bybit");
        bithumb->disconnect();
        bybit->disconnect();
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
        // Pre-set leverage to 1x for all symbols at startup (avoid per-trade REST latency)
        order_manager.pre_set_leverage(common_symbols);

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
        spdlog::info("Monitor-only mode: skipping leverage preset and external position blacklist");
    }

    // Start async JSON exporter FIRST (before any price loading or trading)
    // 200ms interval for file-based updates (fallback)
    engine.start_async_exporter("data/premiums.json", std::chrono::milliseconds(200));
    spdlog::info("JSON exporter started (200ms interval)");

    // =========================================================================
    // WebSocket Broadcast Server for REAL-TIME dashboard updates (<10ms latency)
    // =========================================================================
    auto ws_server = std::make_shared<kimp::network::WsBroadcastServer>(io_context, 8765);
    ws_server->start();

    // =========================================================================
    // DEDICATED BROADCAST THREAD - Exactly 50ms interval for perfect latency
    // =========================================================================
    std::atomic<bool> broadcast_running{true};
    std::atomic<int> broadcast_count{0};
    std::thread broadcast_thread([&]() {
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
                            "{{\"s\":\"{}/{}\",\"kb\":{:.2f},\"ka\":{:.2f},\"fb\":{:.8f},\"fa\":{:.8f},\"kp\":{:.2f},\"fp\":{:.8f},\"r\":{:.4f},\"ep\":{:.4f},\"xp\":{:.4f},\"sp\":{:.4f},\"pm\":{:.4f},\"fr\":{:.8f},\"fi\":{},\"sg\":{}}}",
                            p.symbol.get_base(), p.symbol.get_quote(),
                            p.korean_bid, p.korean_ask,
                            p.foreign_bid, p.foreign_ask,
                            p.korean_price, p.foreign_price, p.usdt_rate,
                            p.entry_premium, p.exit_premium, p.premium_spread, p.premium,
                            p.funding_rate, p.funding_interval_hours,
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

    // =========================================================================
    // INITIAL PRICE LOADING via REST API (parallel fetch for ALL symbols)
    // This ensures all 257+ symbols have prices BEFORE WebSocket streaming starts
    // =========================================================================
    spdlog::info("Loading initial prices via REST API (parallel)...");

    // Launch parallel REST API calls (fast!)
    auto bithumb_fetch = std::async(std::launch::async, [&bithumb]() {
        return bithumb->fetch_all_tickers();
    });

    auto bybit_fetch = std::async(std::launch::async, [&bybit]() {
        return bybit->fetch_all_tickers();
    });

    // Wait for both and process results
    auto bithumb_tickers = bithumb_fetch.get();
    auto bybit_tickers = bybit_fetch.get();

    // Load Bithumb tickers into engine
    int bithumb_loaded = 0;
    for (const auto& ticker : bithumb_tickers) {
        if (common_bases.count(std::string(ticker.symbol.get_base()))) {
            engine.on_ticker_update(ticker);
            ++bithumb_loaded;
        }
        // Also load USDT/KRW for exchange rate
        if (ticker.symbol.get_base() == "USDT") {
            engine.on_ticker_update(ticker);
        }
    }

    // Load Bybit tickers into engine
    int bybit_loaded = 0;
    for (const auto& ticker : bybit_tickers) {
        if (common_bases.count(std::string(ticker.symbol.get_base()))) {
            engine.on_ticker_update(ticker);
            ++bybit_loaded;
        }
    }

    spdlog::info("Initial prices loaded: Bithumb={}, Bybit={}", bithumb_loaded, bybit_loaded);
    // =========================================================================

    // =========================================================================
    // STEP 12: WebSocket Subscriptions (real-time streaming)
    // =========================================================================
    bithumb->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });

    std::vector<kimp::SymbolId> bithumb_subs = common_symbols;
    bithumb_subs.emplace_back("USDT", "KRW");  // For exchange rate
    bithumb->subscribe_ticker(bithumb_subs);
    spdlog::info("Subscribed to {} Bithumb tickers (including USDT/KRW)", bithumb_subs.size());

    // Fetch orderbook snapshots and subscribe to orderbookdepth for real bid/ask
    bithumb->fetch_all_orderbook_snapshots(bithumb_subs);
    bithumb->subscribe_orderbook(bithumb_subs);

    bybit->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });

    std::vector<kimp::SymbolId> bybit_subs;
    for (const auto& s : common_symbols) {
        bybit_subs.emplace_back(std::string(s.get_base()), "USDT");
    }
    bybit->subscribe_ticker(bybit_subs);
    spdlog::info("Subscribed to {} Bybit tickers", bybit_subs.size());

    // =========================================================================
    // STEP 13: Background Refresh Threads (REST fallback for data maintenance)
    // =========================================================================

    // 13-1: Funding refresh (every 5 minutes) - Bybit funding_rate/interval update
    std::atomic<bool> funding_refresh_running{true};
    std::mutex funding_refresh_mutex;
    std::condition_variable funding_refresh_cv;
    std::thread funding_refresh_thread([&]() {
        constexpr auto interval = std::chrono::minutes(5);
        std::unique_lock<std::mutex> lock(funding_refresh_mutex);
        while (funding_refresh_running && !g_shutdown) {
            lock.unlock();

            // Refresh funding info via REST (cached, not on trading path)
            auto bybit_tickers_refresh = bybit->fetch_all_tickers();
            if (!bybit_tickers_refresh.empty()) {
                auto& cache = engine.get_price_cache();
                for (const auto& ticker : bybit_tickers_refresh) {
                    if (common_bases.count(std::string(ticker.symbol.get_base()))) {
                        cache.update_funding(ticker.exchange, ticker.symbol,
                            ticker.funding_rate, ticker.funding_interval_hours, ticker.next_funding_time);
                    }
                }
            }

            lock.lock();
            funding_refresh_cv.wait_for(lock, interval, [&] {
                return !funding_refresh_running || g_shutdown.load();
            });
        }
    });

    // 13-2: Price refresh (every 4 minutes) - WS backup, USDT/KRW & funding sync
    std::atomic<bool> price_refresh_running{true};
    std::mutex price_refresh_mutex;
    std::condition_variable price_refresh_cv;
    std::thread price_refresh_thread([&]() {
        constexpr auto interval = std::chrono::minutes(4);
        std::unique_lock<std::mutex> lock(price_refresh_mutex);
        while (price_refresh_running && !g_shutdown) {
            lock.unlock();

            // Fetch both snapshots in parallel to minimize cross-exchange skew.
            auto bithumb_fetch = std::async(std::launch::async, [&bithumb]() {
                return bithumb->fetch_all_tickers();
            });
            auto bybit_fetch = std::async(std::launch::async, [&bybit]() {
                return bybit->fetch_all_tickers();
            });

            auto bithumb_tickers_refresh = bithumb_fetch.get();
            auto bybit_tickers_refresh = bybit_fetch.get();

            const uint64_t snapshot_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            auto& cache = engine.get_price_cache();
            for (const auto& ticker : bithumb_tickers_refresh) {
                if (ticker.symbol.get_base() == "USDT") {
                    double usdt_price = ticker.last;
                    if (ticker.bid > 0.0 && ticker.ask > 0.0 && ticker.ask >= ticker.bid) {
                        usdt_price = (ticker.bid + ticker.ask) * 0.5;
                    }
                    cache.update_usdt_krw(ticker.exchange, usdt_price);
                }
                if (common_bases.count(std::string(ticker.symbol.get_base()))) {
                    cache.update(ticker.exchange, ticker.symbol, ticker.bid, ticker.ask, ticker.last,
                                 snapshot_ms);
                }
            }

            for (const auto& ticker : bybit_tickers_refresh) {
                if (common_bases.count(std::string(ticker.symbol.get_base()))) {
                    cache.update(ticker.exchange, ticker.symbol, ticker.bid, ticker.ask, ticker.last,
                                 snapshot_ms);
                    if (std::isfinite(ticker.funding_rate) || ticker.next_funding_time != 0) {
                        cache.update_funding(ticker.exchange, ticker.symbol,
                            ticker.funding_rate, ticker.funding_interval_hours, ticker.next_funding_time);
                    }
                }
            }

            lock.lock();
            price_refresh_cv.wait_for(lock, interval, [&] {
                return !price_refresh_running || g_shutdown.load();
            });
        }
    });

    // =========================================================================
    // STARTUP RECOVERY: Check for existing positions (trade mode only)
    // =========================================================================
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
                    double futures_amount{0.0};
                    double matched_amount{0.0};
                    double matched_usd{0.0};
                    bool from_saved_file{false};
                };

                std::vector<RecoveryCandidate> candidates;
                std::unordered_map<std::string, kimp::Position> bybit_short_by_coin;

                // Query Bybit short positions once, then scan all candidates from that set.
                auto bybit_positions = bybit->get_positions();
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

                for (const auto& [coin, bybit_pos] : bybit_short_by_coin) {
                    kimp::SymbolId krw_symbol(coin, "KRW");
                    kimp::SymbolId usdt_symbol(coin, "USDT");

                    double spot_balance = bithumb->get_balance(coin);
                    double futures_amount = bybit_pos.foreign_amount;
                    if (spot_balance <= 0.0001 || futures_amount <= 0.0001) continue;

                    double amount = std::min(spot_balance, futures_amount);

                    auto korean_price = engine.get_price_cache().get_price(
                        kimp::Exchange::Bithumb, krw_symbol);
                    double korean_entry = 0.0;
                    if (korean_price.valid) {
                        if (korean_price.ask > 0) korean_entry = korean_price.ask;
                        else if (korean_price.last > 0) korean_entry = korean_price.last;
                    }

                    auto foreign_price = engine.get_price_cache().get_price(
                        kimp::Exchange::Bybit, usdt_symbol);
                    double foreign_fallback = 0.0;
                    if (foreign_price.valid) {
                        if (foreign_price.bid > 0) foreign_fallback = foreign_price.bid;
                        else if (foreign_price.last > 0) foreign_fallback = foreign_price.last;
                    }
                    double effective_foreign_entry =
                        (bybit_pos.foreign_entry_price > 0.0) ? bybit_pos.foreign_entry_price : foreign_fallback;

                    double usdt_rate = engine.get_price_cache().get_usdt_krw(kimp::Exchange::Bithumb);
                    double recovered_entry_pm = 0.0;
                    if (korean_entry > 0.0 && effective_foreign_entry > 0.0 && usdt_rate > 0.0) {
                        double foreign_krw = effective_foreign_entry * usdt_rate;
                        recovered_entry_pm = ((korean_entry - foreign_krw) / foreign_krw) * 100.0;
                    }

                    kimp::Position pos;
                    pos.symbol = krw_symbol;
                    pos.korean_exchange = kimp::Exchange::Bithumb;
                    pos.foreign_exchange = kimp::Exchange::Bybit;
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
                    c.futures_amount = futures_amount;
                    c.matched_amount = amount;
                    c.matched_usd = pos.position_size_usd;
                    if (c.matched_usd <= 0.0 && foreign_fallback > 0.0) {
                        c.matched_usd = amount * foreign_fallback;
                    }
                    candidates.push_back(c);
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
                        c.futures_amount = loaded_pos->foreign_amount;
                        c.matched_amount = std::min(c.spot_balance, c.futures_amount);
                        c.matched_usd = loaded_pos->position_size_usd;
                        if (c.matched_usd <= 0.0) {
                            c.matched_usd = loaded_pos->korean_amount * loaded_pos->foreign_entry_price;
                        }
                        c.from_saved_file = true;
                        candidates.push_back(c);
                    }
                }

                if (!candidates.empty()) {
                    std::sort(candidates.begin(), candidates.end(),
                        [](const RecoveryCandidate& a, const RecoveryCandidate& b) {
                            if (a.matched_usd != b.matched_usd) return a.matched_usd > b.matched_usd;
                            return a.pos.entry_premium < b.pos.entry_premium;
                        });

                    const auto& best = candidates.front();

                    std::cout << "\n";
                    std::cout << "=========================================\n";
                    std::cout << "  RECOVERY CANDIDATE SELECTED (BEST 1)\n";
                    std::cout << "=========================================\n";
                    std::cout << fmt::format("  Candidates: {} (best by matched USD)\n", candidates.size());
                    std::cout << fmt::format("  Symbol:     {}\n", best.pos.symbol.to_string());
                    std::cout << fmt::format("  Bithumb:    {:.8f} coins\n", best.spot_balance);
                    std::cout << fmt::format("  Bybit:      {:.8f} coins\n", best.futures_amount);
                    std::cout << fmt::format("  Matched:    {:.8f} coins (${:.2f})\n",
                                              best.matched_amount, best.matched_usd);
                    std::cout << fmt::format("  Entry:      KRW {:.2f} / USD {:.6f}\n",
                                              best.pos.korean_entry_price, best.pos.foreign_entry_price);
                    std::cout << fmt::format("  Premium:    {:.4f}%\n", best.pos.entry_premium);
                    if (best.from_saved_file) {
                        std::cout << "  Source:     saved file fallback\n";
                    } else {
                        std::cout << "  Source:     live exchange scan\n";
                    }
                    std::cout << "=========================================\n";
                    std::cout << "\n이 포지션으로 시작할까요? (y/n): ";
                    std::cout.flush();

                    std::string confirm;
                    if (read_stdin_line(confirm) && is_yes(confirm)) {
                        engine.open_position(best.pos);
                        resumed_position = true;
                        save_active_position(best.pos);
                        spdlog::info("[RECOVERY] Restored best position: {} ({:.8f} coins, ${:.2f})",
                                     best.pos.symbol.to_string(), best.matched_amount, best.matched_usd);
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

    if (!g_shutdown) {
        // Start engine
        engine.start();

        if (monitor_only) {
            spdlog::info("=== Bot Running (MONITOR-ONLY, Auto-Trading DISABLED) ===");
            spdlog::info("Monitor interval: {}s", monitor_interval_sec);
            spdlog::info("Press Ctrl+C to stop");
        } else {
            spdlog::info("=== Bot Running (Auto-Trading ENABLED) ===");
            spdlog::info("Mode: SPLIT ORDERS (~33s) | Max positions: {}",
                         kimp::TradingConfig::MAX_POSITIONS);
            spdlog::info("Capital: ${:.2f} | Position size: ${:.2f}/side (복리 성장)",
                         engine.get_current_capital(), engine.get_position_size_usd());
            spdlog::info("Entry: premium <= {:.2f}% (8h funding, rate > 0)",
                         kimp::TradingConfig::ENTRY_PREMIUM_THRESHOLD);
            spdlog::info("Exit: dynamic max(entry_pm + {:.2f}% fees + {:.2f}% profit, +{:.2f}% floor)",
                         kimp::TradingConfig::ROUND_TRIP_FEE_PCT,
                         kimp::TradingConfig::MIN_NET_PROFIT_PCT,
                         kimp::TradingConfig::EXIT_PREMIUM_THRESHOLD);
            spdlog::info("Press Ctrl+C to stop");
        }

        // Main loop with console output
        LiveMonitorGuard live_monitor_guard(monitor_mode);
        int tick = 0;
        while (!g_shutdown) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++tick;

            // Full monitor: all symbols + both-side prices + entry/exit premiums
            if (monitor_mode && tick % monitor_interval_sec == 0) {
                auto premiums = engine.get_all_premiums();
                if (!premiums.empty()) {
                    std::sort(premiums.begin(), premiums.end(),
                        [](const auto& a, const auto& b) { return a.entry_premium < b.entry_premium; });

                    if (live_monitor_guard.active()) {
                        live_monitor_guard.clear_frame();
                    } else {
                        std::cout << "\033[2J\033[H";
                    }
                    std::cout << fmt::format("=== KIMP Full Monitor | {} symbols | USDT: {:.2f} KRW | every {}s ===\n",
                        premiums.size(), premiums[0].usdt_rate, monitor_interval_sec);
                    std::cout << "Columns: KR/Bybit bid-ask, entry/exit premium, spread\n";
                    std::cout << fmt::format("{:<10} {:>11} {:>11} {:>12} {:>12} {:>9} {:>9} {:>8} {:>8} {:>4} {:>7} {:>4}\n",
                        "Symbol", "KR_bid", "KR_ask", "BY_bid", "BY_ask", "Entry%", "Exit%", "Sprd%", "Fund%", "Int", "TTE", "Sig");
                    std::cout << std::string(132, '-') << "\n";

                    for (const auto& p : premiums) {
                        std::string tte = format_remaining_ms(p.funding_interval_hours, p.next_funding_time);
                        const char* sig = p.entry_signal ? "ENT" : (p.exit_signal ? "EXT" : "-");
                        std::cout << fmt::format("{:<10} {:>11.2f} {:>11.2f} {:>12.6f} {:>12.6f} {:>8.4f}% {:>8.4f}% {:>7.4f}% {:>7.4f}% {:>3}h {:>7} {:>4}\n",
                            p.symbol.get_base(), p.korean_bid, p.korean_ask,
                            p.foreign_bid, p.foreign_ask,
                            p.entry_premium, p.exit_premium, p.premium_spread,
                            p.funding_rate * 100, p.funding_interval_hours, tte, sig);
                    }

                    std::cout << std::string(132, '-') << "\n";
                    const auto& tracker = engine.get_capital_tracker();
                    std::cout << fmt::format("Positions: {}/{} | Capital: ${:.2f} (+{:.2f}%) | Size: ${:.2f}/side\n",
                        engine.get_position_count(), kimp::TradingConfig::MAX_POSITIONS,
                        tracker.get_current_capital(), tracker.get_return_percent(),
                        engine.get_position_size_usd());
                    std::cout << fmt::format("Entry <= {:.2f}% | Exit >= max(entry + {:.2f}%, +{:.2f}%) | Trades: {} ({:.1f}% win)\n",
                        kimp::TradingConfig::ENTRY_PREMIUM_THRESHOLD,
                        kimp::TradingConfig::DYNAMIC_EXIT_SPREAD,
                        kimp::TradingConfig::EXIT_PREMIUM_THRESHOLD,
                        tracker.get_total_trades(), tracker.get_win_rate());
                    std::cout << "Ctrl+C to stop\n";
                    std::cout.flush();
                }
            }

            // Non-monitor mode: periodic status log
            if (!monitor_mode && tick % 30 == 0) {
                const auto& tracker = engine.get_capital_tracker();
                spdlog::info("Status: positions={}/{} | Capital: ${:.2f} (+{:.2f}%) | Next size: ${:.2f}/side",
                             engine.get_position_count(), kimp::TradingConfig::MAX_POSITIONS,
                             tracker.get_current_capital(), tracker.get_return_percent(),
                             engine.get_position_size_usd());
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

    // Stop funding refresh thread
    funding_refresh_running = false;
    funding_refresh_cv.notify_all();
    if (funding_refresh_thread.joinable()) {
        funding_refresh_thread.join();
    }

    // Stop price refresh thread
    price_refresh_running = false;
    price_refresh_cv.notify_all();
    if (price_refresh_thread.joinable()) {
        price_refresh_thread.join();
    }

    ws_server->stop();  // Stop WebSocket server
    order_manager.request_shutdown();  // Break adaptive loops before stopping engine
    engine.stop_async_exporter();
    engine.stop();
    bithumb->disconnect();
    bybit->disconnect();

    work_guard.reset();
    io_context.stop();

    for (auto& t : io_threads) {
        if (t.joinable()) t.join();
    }

    spdlog::info("=== Bot Stopped ===");
    kimp::Logger::shutdown();

    return 0;
}
