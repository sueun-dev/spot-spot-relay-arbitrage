#include "kimp/core/latency_probe.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

namespace kimp {

namespace {

uint64_t steady_clock_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

#if defined(__APPLE__)
uint64_t clock_uptime_raw_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t clock_monotonic_raw_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

uint64_t mach_absolute_ns() noexcept {
    static const mach_timebase_info_data_t timebase = []() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const uint64_t ticks = mach_absolute_time();
    if (timebase.numer == timebase.denom) {
        return ticks;
    }
    if (timebase.denom == 1) {
        return ticks * static_cast<uint64_t>(timebase.numer);
    }
    return static_cast<uint64_t>(
        (static_cast<__uint128_t>(ticks) * static_cast<__uint128_t>(timebase.numer)) /
        static_cast<__uint128_t>(timebase.denom));
}
#elif defined(__linux__)
uint64_t clock_monotonic_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t clock_monotonic_raw_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}
#endif

struct ClockCandidate {
    LatencyClockSource source;
    uint64_t (*reader)() noexcept;
};

std::vector<ClockCandidate> available_clock_candidates() {
    std::vector<ClockCandidate> candidates;
    candidates.push_back({LatencyClockSource::SteadyClock, &steady_clock_ns});
#if defined(__APPLE__)
    candidates.push_back({LatencyClockSource::ClockUptimeRaw, &clock_uptime_raw_ns});
    candidates.push_back({LatencyClockSource::ClockMonotonicRaw, &clock_monotonic_raw_ns});
    candidates.push_back({LatencyClockSource::MachAbsoluteTime, &mach_absolute_ns});
#elif defined(__linux__)
    candidates.push_back({LatencyClockSource::ClockMonotonic, &clock_monotonic_ns});
    candidates.push_back({LatencyClockSource::ClockMonotonicRaw, &clock_monotonic_raw_ns});
#endif
    return candidates;
}

bool file_has_content(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) &&
           std::filesystem::file_size(path, ec) > 0;
}

bool is_terminal_stage(LatencyStage stage) {
    return stage == LatencyStage::EntryCompleted ||
           stage == LatencyStage::ExitCompleted ||
           stage == LatencyStage::ReentryCompleted ||
           stage == LatencyStage::Error;
}

constexpr std::size_t kBinaryHeaderBytes = 64;

struct LatencyBinaryHeader {
    char magic[8];
    uint32_t version;
    uint32_t event_size;
    uint32_t clock_source;
    uint32_t output_mode;
    uint64_t run_id;
    uint64_t reserved0;
    uint64_t reserved1;
    uint64_t reserved2;
    uint64_t reserved3;
};

std::size_t page_align(std::size_t bytes) {
    long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }
    const std::size_t page = static_cast<std::size_t>(page_size);
    return ((bytes + page - 1) / page) * page;
}

template <typename Reader>
double benchmark_reader_ns(Reader reader, std::size_t iterations) {
    volatile uint64_t sink = 0;
    for (std::size_t i = 0; i < 10000; ++i) {
        sink ^= reader();
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        sink ^= reader();
    }
    const auto end = std::chrono::steady_clock::now();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    (void)sink;

    return std::chrono::duration<double, std::nano>(end - start).count() /
           static_cast<double>(iterations);
}

struct ProducerSlotCache {
    uint64_t generation{0};
    int slot{-1};
};

thread_local ProducerSlotCache g_producer_slot_cache{};

}  // namespace

