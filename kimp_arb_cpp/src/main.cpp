#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/strategy/arbitrage_engine.hpp"
#include "kimp/execution/order_manager.hpp"
#include "kimp/network/ws_broadcast_server.hpp"

#include <boost/asio.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <filesystem>
#include <unordered_set>
#include <future>
#include <simdjson.h>
#include <fmt/format.h>

namespace net = boost::asio;

std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    std::cout << "\nShutdown signal received..." << std::endl;
    g_shutdown = true;
}

std::string expand_env(const std::string& val) {
    if (val.size() > 3 && val[0] == '$' && val[1] == '{' && val.back() == '}') {
        std::string env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        return env_val ? env_val : "";
    }
    return val;
}

kimp::RuntimeConfig load_config(const std::string& path) {
    kimp::RuntimeConfig config;

    try {
        YAML::Node yaml = YAML::LoadFile(path);

        if (yaml["trading"]) {
            auto t = yaml["trading"];
            if (t["max_positions"]) config.max_positions = t["max_positions"].as<int>();
            if (t["position_size_usd"]) config.position_size_usd = t["position_size_usd"].as<double>();
            if (t["entry_premium_threshold"]) config.entry_premium_threshold = t["entry_premium_threshold"].as<double>();
            if (t["exit_premium_threshold"]) config.exit_premium_threshold = t["exit_premium_threshold"].as<double>();
        }

        if (yaml["logging"]) {
            auto l = yaml["logging"];
            if (l["level"]) config.log_level = l["level"].as<std::string>();
            if (l["file"]) config.log_file = l["file"].as<std::string>();
        }

        auto load_exchange = [&](const std::string& name, kimp::Exchange ex) {
            if (yaml["exchanges"][name]) {
                auto e = yaml["exchanges"][name];
                kimp::ExchangeCredentials creds;
                creds.enabled = e["enabled"] ? e["enabled"].as<bool>() : true;
                if (e["ws_endpoint"]) creds.ws_endpoint = e["ws_endpoint"].as<std::string>();
                if (e["rest_endpoint"]) creds.rest_endpoint = e["rest_endpoint"].as<std::string>();
                if (e["api_key"]) creds.api_key = expand_env(e["api_key"].as<std::string>());
                if (e["secret_key"]) creds.secret_key = expand_env(e["secret_key"].as<std::string>());
                config.exchanges[ex] = std::move(creds);
            }
        };

        load_exchange("bithumb", kimp::Exchange::Bithumb);
        load_exchange("bybit", kimp::Exchange::Bybit);

    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to load config: " << e.what() << std::endl;
    }

    return config;
}

