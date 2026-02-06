#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <iostream>

namespace kimp {

class Logger {
public:
    static bool init(const std::string& log_file = "logs/kimp_bot.log",
                     const std::string& level = "info",
                     int max_size_mb = 100,
                     int max_files = 10,
                     int queue_size = 8192,
                     bool console_output = true) {
        try {
            // Initialize async logging
            spdlog::init_thread_pool(queue_size, 1);

            // Create sinks
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, max_size_mb * 1024 * 1024, max_files);

            // Create multi-sink logger (console optional for TUI mode)
            std::vector<spdlog::sink_ptr> sinks;
            if (console_output) {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                sinks.push_back(console_sink);
            }
            sinks.push_back(file_sink);

            // overrun_oldest: drop oldest log instead of blocking the hot path
            auto logger = std::make_shared<spdlog::async_logger>(
                "kimp", sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);

            // Set pattern with microseconds
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%l%$] [%t] %v");

            // Set level
            if (level == "trace") logger->set_level(spdlog::level::trace);
            else if (level == "debug") logger->set_level(spdlog::level::debug);
            else if (level == "info") logger->set_level(spdlog::level::info);
            else if (level == "warn") logger->set_level(spdlog::level::warn);
            else if (level == "error") logger->set_level(spdlog::level::err);
            else if (level == "critical") logger->set_level(spdlog::level::critical);
            else logger->set_level(spdlog::level::info);

            // Register as default logger
            spdlog::set_default_logger(logger);
            spdlog::flush_every(std::chrono::seconds(1));
            // Flush immediately on warn+ to ensure critical messages are persisted
            spdlog::flush_on(spdlog::level::warn);

            spdlog::info("Logger initialized - level: {}, file: {}", level, log_file);
            return true;
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
            return false;
        }
    }

    static void shutdown() {
        spdlog::shutdown();
    }

    // Convenience methods
    template<typename... Args>
    static void trace(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::trace(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::debug(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void info(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::info(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::warn(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::error(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void critical(fmt::format_string<Args...> fmt, Args&&... args) {
        spdlog::critical(fmt, std::forward<Args>(args)...);
    }
};

// Macros for file/line info in debug builds
#ifdef DEBUG
#define LOG_TRACE(...) spdlog::trace("[{}:{}] " __VA_ARGS__, __FILE__, __LINE__)
#define LOG_DEBUG(...) spdlog::debug("[{}:{}] " __VA_ARGS__, __FILE__, __LINE__)
#else
#define LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#endif

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)

} // namespace kimp