const char* latency_stage_name(LatencyStage stage) noexcept {
    switch (stage) {
        case LatencyStage::SignalDetected: return "signal_detected";
        case LatencyStage::LifecycleEnqueued: return "lifecycle_enqueued";
        case LatencyStage::LifecyclePickedUp: return "lifecycle_picked_up";
        case LatencyStage::EntryLoopStart: return "entry_loop_start";
        case LatencyStage::EntryForeignSubmitStart: return "entry_foreign_submit_start";
        case LatencyStage::EntryForeignSubmitAck: return "entry_foreign_submit_ack";
        case LatencyStage::EntryForeignFillQueryStart: return "entry_foreign_fill_query_start";
        case LatencyStage::EntryForeignFillWorkerStart: return "entry_foreign_fill_worker_start";
        case LatencyStage::EntryForeignFillDone: return "entry_foreign_fill_done";
        case LatencyStage::EntryKoreanSubmitStart: return "entry_korean_submit_start";
        case LatencyStage::EntryKoreanSubmitAck: return "entry_korean_submit_ack";
        case LatencyStage::EntryKoreanFillQueryStart: return "entry_korean_fill_query_start";
        case LatencyStage::EntryKoreanFillWorkerStart: return "entry_korean_fill_worker_start";
        case LatencyStage::EntryKoreanFillDone: return "entry_korean_fill_done";
        case LatencyStage::EntryCompleted: return "entry_completed";
        case LatencyStage::ExitLoopStart: return "exit_loop_start";
        case LatencyStage::ExitForeignSubmitStart: return "exit_foreign_submit_start";
        case LatencyStage::ExitForeignSubmitAck: return "exit_foreign_submit_ack";
        case LatencyStage::ExitForeignFillQueryStart: return "exit_foreign_fill_query_start";
        case LatencyStage::ExitForeignFillWorkerStart: return "exit_foreign_fill_worker_start";
        case LatencyStage::ExitForeignFillDone: return "exit_foreign_fill_done";
        case LatencyStage::ExitKoreanSubmitStart: return "exit_korean_submit_start";
        case LatencyStage::ExitKoreanSubmitAck: return "exit_korean_submit_ack";
        case LatencyStage::ExitKoreanFillQueryStart: return "exit_korean_fill_query_start";
        case LatencyStage::ExitKoreanFillWorkerStart: return "exit_korean_fill_worker_start";
        case LatencyStage::ExitKoreanFillDone: return "exit_korean_fill_done";
        case LatencyStage::ExitCompleted: return "exit_completed";
        case LatencyStage::ReentryLoopStart: return "reentry_loop_start";
        case LatencyStage::ReentryForeignSubmitStart: return "reentry_foreign_submit_start";
        case LatencyStage::ReentryForeignSubmitAck: return "reentry_foreign_submit_ack";
        case LatencyStage::ReentryForeignFillQueryStart: return "reentry_foreign_fill_query_start";
        case LatencyStage::ReentryForeignFillWorkerStart: return "reentry_foreign_fill_worker_start";
        case LatencyStage::ReentryForeignFillDone: return "reentry_foreign_fill_done";
        case LatencyStage::ReentryKoreanSubmitStart: return "reentry_korean_submit_start";
        case LatencyStage::ReentryKoreanSubmitAck: return "reentry_korean_submit_ack";
        case LatencyStage::ReentryKoreanFillQueryStart: return "reentry_korean_fill_query_start";
        case LatencyStage::ReentryKoreanFillWorkerStart: return "reentry_korean_fill_worker_start";
        case LatencyStage::ReentryKoreanFillDone: return "reentry_korean_fill_done";
        case LatencyStage::ReentryCompleted: return "reentry_completed";
        case LatencyStage::Error: return "error";
        default: return "unknown";
    }
}

const char* latency_clock_source_name(LatencyClockSource source) noexcept {
    switch (source) {
        case LatencyClockSource::SteadyClock: return "std::chrono::steady_clock";
#if defined(__APPLE__)
        case LatencyClockSource::ClockUptimeRaw: return "clock_gettime_nsec_np(CLOCK_UPTIME_RAW)";
        case LatencyClockSource::ClockMonotonicRaw: return "clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)";
        case LatencyClockSource::MachAbsoluteTime: return "mach_absolute_time";
#elif defined(__linux__)
        case LatencyClockSource::ClockMonotonic: return "clock_gettime(CLOCK_MONOTONIC)";
        case LatencyClockSource::ClockMonotonicRaw: return "clock_gettime(CLOCK_MONOTONIC_RAW)";
#endif
        default: return "unknown";
    }
}

const char* latency_output_mode_name(LatencyOutputMode mode) noexcept {
    switch (mode) {
        case LatencyOutputMode::CsvText: return "csv";
        case LatencyOutputMode::Binary: return "binary";
        case LatencyOutputMode::MmapBinary: return "mmap";
        default: return "unknown";
    }
}

LatencyProbe& LatencyProbe::instance() {
    static LatencyProbe probe;
    return probe;
}

LatencyProbe::~LatencyProbe() {
    stop();
}

LatencySymbol LatencyProbe::format_symbol_fast(const SymbolId& symbol) {
    return format_symbol(symbol);
}

