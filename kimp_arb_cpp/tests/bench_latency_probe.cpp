#include "kimp/core/latency_probe.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename Fn>
double bench_ns_per_op(Fn&& fn, std::size_t iterations) {
    volatile uint64_t sink = 0;
    for (std::size_t i = 0; i < 10000; ++i) {
        sink ^= fn();
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        sink ^= fn();
    }
    const auto end = std::chrono::steady_clock::now();

    std::atomic_signal_fence(std::memory_order_seq_cst);
    (void)sink;
    return std::chrono::duration<double, std::nano>(end - start).count() /
           static_cast<double>(iterations);
}

uint64_t steady_epoch_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct ExportScenario {
    kimp::LatencyOutputMode output_mode;
    bool summary_enabled;
    std::string label;
};

struct ExportResult {
    std::string label;
    std::size_t producers{0};
    double produce_ns_per_event{0.0};
    double total_ns_per_event{0.0};
    uint64_t dropped{0};
};

ExportResult bench_export_scenario(const ExportScenario& scenario,
                                   std::size_t producers,
                                   std::size_t traces_per_thread) {
    static constexpr kimp::LatencyStage kStages[] = {
        kimp::LatencyStage::SignalDetected,
        kimp::LatencyStage::LifecycleEnqueued,
        kimp::LatencyStage::LifecyclePickedUp,
        kimp::LatencyStage::EntryLoopStart,
        kimp::LatencyStage::EntryForeignSubmitStart,
        kimp::LatencyStage::EntryForeignSubmitAck,
        kimp::LatencyStage::EntryForeignFillQueryStart,
        kimp::LatencyStage::EntryForeignFillWorkerStart,
        kimp::LatencyStage::EntryForeignFillDone,
        kimp::LatencyStage::EntryKoreanSubmitStart,
        kimp::LatencyStage::EntryKoreanSubmitAck,
        kimp::LatencyStage::EntryKoreanFillQueryStart,
        kimp::LatencyStage::EntryKoreanFillWorkerStart,
        kimp::LatencyStage::EntryKoreanFillDone,
        kimp::LatencyStage::EntryCompleted,
    };

    const std::size_t events_per_trace = sizeof(kStages) / sizeof(kStages[0]);
    const std::size_t total_traces = traces_per_thread * producers;
    const std::size_t total_events = total_traces * events_per_trace;

    const auto unique_id = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto base_dir = std::filesystem::temp_directory_path() /
                          ("kimp_latency_export_bench_" + unique_id);
    std::filesystem::create_directories(base_dir);

    kimp::LatencyProbeStartOptions options;
    options.enabled = true;
    options.output_mode = scenario.output_mode;
    options.summary_enabled = scenario.summary_enabled;
    options.summary_path = (base_dir / "summary.csv").string();
    options.mmap_initial_bytes = sizeof(kimp::LatencyEvent) * total_events + (8ULL << 20);
    switch (scenario.output_mode) {
        case kimp::LatencyOutputMode::CsvText:
            options.events_path = (base_dir / "events.csv").string();
            break;
        case kimp::LatencyOutputMode::Binary:
            options.events_path = (base_dir / "events.bin").string();
            break;
        case kimp::LatencyOutputMode::MmapBinary:
            options.events_path = (base_dir / "events.mmapbin").string();
            break;
    }

    auto& probe = kimp::LatencyProbe::instance();
    probe.start(std::move(options));

    const auto producer_begin = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(producers);
    for (std::size_t worker = 0; worker < producers; ++worker) {
        threads.emplace_back([&, worker]() {
            const kimp::SymbolId symbol(worker % 2 == 0 ? "BTC" : "ETH", "KRW");
            const uint64_t trace_base = 1 + static_cast<uint64_t>(worker * traces_per_thread);
            for (std::size_t trace_index = 0; trace_index < traces_per_thread; ++trace_index) {
                const uint64_t trace_id = trace_base + trace_index;
                const uint64_t trace_start_ns = probe.capture_now_ns();
                for (std::size_t stage_index = 0; stage_index < events_per_trace; ++stage_index) {
                    probe.record(trace_id,
                                 symbol,
                                 kStages[stage_index],
                                 trace_start_ns,
                                 static_cast<int64_t>(worker),
                                 static_cast<int64_t>(stage_index),
                                 static_cast<double>(trace_index),
                                 static_cast<double>(stage_index));
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    const auto producer_end = std::chrono::steady_clock::now();
    probe.stop();
    const auto total_end = std::chrono::steady_clock::now();

    std::filesystem::remove_all(base_dir);
    return {
        scenario.label,
        producers,
        std::chrono::duration<double, std::nano>(producer_end - producer_begin).count() /
            static_cast<double>(total_events),
        std::chrono::duration<double, std::nano>(total_end - producer_begin).count() /
            static_cast<double>(total_events),
        probe.dropped_events(),
    };
}

}  // namespace

int main() {
    constexpr std::size_t clock_iterations = 1000000;
    constexpr std::size_t record_iterations = 2000000;

    const auto clock_results =
        kimp::LatencyProbe::benchmark_clock_sources(clock_iterations);

    std::cout << "=== Latency Probe Clock Benchmark ===\n";
    for (const auto& result : clock_results) {
        std::cout << std::left << std::setw(64)
                  << kimp::latency_clock_source_name(result.source)
                  << std::right << std::fixed << std::setprecision(2)
                  << result.ns_per_call << " ns/op\n";
    }

    const auto unique_id = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto base_dir = std::filesystem::temp_directory_path() /
                          ("kimp_latency_probe_bench_" + unique_id);
    std::filesystem::create_directories(base_dir);

    auto& probe = kimp::LatencyProbe::instance();
    kimp::LatencyProbeStartOptions start_options;
    start_options.enabled = true;
    start_options.output_mode = kimp::LatencyOutputMode::CsvText;
    start_options.summary_enabled = false;
    start_options.events_path = (base_dir / "events.csv").string();
    start_options.summary_path = (base_dir / "summary.csv").string();
    probe.start(std::move(start_options));

    const uint64_t optimized_trace_start_ns = probe.capture_now_ns();
    const auto legacy_trace_start = std::chrono::steady_clock::now();

    const double legacy_record_ns = bench_ns_per_op([&]() -> uint64_t {
        const uint64_t monotonic_ns = steady_epoch_ns();
        const uint64_t delta_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - legacy_trace_start).count());
        return monotonic_ns ^ delta_ns;
    }, record_iterations);

    const double optimized_record_ns = bench_ns_per_op([&]() -> uint64_t {
        const uint64_t now_ns = probe.capture_now_ns();
        const uint64_t delta_ns = now_ns - optimized_trace_start_ns;
        return now_ns ^ delta_ns;
    }, record_iterations);

    probe.stop();
    std::filesystem::remove_all(base_dir);

    std::cout << "\n=== Trace Math Benchmark ===\n";
    std::cout << std::left << std::setw(64) << "legacy dual steady_clock"
              << std::right << std::fixed << std::setprecision(2)
              << legacy_record_ns << " ns/op\n";
    std::cout << std::left << std::setw(64)
              << ("optimized single " + std::string(
                      kimp::latency_clock_source_name(probe.clock_source())))
              << std::right << std::fixed << std::setprecision(2)
              << optimized_record_ns << " ns/op\n";

    if (optimized_record_ns > 0.0) {
        std::cout << std::left << std::setw(64) << "speedup"
                  << std::right << std::fixed << std::setprecision(2)
                  << (legacy_record_ns / optimized_record_ns) << "x\n";
    }

    const std::vector<ExportScenario> scenarios = {
        {kimp::LatencyOutputMode::CsvText, true, "csv + summary"},
        {kimp::LatencyOutputMode::CsvText, false, "csv raw-only"},
        {kimp::LatencyOutputMode::Binary, true, "binary + summary"},
        {kimp::LatencyOutputMode::Binary, false, "binary raw-only"},
        {kimp::LatencyOutputMode::MmapBinary, true, "mmap + summary"},
        {kimp::LatencyOutputMode::MmapBinary, false, "mmap raw-only"},
    };

    std::cout << "\n=== Export Path Benchmark ===\n";
    std::cout << std::left << std::setw(24) << "scenario"
              << std::setw(12) << "producers"
              << std::setw(20) << "produce ns/event"
              << std::setw(20) << "total ns/event"
              << "dropped\n";

    for (std::size_t producers : {std::size_t{1}, std::size_t{4}}) {
        for (const auto& scenario : scenarios) {
            const auto result = bench_export_scenario(
                scenario,
                producers,
                producers == 1 ? 30000 : 20000);

            std::cout << std::left << std::setw(24) << result.label
                      << std::setw(12) << result.producers
                      << std::setw(20) << std::fixed << std::setprecision(2)
                      << result.produce_ns_per_event
                      << std::setw(20) << result.total_ns_per_event
                      << result.dropped << '\n';
        }
    }

    return 0;
}
