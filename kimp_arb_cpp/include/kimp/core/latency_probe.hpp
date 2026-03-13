#pragma once

#include "kimp/core/types.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kimp {

using LatencySymbol = std::array<char, 24>;

enum class LatencyStage : uint8_t {
    SignalDetected = 0,
    LifecycleEnqueued,
    LifecyclePickedUp,
    EntryLoopStart,
    EntryForeignSubmitStart,
    EntryForeignSubmitAck,
    EntryForeignFillQueryStart,
    EntryForeignFillWorkerStart,
    EntryForeignFillDone,
    EntryKoreanSubmitStart,
    EntryKoreanSubmitAck,
    EntryKoreanFillQueryStart,
    EntryKoreanFillWorkerStart,
    EntryKoreanFillDone,
    EntryCompleted,
    ExitLoopStart,
    ExitForeignSubmitStart,
    ExitForeignSubmitAck,
    ExitForeignFillQueryStart,
    ExitForeignFillWorkerStart,
    ExitForeignFillDone,
    ExitKoreanSubmitStart,
    ExitKoreanSubmitAck,
    ExitKoreanFillQueryStart,
    ExitKoreanFillWorkerStart,
    ExitKoreanFillDone,
    ExitCompleted,
    ReentryLoopStart,
    ReentryForeignSubmitStart,
    ReentryForeignSubmitAck,
    ReentryForeignFillQueryStart,
    ReentryForeignFillWorkerStart,
    ReentryForeignFillDone,
    ReentryKoreanSubmitStart,
    ReentryKoreanSubmitAck,
    ReentryKoreanFillQueryStart,
    ReentryKoreanFillWorkerStart,
    ReentryKoreanFillDone,
    ReentryCompleted,
    Error,
    Count,
};

const char* latency_stage_name(LatencyStage stage) noexcept;

enum class LatencyClockSource : uint8_t {
    SteadyClock = 0,
#if defined(__APPLE__)
    ClockUptimeRaw,
    ClockMonotonicRaw,
    MachAbsoluteTime,
#elif defined(__linux__)
    ClockMonotonic,
    ClockMonotonicRaw,
#endif
};

const char* latency_clock_source_name(LatencyClockSource source) noexcept;

enum class LatencyOutputMode : uint8_t {
    CsvText = 0,
    Binary,
    MmapBinary,
};

const char* latency_output_mode_name(LatencyOutputMode mode) noexcept;

struct LatencyClockBenchmarkResult {
    LatencyClockSource source{LatencyClockSource::SteadyClock};
    double ns_per_call{0.0};
};

struct LatencyProbeStartOptions {
    bool enabled{false};
    std::string events_path{"trade_logs/latency_events.mmapbin"};
    std::string summary_path{"trade_logs/latency_summary.csv"};
    LatencyOutputMode output_mode{LatencyOutputMode::MmapBinary};
    bool summary_enabled{false};
    std::size_t mmap_initial_bytes{64ULL << 20};
    bool benchmark_clock_on_start{true};
};

struct LatencyEvent {
    uint64_t run_id{0};
    uint64_t trace_id{0};
    uint64_t monotonic_ns{0};
    uint64_t delta_ns{0};
    LatencyStage stage{LatencyStage::SignalDetected};
    int64_t aux0{0};
    int64_t aux1{0};
    double value0{0.0};
    double value1{0.0};
    LatencySymbol symbol{};
};

class LatencyProbe {
public:
    static LatencyProbe& instance();