void LatencyProbe::start(bool enabled, std::string events_path, std::string summary_path) {
    LatencyProbeStartOptions options;
    options.enabled = enabled;
    options.events_path = std::move(events_path);
    options.summary_path = std::move(summary_path);
    start(std::move(options));
}

void LatencyProbe::start(LatencyProbeStartOptions options) {
    stop();

    enabled_.store(options.enabled, std::memory_order_release);
    if (!options.enabled) {
        return;
    }

    events_path_ = std::move(options.events_path);
    summary_path_ = std::move(options.summary_path);
    output_mode_ = options.output_mode;
    summary_enabled_ = options.summary_enabled;
    mmap_initial_bytes_ = std::max<std::size_t>(options.mmap_initial_bytes, page_align(kBinaryHeaderBytes + sizeof(LatencyEvent) * 1024));
    pending_events_.store(0, std::memory_order_release);
    dropped_events_.store(0, std::memory_order_release);
    producer_queue_count_.store(0, std::memory_order_release);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    for (auto& slot : producer_queues_) {
        slot.claimed.store(false, std::memory_order_release);
        slot.queue.reset();
    }
    run_id_.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()), std::memory_order_release);
    next_trace_id_.store(1, std::memory_order_release);
    summaries_.clear();
    summaries_.reserve(512);

    if (options.benchmark_clock_on_start) {
        const auto clock_results = benchmark_clock_sources();
        clock_source_ = choose_best_clock_source(clock_results);
        clock_cost_ns_ = 0.0;
        for (const auto& result : clock_results) {
            if (result.source == clock_source_) {
                clock_cost_ns_ = result.ns_per_call;
                break;
            }
        }
    } else {
        clock_source_ = LatencyClockSource::SteadyClock;
        clock_cost_ns_ = 0.0;
    }
    clock_reader_ = reader_for_clock_source(clock_source_);

    std::error_code ec;
    if (const auto events_parent = std::filesystem::path(events_path_).parent_path();
        !events_parent.empty()) {
        std::filesystem::create_directories(events_parent, ec);
    }
    if (summary_enabled_) {
        if (const auto summary_parent = std::filesystem::path(summary_path_).parent_path();
            !summary_parent.empty()) {
            std::filesystem::create_directories(summary_parent, ec);
        }
    }

    open_outputs();
    running_.store(true, std::memory_order_release);
    exporter_thread_ = std::thread(&LatencyProbe::exporter_loop, this);
}

void LatencyProbe::stop() {
    enabled_.store(false, std::memory_order_release);
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) {
        if (exporter_thread_.joinable()) {
            exporter_thread_.join();
        }
        return;
    }

    wake_cv_.notify_all();
    if (exporter_thread_.joinable()) {
        exporter_thread_.join();
    }

    close_outputs();
    producer_queue_count_.store(0, std::memory_order_release);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    for (auto& slot : producer_queues_) {
        slot.claimed.store(false, std::memory_order_release);
        slot.queue.reset();
    }
}

std::vector<LatencyClockBenchmarkResult> LatencyProbe::benchmark_clock_sources(
    std::size_t iterations) {
    std::vector<LatencyClockBenchmarkResult> results;
    const auto candidates = available_clock_candidates();
    results.reserve(candidates.size());
    const std::size_t effective_iterations = std::max<std::size_t>(iterations, 10000);

    for (const auto& candidate : candidates) {
        results.push_back({
            candidate.source,
            benchmark_reader_ns(candidate.reader, effective_iterations),
        });
    }

    std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.ns_per_call < rhs.ns_per_call;
    });
    return results;
}

void LatencyProbe::record(uint64_t trace_id,
                          const SymbolId& symbol,
                          LatencyStage stage,
                          uint64_t trace_start_ns,
                          int64_t aux0,
                          int64_t aux1,
                          double value0,
                          double value1) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    // Capture clock BEFORE format_symbol to exclude formatting overhead from timestamp
    const uint64_t now = capture_now_ns();
    record_at_ns(trace_id,
                 format_symbol(symbol),
                 stage,
                 trace_start_ns,
                 now,
                 aux0,
                 aux1,
                 value0,
                 value1);
}

void LatencyProbe::record(uint64_t trace_id,
                          const LatencySymbol& symbol,
                          LatencyStage stage,
                          uint64_t trace_start_ns,
                          int64_t aux0,
                          int64_t aux1,
                          double value0,
                          double value1) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    record_at_ns(trace_id,
                 symbol,
                 stage,
                 trace_start_ns,
                 capture_now_ns(),
                 aux0,
                 aux1,
                 value0,
                 value1);
}

