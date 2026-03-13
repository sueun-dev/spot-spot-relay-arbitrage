#include "kimp/core/latency_probe.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

std::array<char, 24> format_symbol_like_probe(const kimp::SymbolId& symbol) {
    std::array<char, 24> out{};
    std::size_t offset = 0;

    auto append = [&](std::string_view part) {
        const std::size_t remaining = out.size() > offset ? out.size() - offset - 1 : 0;
        const std::size_t copy_len = std::min(part.size(), remaining);
        if (copy_len > 0) {
            std::copy_n(part.data(), copy_len, out.data() + offset);
            offset += copy_len;
        }
    };

    append(symbol.get_base());
    if (offset + 1 < out.size()) {
        out[offset++] = '/';
    }
    append(symbol.get_quote());
    out[std::min(offset, out.size() - 1)] = '\0';
    return out;
}

struct SummaryLike {
    std::array<int64_t, static_cast<std::size_t>(kimp::LatencyStage::Count)> stage_ns{};
    std::array<char, 24> symbol{};

    SummaryLike() {
        stage_ns.fill(-1);
    }
};

struct QueueBenchResult {
    std::string label;
    std::size_t producers{0};
    double producer_ns_per_event{0.0};
    double total_ns_per_event{0.0};
};

kimp::LatencyEvent make_event_template(const std::array<char, 24>& symbol, uint64_t trace_id) {
    kimp::LatencyEvent event;
    event.run_id = 1;
    event.trace_id = trace_id;
    event.monotonic_ns = 123456789;
    event.delta_ns = 321;
    event.stage = kimp::LatencyStage::EntryForeignSubmitAck;
    event.aux0 = 11;
    event.aux1 = 22;
    event.value0 = 33.0;
    event.value1 = 44.0;
    event.symbol = symbol;
    return event;
}