    void start(bool enabled,
               std::string events_path = "trade_logs/latency_events.mmapbin",
               std::string summary_path = "trade_logs/latency_summary.csv");
    void start(LatencyProbeStartOptions options);
    void stop();

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t next_trace_id() noexcept {
        return next_trace_id_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t run_id() const noexcept {
        return run_id_.load(std::memory_order_acquire);
    }

    [[nodiscard]] LatencyClockSource clock_source() const noexcept {
        return clock_source_;
    }

    [[nodiscard]] double clock_cost_ns() const noexcept {
        return clock_cost_ns_;
    }

    [[nodiscard]] LatencyOutputMode output_mode() const noexcept {
        return output_mode_;
    }

    [[nodiscard]] bool summary_enabled() const noexcept {
        return summary_enabled_;
    }

    [[nodiscard]] uint64_t capture_now_ns() const noexcept {
        auto reader = clock_reader_;
        return reader != nullptr ? reader() : read_clock_ns(clock_source_);
    }

    [[nodiscard]] uint64_t dropped_events() const noexcept {
        return dropped_events_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static LatencySymbol format_symbol_fast(const SymbolId& symbol);

    static std::vector<LatencyClockBenchmarkResult> benchmark_clock_sources(
        std::size_t iterations = 1000000);

    void record(uint64_t trace_id,
                const SymbolId& symbol,
                LatencyStage stage,
                uint64_t trace_start_ns,
                int64_t aux0 = 0,
                int64_t aux1 = 0,
                double value0 = 0.0,
                double value1 = 0.0);
    void record(uint64_t trace_id,
                const LatencySymbol& symbol,
                LatencyStage stage,
                uint64_t trace_start_ns,
                int64_t aux0 = 0,
                int64_t aux1 = 0,
                double value0 = 0.0,
                double value1 = 0.0);
    void record_at_ns(uint64_t trace_id,
                      const SymbolId& symbol,
                      LatencyStage stage,
                      uint64_t trace_start_ns,
                      uint64_t now_ns,
                      int64_t aux0 = 0,
                      int64_t aux1 = 0,
                      double value0 = 0.0,
                      double value1 = 0.0);
    void record_at_ns(uint64_t trace_id,
                      const LatencySymbol& symbol,
                      LatencyStage stage,
                      uint64_t trace_start_ns,
                      uint64_t now_ns,
                      int64_t aux0 = 0,
                      int64_t aux1 = 0,
                      double value0 = 0.0,
                      double value1 = 0.0);

private:
    LatencyProbe() = default;
    ~LatencyProbe();

    LatencyProbe(const LatencyProbe&) = delete;
    LatencyProbe& operator=(const LatencyProbe&) = delete;

    struct TraceSummary {
        std::array<int64_t, static_cast<std::size_t>(LatencyStage::Count)> stage_ns{};
        LatencySymbol symbol{};

        TraceSummary() {
            stage_ns.fill(-1);
        }
    };

    struct ProducerQueueSlot {
        memory::SPSCRingBuffer<LatencyEvent, 1 << 14> queue;
        std::atomic<bool> claimed{false};
    };

    void exporter_loop();
    void open_outputs();
    void close_outputs();
    void write_event_row(const LatencyEvent& event);
    void write_summary_row(uint64_t trace_id,
                           LatencyStage terminal_stage,
                           const TraceSummary& summary);
    void write_binary_header();
    void ensure_mmap_capacity(std::size_t additional_bytes);
    void close_events_output();
    void close_summary_output();
    void publish_event(LatencyEvent&& event);
    bool try_pop_event(LatencyEvent& event, std::size_t& producer_cursor);
    std::size_t acquire_producer_queue_slot(uint64_t generation);
    static LatencySymbol format_symbol(const SymbolId& symbol);
    static LatencyClockSource choose_best_clock_source(
        const std::vector<LatencyClockBenchmarkResult>& results) noexcept;
    static uint64_t (*reader_for_clock_source(LatencyClockSource source) noexcept)() noexcept;
    static uint64_t read_clock_ns(LatencyClockSource source) noexcept;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_trace_id_{1};
    std::atomic<uint64_t> run_id_{0};
    std::atomic<std::size_t> pending_events_{0};
    std::atomic<uint64_t> dropped_events_{0};

    std::array<ProducerQueueSlot, 16> producer_queues_{};
    std::mutex producer_registration_mutex_;
    std::atomic<std::size_t> producer_queue_count_{0};
    std::atomic<uint64_t> run_generation_{1};
    memory::MPMCRingBuffer<LatencyEvent, 1 << 15> overflow_queue_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::thread exporter_thread_;

    std::string events_path_;
    std::string summary_path_;
    std::FILE* events_file_{nullptr};
    std::ofstream events_out_;
    std::ofstream summary_out_;
    std::array<char, 1 << 20> events_buffer_{};
    std::array<char, 1 << 16> summary_buffer_{};
    std::unordered_map<uint64_t, TraceSummary> summaries_;
    LatencyOutputMode output_mode_{LatencyOutputMode::MmapBinary};
    bool summary_enabled_{false};
    std::size_t mmap_initial_bytes_{64ULL << 20};
    int events_fd_{-1};
    char* events_mmap_ptr_{nullptr};
    std::size_t events_mmap_capacity_{0};
    std::size_t events_mmap_used_{0};
    LatencyClockSource clock_source_{LatencyClockSource::SteadyClock};
    uint64_t (*clock_reader_)() noexcept{nullptr};
    double clock_cost_ns_{0.0};
};

}  // namespace kimp