void LatencyProbe::record_at_ns(uint64_t trace_id,
                                const SymbolId& symbol,
                                LatencyStage stage,
                                uint64_t trace_start_ns,
                                uint64_t now_ns,
                                int64_t aux0,
                                int64_t aux1,
                                double value0,
                                double value1) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    record_at_ns(trace_id,
                 format_symbol(symbol),
                 stage,
                 trace_start_ns,
                 now_ns,
                 aux0,
                 aux1,
                 value0,
                 value1);
}

void LatencyProbe::record_at_ns(uint64_t trace_id,
                                const LatencySymbol& symbol,
                                LatencyStage stage,
                                uint64_t trace_start_ns,
                                uint64_t now_ns,
                                int64_t aux0,
                                int64_t aux1,
                                double value0,
                                double value1) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    LatencyEvent event;
    event.run_id = run_id_.load(std::memory_order_relaxed);
    event.trace_id = trace_id;
    event.monotonic_ns = now_ns;
    event.delta_ns = trace_start_ns > 0 && now_ns >= trace_start_ns
        ? (now_ns - trace_start_ns)
        : 0;
    event.stage = stage;
    event.aux0 = aux0;
    event.aux1 = aux1;
    event.value0 = value0;
    event.value1 = value1;
    event.symbol = symbol;

    publish_event(std::move(event));
}

void LatencyProbe::publish_event(LatencyEvent&& event) {
    const uint64_t generation = run_generation_.load(std::memory_order_relaxed);
    const std::size_t slot = acquire_producer_queue_slot(generation);

    bool pushed = false;
    if (slot < producer_queues_.size()) {
        pushed = producer_queues_[slot].queue.try_push(std::move(event));
    }
    if (!pushed) {
        pushed = overflow_queue_.try_push(std::move(event));
    }
    if (!pushed) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (pending_events_.fetch_add(1, std::memory_order_relaxed) == 0) {
        wake_cv_.notify_one();
    }
}

bool LatencyProbe::try_pop_event(LatencyEvent& event, std::size_t& producer_cursor) {
    const std::size_t producer_count = producer_queue_count_.load(std::memory_order_acquire);
    if (producer_count > 0) {
        for (std::size_t offset = 0; offset < producer_count; ++offset) {
            const std::size_t idx = (producer_cursor + offset) % producer_count;
            if (!producer_queues_[idx].claimed.load(std::memory_order_acquire)) {
                continue;
            }
            auto item = producer_queues_[idx].queue.try_pop();
            if (item) {
                event = std::move(*item);
                producer_cursor = (idx + 1) % producer_count;
                return true;
            }
        }
    }

    auto overflow_item = overflow_queue_.try_pop();
    if (!overflow_item) {
        return false;
    }
    event = std::move(*overflow_item);
    return true;
}

std::size_t LatencyProbe::acquire_producer_queue_slot(uint64_t generation) {
    if (g_producer_slot_cache.generation == generation) {
        return g_producer_slot_cache.slot >= 0
            ? static_cast<std::size_t>(g_producer_slot_cache.slot)
            : static_cast<std::size_t>(-1);
    }

    std::lock_guard lock(producer_registration_mutex_);
    if (g_producer_slot_cache.generation == generation) {
        return g_producer_slot_cache.slot >= 0
            ? static_cast<std::size_t>(g_producer_slot_cache.slot)
            : static_cast<std::size_t>(-1);
    }

    for (std::size_t i = 0; i < producer_queues_.size(); ++i) {
        bool expected = false;
        if (!producer_queues_[i].claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            continue;
        }
        const std::size_t current_count = producer_queue_count_.load(std::memory_order_relaxed);
        if (i + 1 > current_count) {
            producer_queue_count_.store(i + 1, std::memory_order_release);
        }
        g_producer_slot_cache = ProducerSlotCache{
            generation,
            static_cast<int>(i),
        };
        return i;
    }

    g_producer_slot_cache = ProducerSlotCache{
        generation,
        -1,
    };
    return static_cast<std::size_t>(-1);
}