int main(int argc, char* argv[]) {
    std::string config_path = "config/config.yaml";
    bool monitor_mode = false;  // TUI mode: show premium table, logs to file only

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "-m" || arg == "--monitor") {
            monitor_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "KIMP Arbitrage Bot - C++ HFT Version\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  -c, --config <path>  Config file (default: config/config.yaml)\n"
                      << "  -m, --monitor        Monitor mode (show premium table in console)\n"
                      << "  -h, --help           Show this help\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto config = load_config(config_path);

    std::filesystem::create_directories("logs");
    std::filesystem::create_directories("data");

    // In monitor mode, logs go to file only (no console spam)
    kimp::Logger::init(config.log_file, config.log_level, 100, 10, 8192, !monitor_mode);
    spdlog::info("=== KIMP Arbitrage Bot Starting ===");

    net::io_context io_context;
    auto work_guard = net::make_work_guard(io_context);

    // Create exchanges (Bithumb: Korean spot, Bybit: Foreign futures)
    std::shared_ptr<kimp::exchange::bithumb::BithumbExchange> bithumb;
    std::shared_ptr<kimp::exchange::bybit::BybitExchange> bybit;

    if (config.exchanges.count(kimp::Exchange::Bithumb) &&
        config.exchanges[kimp::Exchange::Bithumb].enabled) {
        bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
            io_context, config.exchanges[kimp::Exchange::Bithumb]);
        spdlog::info("Bithumb enabled");
    }

    if (config.exchanges.count(kimp::Exchange::Bybit) &&
        config.exchanges[kimp::Exchange::Bybit].enabled) {
        bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges[kimp::Exchange::Bybit]);
        spdlog::info("Bybit enabled");
    }

    if (!bithumb || !bybit) {
        spdlog::error("Both Bithumb and Bybit must be enabled for arbitrage");
        return 1;
    }

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

    // Auto-trading callbacks
    // IMPORTANT: For perfect hedge, execute futures FIRST to get exact contract size
    //
    // Entry sequence:
    //   1. SHORT futures (Bybit) - get exact coin amount from contract size
    //   2. BUY spot (Bithumb) - buy exact same amount
    //
    // Exit sequence:
    //   1. COVER futures (Bybit) - close short position
    //   2. SELL spot (Bithumb) - sell exact same amount

    engine.set_entry_callback([&](const kimp::ArbitrageSignal& signal) {
        if (engine.get_position_count() >= kimp::TradingConfig::MAX_POSITIONS) {
            spdlog::warn("Max positions reached, skipping entry for {}", signal.symbol.to_string());
            return;
        }

        spdlog::info("ENTRY SIGNAL: {} premium={:.2f}%", signal.symbol.to_string(), signal.premium);

        // Execute with futures-first order for perfect hedge
        auto result = order_manager.execute_entry_futures_first(signal);
        if (result.success) {
            engine.open_position(result.position);
            spdlog::info("Position OPENED: {} | SHORT {} coins on Bybit, BUY same on Bithumb",
                         signal.symbol.to_string(), result.position.foreign_amount);
        } else {
            spdlog::error("Entry FAILED: {}", result.error_message);
        }
    });

    engine.set_exit_callback([&](const kimp::ExitSignal& signal) {
        spdlog::info("EXIT SIGNAL: {} premium={:.2f}%", signal.symbol.to_string(), signal.premium);

        kimp::Position closed_pos;
        if (!engine.close_position(signal.symbol, closed_pos)) {
            spdlog::warn("No position to close for {}", signal.symbol.to_string());
            return;
        }

        // Execute with futures-first order for perfect hedge
        auto result = order_manager.execute_exit_futures_first(signal, closed_pos);
        if (result.success) {
            spdlog::info("Position CLOSED: {} | P&L: {:.0f} KRW",
                         signal.symbol.to_string(), result.position.realized_pnl_krw);
        } else {
            spdlog::error("Exit FAILED: {}", result.error_message);
        }
    });

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
        return 1;
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
                            "{{\"s\":\"{}/{}\",\"kp\":{:.2f},\"fp\":{:.8f},\"r\":{:.4f},\"pm\":{:.4f},\"fr\":{:.8f},\"fi\":{},\"sg\":{}}}",
                            p.symbol.get_base(), p.symbol.get_quote(),
                            p.korean_price, p.foreign_price, p.usdt_rate, p.premium,
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

    // Create lookup set for fast matching
    std::unordered_set<std::string> common_bases;
    for (const auto& s : common_symbols) {
        common_bases.insert(std::string(s.get_base()));
    }

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

    // Subscribe to tickers (WebSocket for real-time updates)
    // Dedicated broadcast thread handles WebSocket push every 50ms
    bithumb->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });

    std::vector<kimp::SymbolId> bithumb_subs = common_symbols;
    bithumb_subs.emplace_back("USDT", "KRW");  // For exchange rate
    bithumb->subscribe_ticker(bithumb_subs);
    spdlog::info("Subscribed to {} Bithumb tickers (including USDT/KRW)", bithumb_subs.size());

    bybit->set_ticker_callback([&engine](const kimp::Ticker& ticker) {
        engine.on_ticker_update(ticker);
    });

    std::vector<kimp::SymbolId> bybit_subs;
    for (const auto& s : common_symbols) {
        bybit_subs.emplace_back(std::string(s.get_base()), "USDT");
    }
    bybit->subscribe_ticker(bybit_subs);
    spdlog::info("Subscribed to {} Bybit tickers", bybit_subs.size());

    // Start engine
    engine.start();

    spdlog::info("=== Bot Running (Auto-Trading ENABLED) ===");
    spdlog::info("Entry: premium <= {:.1f}% | Exit: premium >= {:.1f}%",
                 kimp::TradingConfig::ENTRY_PREMIUM_THRESHOLD,
                 kimp::TradingConfig::EXIT_PREMIUM_THRESHOLD);
    spdlog::info("Press Ctrl+C to stop");

    // Main loop with console output
    int tick = 0;
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++tick;

        // Print premium table every 2 seconds (only in monitor mode)
        if (monitor_mode && tick % 2 == 0) {
            auto premiums = engine.get_all_premiums();
            if (!premiums.empty()) {
                // Sort by premium (lowest first for entry opportunities)
                std::sort(premiums.begin(), premiums.end(),
                    [](const auto& a, const auto& b) { return a.premium < b.premium; });

                // Clear screen and print header
                std::cout << "\033[2J\033[H";  // Clear screen
                std::cout << fmt::format("=== KIMP Premium Monitor | {} symbols | USDT: {:.2f} KRW ===\n",
                    premiums.size(), premiums[0].usdt_rate);
                std::cout << fmt::format("{:<12} {:>14} {:>14} {:>10} {:>8} {:>6}\n",
                    "Symbol", "KR Price", "Foreign", "Premium", "Fund%", "Int");
                std::cout << std::string(70, '-') << "\n";

                // Print top 10 lowest (entry candidates) in GREEN
                std::cout << "\033[32m";  // Green
                int count = 0;
                for (const auto& p : premiums) {
                    if (count++ >= 10) break;
                    std::cout << fmt::format("{:<12} {:>14.2f} {:>14.6f} {:>9.4f}% {:>7.4f}% {:>5}h\n",
                        p.symbol.get_base(), p.korean_price, p.foreign_price,
                        p.premium, p.funding_rate * 100, p.funding_interval_hours);
                }
                std::cout << "\033[0m";  // Reset

                // Print separator
                std::cout << std::string(70, '-') << "\n";

                // Print top 10 highest (exit candidates) in RED
                std::cout << "\033[31m";  // Red
                count = 0;
                for (auto it = premiums.rbegin(); it != premiums.rend() && count < 10; ++it, ++count) {
                    std::cout << fmt::format("{:<12} {:>14.2f} {:>14.6f} {:>9.4f}% {:>7.4f}% {:>5}h\n",
                        it->symbol.get_base(), it->korean_price, it->foreign_price,
                        it->premium, it->funding_rate * 100, it->funding_interval_hours);
                }
                std::cout << "\033[0m";  // Reset

                std::cout << std::string(70, '-') << "\n";
                std::cout << fmt::format("Positions: {}/{} | Entry: <={:.2f}% | Exit: >={:.2f}%\n",
                    engine.get_position_count(), kimp::TradingConfig::MAX_POSITIONS,
                    kimp::TradingConfig::ENTRY_PREMIUM_THRESHOLD,
                    kimp::TradingConfig::EXIT_PREMIUM_THRESHOLD);
                std::cout << "Press Ctrl+C to stop\n";
            }
        }

        // Non-monitor mode: periodic status log
        if (!monitor_mode && tick % 30 == 0) {
            spdlog::info("Status: positions={}/{}", engine.get_position_count(),
                         kimp::TradingConfig::MAX_POSITIONS);
        }
    }

    // Shutdown
    spdlog::info("Shutting down...");

    // Stop broadcast thread first
    broadcast_running = false;
    if (broadcast_thread.joinable()) {
        broadcast_thread.join();
    }

    ws_server->stop();  // Stop WebSocket server
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