QueueBenchResult bench_mpmc(std::size_t producers, std::size_t events_per_producer) {
    static constexpr std::size_t kCapacity = 1 << 15;
    auto queue = std::make_unique<kimp::memory::MPMCRingBuffer<kimp::LatencyEvent, kCapacity>>();
    std::atomic<bool> start{false};
    std::atomic<std::size_t> consumed{0};
    const std::size_t total_events = producers * events_per_producer;
    const auto symbol = format_symbol_like_probe(kimp::SymbolId("BTC", "KRW"));

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (consumed.load(std::memory_order_acquire) < total_events) {
            auto item = queue->try_pop();
            if (item) {
                consumed.fetch_add(1, std::memory_order_release);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::thread> threads;
    threads.reserve(producers);
    const auto producer_begin = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < producers; ++worker) {
        threads.emplace_back([&, worker]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t i = 0; i < events_per_producer; ++i) {
                auto event = make_event_template(symbol, 1 + worker * events_per_producer + i);
                while (!queue->try_push(std::move(event))) {
                    std::this_thread::yield();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }
    const auto producer_end = std::chrono::steady_clock::now();
    consumer.join();
    const auto total_end = std::chrono::steady_clock::now();

    return {
        "mpmc",
        producers,
        std::chrono::duration<double, std::nano>(producer_end - producer_begin).count() /
            static_cast<double>(total_events),
        std::chrono::duration<double, std::nano>(total_end - producer_begin).count() /
            static_cast<double>(total_events),
    };
}

QueueBenchResult bench_spsc_1p1c(std::size_t events_per_producer) {
    static constexpr std::size_t kCapacity = 1 << 15;
    auto queue = std::make_unique<kimp::memory::SPSCRingBuffer<kimp::LatencyEvent, kCapacity>>();
    std::atomic<bool> start{false};
    std::atomic<std::size_t> consumed{0};
    const auto symbol = format_symbol_like_probe(kimp::SymbolId("BTC", "KRW"));

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (consumed.load(std::memory_order_acquire) < events_per_producer) {
            auto item = queue->try_pop();
            if (item) {
                consumed.fetch_add(1, std::memory_order_release);
            } else {
                std::this_thread::yield();
            }
        }
    });

    const auto producer_begin = std::chrono::steady_clock::now();
    std::thread producer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < events_per_producer; ++i) {
            auto event = make_event_template(symbol, 1 + i);
            while (!queue->try_push(std::move(event))) {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    const auto producer_end = std::chrono::steady_clock::now();
    consumer.join();
    const auto total_end = std::chrono::steady_clock::now();

    return {
        "spsc-1p1c",
        1,
        std::chrono::duration<double, std::nano>(producer_end - producer_begin).count() /
            static_cast<double>(events_per_producer),
        std::chrono::duration<double, std::nano>(total_end - producer_begin).count() /
            static_cast<double>(events_per_producer),
    };
}

QueueBenchResult bench_spsc_fanin(std::size_t producers, std::size_t events_per_producer) {
    static constexpr std::size_t kCapacity = 1 << 15;
    auto queues = std::make_unique<std::array<kimp::memory::SPSCRingBuffer<kimp::LatencyEvent, kCapacity>, 4>>();
    std::atomic<bool> start{false};
    std::atomic<std::size_t> consumed{0};
    const std::size_t total_events = producers * events_per_producer;
    const auto symbol = format_symbol_like_probe(kimp::SymbolId("BTC", "KRW"));

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::size_t cursor = 0;
        while (consumed.load(std::memory_order_acquire) < total_events) {
            bool progressed = false;
            for (std::size_t i = 0; i < producers; ++i) {
                auto& queue = (*queues)[(cursor + i) % producers];
                auto item = queue.try_pop();
                if (item) {
                    consumed.fetch_add(1, std::memory_order_release);
                    cursor = (cursor + i + 1) % producers;
                    progressed = true;
                    break;
                }
            }
            if (!progressed) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::thread> threads;
    threads.reserve(producers);
    const auto producer_begin = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < producers; ++worker) {
        threads.emplace_back([&, worker]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto& queue = (*queues)[worker];
            for (std::size_t i = 0; i < events_per_producer; ++i) {
                auto event = make_event_template(symbol, 1 + worker * events_per_producer + i);
                while (!queue.try_push(std::move(event))) {
                    std::this_thread::yield();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }
    const auto producer_end = std::chrono::steady_clock::now();
    consumer.join();
    const auto total_end = std::chrono::steady_clock::now();

    return {
        "spsc-fanin",
        producers,
        std::chrono::duration<double, std::nano>(producer_end - producer_begin).count() /
            static_cast<double>(total_events),
        std::chrono::duration<double, std::nano>(total_end - producer_begin).count() /
            static_cast<double>(total_events),
    };
}

}  // namespace

int main() {
    constexpr std::size_t component_iterations = 2000000;
    constexpr std::size_t queue_events_1p = 250000;
    constexpr std::size_t queue_events_4p = 150000;

    auto& probe = kimp::LatencyProbe::instance();
    const auto unique_id = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto base_dir = std::filesystem::temp_directory_path() /
                          ("kimp_latency_probe_hotpath_" + unique_id);
    std::filesystem::create_directories(base_dir);

    kimp::LatencyProbeStartOptions options;
    options.enabled = true;
    options.output_mode = kimp::LatencyOutputMode::MmapBinary;
    options.summary_enabled = false;
    options.events_path = (base_dir / "events.mmapbin").string();
    options.summary_path = (base_dir / "summary.csv").string();
    probe.start(std::move(options));

    const kimp::SymbolId symbol("BTC", "KRW");
    const auto preformatted_symbol = format_symbol_like_probe(symbol);
    const uint64_t trace_start_ns = probe.capture_now_ns();

    const double chosen_clock_ns = bench_ns_per_op([&]() -> uint64_t {
        return probe.capture_now_ns();
    }, component_iterations);

    const double format_symbol_ns = bench_ns_per_op([&]() -> uint64_t {
        const auto formatted = format_symbol_like_probe(symbol);
        return static_cast<uint64_t>(formatted[0]);
    }, component_iterations);

    const double event_fill_with_format_ns = bench_ns_per_op([&]() -> uint64_t {
        kimp::LatencyEvent event;
        const uint64_t now_ns = probe.capture_now_ns();
        event.run_id = 1;
        event.trace_id = 1;
        event.monotonic_ns = now_ns;
        event.delta_ns = now_ns - trace_start_ns;
        event.stage = kimp::LatencyStage::EntryForeignSubmitAck;
        event.aux0 = 11;
        event.aux1 = 22;
        event.value0 = 33.0;
        event.value1 = 44.0;
        event.symbol = format_symbol_like_probe(symbol);
        return event.delta_ns ^ static_cast<uint64_t>(event.symbol[0]);
    }, component_iterations);

    const double event_fill_cached_symbol_ns = bench_ns_per_op([&]() -> uint64_t {
        kimp::LatencyEvent event;
        const uint64_t now_ns = probe.capture_now_ns();
        event.run_id = 1;
        event.trace_id = 1;
        event.monotonic_ns = now_ns;
        event.delta_ns = now_ns - trace_start_ns;
        event.stage = kimp::LatencyStage::EntryForeignSubmitAck;
        event.aux0 = 11;
        event.aux1 = 22;
        event.value0 = 33.0;
        event.value1 = 44.0;
        event.symbol = preformatted_symbol;
        return event.delta_ns ^ static_cast<uint64_t>(event.symbol[0]);
    }, component_iterations);

    auto mpmc_queue = std::make_unique<kimp::memory::MPMCRingBuffer<kimp::LatencyEvent, 1 << 15>>();
    const auto template_event = make_event_template(preformatted_symbol, 1);
    const double mpmc_push_pop_ns = bench_ns_per_op([&]() -> uint64_t {
        auto event = template_event;
        while (!mpmc_queue->try_push(std::move(event))) {
        }
        auto popped = mpmc_queue->try_pop();
        return popped ? popped->trace_id : 0;
    }, component_iterations);

    auto spsc_queue = std::make_unique<kimp::memory::SPSCRingBuffer<kimp::LatencyEvent, 1 << 15>>();
    const double spsc_push_pop_ns = bench_ns_per_op([&]() -> uint64_t {
        auto event = template_event;
        while (!spsc_queue->try_push(std::move(event))) {
        }
        auto popped = spsc_queue->try_pop();
        return popped ? popped->trace_id : 0;
    }, component_iterations);

    std::atomic<std::size_t> pending_nonzero{1};
    const double pending_counter_ns = bench_ns_per_op([&]() -> uint64_t {
        return pending_nonzero.fetch_add(1, std::memory_order_acq_rel);
    }, component_iterations);

    std::atomic<std::size_t> pending_empty{0};
    std::condition_variable cv;
    std::mutex cv_mutex;
    const double wake_transition_ns = bench_ns_per_op([&]() -> uint64_t {
        pending_empty.store(0, std::memory_order_release);
        const auto previous = pending_empty.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0) {
            cv.notify_one();
        }
        return previous;
    }, component_iterations / 10);

    std::unordered_map<uint64_t, SummaryLike> summaries;
    summaries.reserve(1024);
    const double summary_map_ns = bench_ns_per_op([&]() -> uint64_t {
        static uint64_t trace_id = 1;
        auto& summary = summaries[trace_id];
        if (summary.symbol[0] == '\0') {
            summary.symbol = preformatted_symbol;
        }
        summary.stage_ns[static_cast<std::size_t>(kimp::LatencyStage::SignalDetected)] = 10;
        summary.stage_ns[static_cast<std::size_t>(kimp::LatencyStage::EntryCompleted)] = 100;
        summaries.erase(trace_id);
        return trace_id++;
    }, component_iterations / 4);

    probe.stop();
    std::filesystem::remove_all(base_dir);

    const auto mpmc_1p1c = bench_mpmc(1, queue_events_1p);
    const auto spsc_1p1c = bench_spsc_1p1c(queue_events_1p);
    const auto mpmc_4p1c = bench_mpmc(4, queue_events_4p);
    const auto spsc_fanin_4p1c = bench_spsc_fanin(4, queue_events_4p);

    std::cout << "=== Latency Probe Hot Path Benchmark ===\n";
    std::cout << std::left << std::setw(42) << "component"
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(14) << "ns/op\n";
    std::cout << std::left << std::setw(42) << "chosen clock read"
              << std::right << std::setw(14) << chosen_clock_ns << '\n';
    std::cout << std::left << std::setw(42) << "format_symbol"
              << std::right << std::setw(14) << format_symbol_ns << '\n';
    std::cout << std::left << std::setw(42) << "event fill + format_symbol"
              << std::right << std::setw(14) << event_fill_with_format_ns << '\n';
    std::cout << std::left << std::setw(42) << "event fill + cached symbol"
              << std::right << std::setw(14) << event_fill_cached_symbol_ns << '\n';
    std::cout << std::left << std::setw(42) << "mpmc push+pop (same thread)"
              << std::right << std::setw(14) << mpmc_push_pop_ns << '\n';
    std::cout << std::left << std::setw(42) << "spsc push+pop (same thread)"
              << std::right << std::setw(14) << spsc_push_pop_ns << '\n';
    std::cout << std::left << std::setw(42) << "pending counter hot path"
              << std::right << std::setw(14) << pending_counter_ns << '\n';
    std::cout << std::left << std::setw(42) << "empty->nonempty wake transition"
              << std::right << std::setw(14) << wake_transition_ns << '\n';
    std::cout << std::left << std::setw(42) << "summary map create/update/erase"
              << std::right << std::setw(14) << summary_map_ns << '\n';

    std::cout << "\n=== Queue Topology Benchmark ===\n";
    std::cout << std::left << std::setw(18) << "topology"
              << std::setw(12) << "producers"
              << std::setw(22) << "producer ns/event"
              << "total ns/event\n";
    for (const auto& result : {mpmc_1p1c, spsc_1p1c, mpmc_4p1c, spsc_fanin_4p1c}) {
        std::cout << std::left << std::setw(18) << result.label
                  << std::setw(12) << result.producers
                  << std::setw(22) << result.producer_ns_per_event
                  << result.total_ns_per_event << '\n';
    }

    if (mpmc_1p1c.total_ns_per_event > 0.0) {
        std::cout << "\nSPSC 1p1c speedup vs MPMC 1p1c: "
                  << std::fixed << std::setprecision(2)
                  << (mpmc_1p1c.total_ns_per_event / spsc_1p1c.total_ns_per_event) << "x\n";
    }
    if (mpmc_4p1c.total_ns_per_event > 0.0) {
        std::cout << "SPSC fan-in 4p1c speedup vs MPMC 4p1c: "
                  << std::fixed << std::setprecision(2)
                  << (mpmc_4p1c.total_ns_per_event / spsc_fanin_4p1c.total_ns_per_event) << "x\n";
    }

    return 0;
}