void LatencyProbe::exporter_loop() {
    std::vector<LatencyEvent> batch;
    batch.reserve(256);
    std::size_t producer_cursor = 0;
    LatencyEvent event;

    while (running_.load(std::memory_order_acquire) ||
           pending_events_.load(std::memory_order_relaxed) > 0) {
        batch.clear();
        while (batch.size() < 256) {
            if (!try_pop_event(event, producer_cursor)) {
                break;
            }
            batch.push_back(std::move(event));
        }

        if (batch.empty()) {
            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                return !running_.load(std::memory_order_acquire) ||
                       pending_events_.load(std::memory_order_relaxed) > 0;
            });
            continue;
        }

        pending_events_.fetch_sub(batch.size(), std::memory_order_relaxed);

        for (const auto& event : batch) {
            write_event_row(event);

            if (summary_enabled_) {
                auto& summary = summaries_[event.trace_id];
                if (summary.symbol[0] == '\0') {
                    summary.symbol = event.symbol;
                }
                const auto stage_index = static_cast<std::size_t>(event.stage);
                if (stage_index < summary.stage_ns.size()) {
                    summary.stage_ns[stage_index] = static_cast<int64_t>(event.delta_ns);
                }

                if (is_terminal_stage(event.stage)) {
                    write_summary_row(event.trace_id, event.stage, summary);
                    summaries_.erase(event.trace_id);
                }
            }
        }

        if (events_out_.is_open()) {
            events_out_.flush();
        }
        if (events_file_ != nullptr) {
            std::fflush(events_file_);
        }
        if (summary_out_.is_open()) {
            summary_out_.flush();
        }
    }
}

void LatencyProbe::open_outputs() {
    const bool events_need_header = !file_has_content(events_path_);
    const bool summary_need_header = summary_enabled_ && !file_has_content(summary_path_);

    switch (output_mode_) {
        case LatencyOutputMode::CsvText:
            events_out_.open(events_path_, std::ios::app);
            if (events_out_.is_open()) {
                events_out_.rdbuf()->pubsetbuf(events_buffer_.data(), static_cast<std::streamsize>(events_buffer_.size()));
                if (events_need_header) {
                    events_out_ << "run_id,trace_id,symbol,stage,monotonic_ns,delta_ns,aux0,aux1,value0,value1\n";
                }
            }
            break;
        case LatencyOutputMode::Binary:
            events_file_ = std::fopen(events_path_.c_str(), events_need_header ? "wb" : "ab");
            if (events_file_ != nullptr) {
                std::setvbuf(events_file_, events_buffer_.data(), _IOFBF, events_buffer_.size());
                if (events_need_header) {
                    write_binary_header();
                }
            }
            break;
        case LatencyOutputMode::MmapBinary: {
            events_fd_ = ::open(events_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (events_fd_ >= 0) {
                events_mmap_capacity_ = page_align(mmap_initial_bytes_);
                ::ftruncate(events_fd_, static_cast<off_t>(events_mmap_capacity_));
                events_mmap_ptr_ = static_cast<char*>(::mmap(
                    nullptr,
                    events_mmap_capacity_,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    events_fd_,
                    0));
                if (events_mmap_ptr_ == MAP_FAILED) {
                    events_mmap_ptr_ = nullptr;
                    ::close(events_fd_);
                    events_fd_ = -1;
                } else {
                    events_mmap_used_ = 0;
                    write_binary_header();
                }
            }
            break;
        }
    }

    if (!summary_enabled_) {
        return;
    }

    summary_out_.open(summary_path_, std::ios::app);
    if (summary_out_.is_open()) {
        summary_out_.rdbuf()->pubsetbuf(summary_buffer_.data(), static_cast<std::streamsize>(summary_buffer_.size()));
        if (summary_need_header) {
            summary_out_
                << "run_id,trace_id,symbol,terminal_stage,total_ns,detect_to_enqueue_ns,"
                   "queue_to_worker_ns,worker_to_entry_loop_ns,entry_loop_prep_ns,entry_foreign_submit_ns,"
                   "entry_foreign_fill_queue_ns,entry_foreign_fill_ns,"
                   "entry_foreign_to_korean_gap_ns,entry_korean_submit_ns,"
                   "entry_korean_fill_queue_ns,entry_korean_fill_ns,entry_total_ns,"
                   "exit_loop_prep_ns,exit_foreign_submit_ns,exit_foreign_fill_queue_ns,exit_foreign_fill_ns,"
                   "exit_foreign_to_korean_gap_ns,exit_korean_submit_ns,"
                   "exit_korean_fill_queue_ns,exit_korean_fill_ns,exit_total_ns,"
                   "reentry_loop_prep_ns,reentry_foreign_submit_ns,reentry_foreign_fill_queue_ns,reentry_foreign_fill_ns,"
                   "reentry_foreign_to_korean_gap_ns,reentry_korean_submit_ns,"
                   "reentry_korean_fill_queue_ns,reentry_korean_fill_ns,reentry_total_ns\n";
        }
    }
}

void LatencyProbe::close_outputs() {
    close_events_output();
    close_summary_output();
}

void LatencyProbe::write_event_row(const LatencyEvent& event) {
    switch (output_mode_) {
        case LatencyOutputMode::CsvText:
            if (events_out_.is_open()) {
                events_out_ << event.run_id << ','
                            << event.trace_id << ','
                            << event.symbol.data() << ','
                            << latency_stage_name(event.stage) << ','
                            << event.monotonic_ns << ','
                            << event.delta_ns << ','
                            << event.aux0 << ','
                            << event.aux1 << ','
                            << std::fixed << std::setprecision(8) << event.value0 << ','
                            << event.value1 << '\n';
            }
            break;
        case LatencyOutputMode::Binary:
            if (events_file_ != nullptr) {
                std::fwrite(&event, sizeof(event), 1, events_file_);
            }
            break;
        case LatencyOutputMode::MmapBinary:
            if (events_mmap_ptr_ != nullptr) {
                ensure_mmap_capacity(sizeof(event));
                std::memcpy(events_mmap_ptr_ + events_mmap_used_, &event, sizeof(event));
                events_mmap_used_ += sizeof(event);
            }
            break;
    }
}

void LatencyProbe::write_summary_row(uint64_t trace_id,
                                     LatencyStage terminal_stage,
                                     const TraceSummary& summary) {
    auto stage_ns = [&](LatencyStage stage) -> int64_t {
        return summary.stage_ns[static_cast<std::size_t>(stage)];
    };
    auto span = [&](LatencyStage start, LatencyStage end) -> int64_t {
        const int64_t a = stage_ns(start);
        const int64_t b = stage_ns(end);
        if (a < 0 || b < 0 || b < a) {
            return -1;
        }
        return b - a;
    };

    const int64_t total_ns = stage_ns(terminal_stage);
    const int64_t detect_to_enqueue = span(LatencyStage::SignalDetected, LatencyStage::LifecycleEnqueued);
    const int64_t queue_to_worker = span(LatencyStage::LifecycleEnqueued, LatencyStage::LifecyclePickedUp);
    const int64_t worker_to_entry_loop = span(LatencyStage::LifecyclePickedUp, LatencyStage::EntryLoopStart);
    const int64_t entry_loop_prep = span(LatencyStage::EntryLoopStart, LatencyStage::EntryForeignSubmitStart);
    const int64_t entry_foreign_submit = span(LatencyStage::EntryForeignSubmitStart, LatencyStage::EntryForeignSubmitAck);
    const int64_t entry_foreign_fill_queue = span(LatencyStage::EntryForeignFillQueryStart, LatencyStage::EntryForeignFillWorkerStart);
    const int64_t entry_foreign_fill = span(LatencyStage::EntryForeignFillWorkerStart, LatencyStage::EntryForeignFillDone);
    const int64_t entry_foreign_to_korean_gap = span(LatencyStage::EntryForeignSubmitAck, LatencyStage::EntryKoreanSubmitStart);
    const int64_t entry_korean_submit = span(LatencyStage::EntryKoreanSubmitStart, LatencyStage::EntryKoreanSubmitAck);
    const int64_t entry_korean_fill_queue = span(LatencyStage::EntryKoreanFillQueryStart, LatencyStage::EntryKoreanFillWorkerStart);
    const int64_t entry_korean_fill = span(LatencyStage::EntryKoreanFillWorkerStart, LatencyStage::EntryKoreanFillDone);
    const int64_t entry_total = span(LatencyStage::SignalDetected, LatencyStage::EntryCompleted);

    const int64_t exit_loop_prep = span(LatencyStage::ExitLoopStart, LatencyStage::ExitForeignSubmitStart);
    const int64_t exit_foreign_submit = span(LatencyStage::ExitForeignSubmitStart, LatencyStage::ExitForeignSubmitAck);
    const int64_t exit_foreign_fill_queue = span(LatencyStage::ExitForeignFillQueryStart, LatencyStage::ExitForeignFillWorkerStart);
    const int64_t exit_foreign_fill = span(LatencyStage::ExitForeignFillWorkerStart, LatencyStage::ExitForeignFillDone);
    const int64_t exit_foreign_to_korean_gap = span(LatencyStage::ExitForeignSubmitAck, LatencyStage::ExitKoreanSubmitStart);
    const int64_t exit_korean_submit = span(LatencyStage::ExitKoreanSubmitStart, LatencyStage::ExitKoreanSubmitAck);
    const int64_t exit_korean_fill_queue = span(LatencyStage::ExitKoreanFillQueryStart, LatencyStage::ExitKoreanFillWorkerStart);
    const int64_t exit_korean_fill = span(LatencyStage::ExitKoreanFillWorkerStart, LatencyStage::ExitKoreanFillDone);
    const int64_t exit_total = span(LatencyStage::ExitLoopStart, LatencyStage::ExitCompleted);

    const int64_t reentry_loop_prep = span(LatencyStage::ReentryLoopStart, LatencyStage::ReentryForeignSubmitStart);
    const int64_t reentry_foreign_submit = span(LatencyStage::ReentryForeignSubmitStart, LatencyStage::ReentryForeignSubmitAck);
    const int64_t reentry_foreign_fill_queue = span(LatencyStage::ReentryForeignFillQueryStart, LatencyStage::ReentryForeignFillWorkerStart);
    const int64_t reentry_foreign_fill = span(LatencyStage::ReentryForeignFillWorkerStart, LatencyStage::ReentryForeignFillDone);
    const int64_t reentry_foreign_to_korean_gap = span(LatencyStage::ReentryForeignSubmitAck, LatencyStage::ReentryKoreanSubmitStart);
    const int64_t reentry_korean_submit = span(LatencyStage::ReentryKoreanSubmitStart, LatencyStage::ReentryKoreanSubmitAck);
    const int64_t reentry_korean_fill_queue = span(LatencyStage::ReentryKoreanFillQueryStart, LatencyStage::ReentryKoreanFillWorkerStart);
    const int64_t reentry_korean_fill = span(LatencyStage::ReentryKoreanFillWorkerStart, LatencyStage::ReentryKoreanFillDone);
    const int64_t reentry_total = span(LatencyStage::ReentryLoopStart, LatencyStage::ReentryCompleted);

    if (!summary_out_.is_open()) {
        return;
    }

    summary_out_ << run_id_.load(std::memory_order_relaxed) << ','
                 << trace_id << ','
                 << summary.symbol.data() << ','
                 << latency_stage_name(terminal_stage) << ','
                 << total_ns << ','
                 << detect_to_enqueue << ','
                 << queue_to_worker << ','
                 << worker_to_entry_loop << ','
                 << entry_loop_prep << ','
                 << entry_foreign_submit << ','
                 << entry_foreign_fill_queue << ','
                 << entry_foreign_fill << ','
                 << entry_foreign_to_korean_gap << ','
                 << entry_korean_submit << ','
                 << entry_korean_fill_queue << ','
                 << entry_korean_fill << ','
                 << entry_total << ','
                 << exit_loop_prep << ','
                 << exit_foreign_submit << ','
                 << exit_foreign_fill_queue << ','
                 << exit_foreign_fill << ','
                 << exit_foreign_to_korean_gap << ','
                 << exit_korean_submit << ','
                 << exit_korean_fill_queue << ','
                 << exit_korean_fill << ','
                 << exit_total << ','
                 << reentry_loop_prep << ','
                 << reentry_foreign_submit << ','
                 << reentry_foreign_fill_queue << ','
                 << reentry_foreign_fill << ','
                 << reentry_foreign_to_korean_gap << ','
                 << reentry_korean_submit << ','
                 << reentry_korean_fill_queue << ','
                 << reentry_korean_fill << ','
                 << reentry_total << '\n';
}

void LatencyProbe::write_binary_header() {
    LatencyBinaryHeader header{};
    std::memcpy(header.magic, "KLATv1", 7);
    header.magic[7] = '\0';
    header.version = 1;
    header.event_size = static_cast<uint32_t>(sizeof(LatencyEvent));
    header.clock_source = static_cast<uint32_t>(clock_source_);
    header.output_mode = static_cast<uint32_t>(output_mode_);
    header.run_id = run_id_.load(std::memory_order_relaxed);

    switch (output_mode_) {
        case LatencyOutputMode::CsvText:
            break;
        case LatencyOutputMode::Binary:
            if (events_file_ != nullptr) {
                std::fwrite(&header, sizeof(header), 1, events_file_);
            }
            break;
        case LatencyOutputMode::MmapBinary:
            if (events_mmap_ptr_ != nullptr) {
                ensure_mmap_capacity(sizeof(header));
                std::memcpy(events_mmap_ptr_ + events_mmap_used_, &header, sizeof(header));
                events_mmap_used_ += sizeof(header);
            }
            break;
    }
}

void LatencyProbe::ensure_mmap_capacity(std::size_t additional_bytes) {
    if (events_mmap_ptr_ == nullptr || events_fd_ < 0) {
        return;
    }

    const std::size_t required = events_mmap_used_ + additional_bytes;
    if (required <= events_mmap_capacity_) {
        return;
    }

    std::size_t new_capacity = events_mmap_capacity_ > 0 ? events_mmap_capacity_ : page_align(mmap_initial_bytes_);
    while (new_capacity < required) {
        new_capacity *= 2;
    }
    new_capacity = page_align(new_capacity);

    ::msync(events_mmap_ptr_, events_mmap_used_, MS_ASYNC);
    ::munmap(events_mmap_ptr_, events_mmap_capacity_);
    ::ftruncate(events_fd_, static_cast<off_t>(new_capacity));
    events_mmap_ptr_ = static_cast<char*>(::mmap(
        nullptr,
        new_capacity,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        events_fd_,
        0));
    if (events_mmap_ptr_ == MAP_FAILED) {
        events_mmap_ptr_ = nullptr;
        events_mmap_capacity_ = 0;
        return;
    }
    events_mmap_capacity_ = new_capacity;
}

void LatencyProbe::close_events_output() {
    if (events_out_.is_open()) {
        events_out_.flush();
        events_out_.close();
    }

    if (events_file_ != nullptr) {
        std::fflush(events_file_);
        std::fclose(events_file_);
        events_file_ = nullptr;
    }

    if (events_mmap_ptr_ != nullptr && events_fd_ >= 0) {
        ::msync(events_mmap_ptr_, events_mmap_used_, MS_SYNC);
        ::munmap(events_mmap_ptr_, events_mmap_capacity_);
        events_mmap_ptr_ = nullptr;
        ::ftruncate(events_fd_, static_cast<off_t>(events_mmap_used_));
        ::close(events_fd_);
        events_fd_ = -1;
        events_mmap_capacity_ = 0;
        events_mmap_used_ = 0;
    }
}

void LatencyProbe::close_summary_output() {
    if (summary_out_.is_open()) {
        summary_out_.flush();
        summary_out_.close();
    }
}

LatencySymbol LatencyProbe::format_symbol(const SymbolId& symbol) {
    LatencySymbol out{};
    size_t offset = 0;
    auto append = [&](std::string_view part) {
        const size_t remaining = out.size() > offset ? out.size() - offset - 1 : 0;
        const size_t copy_len = std::min(part.size(), remaining);
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

LatencyClockSource LatencyProbe::choose_best_clock_source(
    const std::vector<LatencyClockBenchmarkResult>& results) noexcept {
    if (results.empty()) {
        return LatencyClockSource::SteadyClock;
    }
    return std::min_element(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.ns_per_call < rhs.ns_per_call;
    })->source;
}

uint64_t (*LatencyProbe::reader_for_clock_source(LatencyClockSource source) noexcept)() noexcept {
    switch (source) {
        case LatencyClockSource::SteadyClock:
            return &steady_clock_ns;
#if defined(__APPLE__)
        case LatencyClockSource::ClockUptimeRaw:
            return &clock_uptime_raw_ns;
        case LatencyClockSource::ClockMonotonicRaw:
            return &clock_monotonic_raw_ns;
        case LatencyClockSource::MachAbsoluteTime:
            return &mach_absolute_ns;
#elif defined(__linux__)
        case LatencyClockSource::ClockMonotonic:
            return &clock_monotonic_ns;
        case LatencyClockSource::ClockMonotonicRaw:
            return &clock_monotonic_raw_ns;
#endif
        default:
            return &steady_clock_ns;
    }
}

uint64_t LatencyProbe::read_clock_ns(LatencyClockSource source) noexcept {
    return reader_for_clock_source(source)();
}

}  // namespace kimp
