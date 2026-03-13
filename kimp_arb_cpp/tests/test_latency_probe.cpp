#include "kimp/core/latency_probe.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> split_csv_row(const std::string& row) {
    std::vector<std::string> cols;
    std::string current;
    for (char ch : row) {
        if (ch == ',') {
            cols.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    cols.push_back(current);
    return cols;
}

int64_t to_i64(const std::string& value) {
    return std::stoll(value);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    auto benchmark = kimp::LatencyProbe::benchmark_clock_sources(50000);
    require(!benchmark.empty(), "benchmark returned no clock candidates");

    const auto unique_id = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto base_dir = std::filesystem::temp_directory_path() /
                          ("kimp_latency_probe_" + unique_id);
    std::filesystem::create_directories(base_dir);

    const auto events_path = (base_dir / "latency_events.csv").string();
    const auto summary_path = (base_dir / "latency_summary.csv").string();

    auto& probe = kimp::LatencyProbe::instance();
    kimp::LatencyProbeStartOptions options;
    options.enabled = true;
    options.events_path = events_path;
    options.summary_path = summary_path;
    options.output_mode = kimp::LatencyOutputMode::CsvText;
    options.summary_enabled = true;
    probe.start(std::move(options));
    require(probe.enabled(), "latency probe did not enable");
    bool chosen_clock_seen = false;
    for (const auto& row : benchmark) {
        if (row.source == probe.clock_source()) {
            chosen_clock_seen = true;
            break;
        }
    }
    require(chosen_clock_seen, "selected clock source missing from benchmark list");

    const kimp::SymbolId symbol("BTC", "KRW");
    const uint64_t trace_id = probe.next_trace_id();
    const uint64_t trace_start_ns = probe.capture_now_ns();

    auto emit = [&](kimp::LatencyStage stage) {
        probe.record(trace_id, symbol, stage, trace_start_ns);
        std::this_thread::sleep_for(50us);
    };

    emit(kimp::LatencyStage::SignalDetected);
    emit(kimp::LatencyStage::LifecycleEnqueued);
    emit(kimp::LatencyStage::LifecyclePickedUp);
    emit(kimp::LatencyStage::EntryLoopStart);
    emit(kimp::LatencyStage::EntryForeignSubmitStart);
    emit(kimp::LatencyStage::EntryForeignSubmitAck);
    emit(kimp::LatencyStage::EntryForeignFillQueryStart);
    emit(kimp::LatencyStage::EntryForeignFillWorkerStart);
    emit(kimp::LatencyStage::EntryForeignFillDone);
    emit(kimp::LatencyStage::EntryKoreanSubmitStart);
    emit(kimp::LatencyStage::EntryKoreanSubmitAck);
    emit(kimp::LatencyStage::EntryKoreanFillQueryStart);
    emit(kimp::LatencyStage::EntryKoreanFillWorkerStart);
    emit(kimp::LatencyStage::EntryKoreanFillDone);
    emit(kimp::LatencyStage::EntryCompleted);

    const uint64_t exit_trace_id = probe.next_trace_id();
    const uint64_t exit_trace_start_ns = probe.capture_now_ns();
    std::this_thread::sleep_for(200us);
    auto emit_exit = [&](kimp::LatencyStage stage) {
        probe.record(exit_trace_id, symbol, stage, exit_trace_start_ns);
        std::this_thread::sleep_for(50us);
    };

    emit_exit(kimp::LatencyStage::ExitLoopStart);
    emit_exit(kimp::LatencyStage::ExitForeignSubmitStart);
    emit_exit(kimp::LatencyStage::ExitForeignSubmitAck);
    emit_exit(kimp::LatencyStage::ExitForeignFillQueryStart);
    emit_exit(kimp::LatencyStage::ExitForeignFillWorkerStart);
    emit_exit(kimp::LatencyStage::ExitForeignFillDone);
    emit_exit(kimp::LatencyStage::ExitKoreanSubmitStart);
    emit_exit(kimp::LatencyStage::ExitKoreanSubmitAck);
    emit_exit(kimp::LatencyStage::ExitKoreanFillQueryStart);
    emit_exit(kimp::LatencyStage::ExitKoreanFillWorkerStart);
    emit_exit(kimp::LatencyStage::ExitKoreanFillDone);
    emit_exit(kimp::LatencyStage::ExitCompleted);

    probe.stop();
    require(probe.dropped_events() == 0, "latency probe dropped events during test");

    {
        std::ifstream events_in(events_path);
        require(events_in.is_open(), "failed to open latency events file");

        std::string header;
        std::getline(events_in, header);
        require(header.find("monotonic_ns") != std::string::npos, "events header missing monotonic_ns");
        require(header.find("delta_ns") != std::string::npos, "events header missing delta_ns");

        int row_count = 0;
        std::string row;
        while (std::getline(events_in, row)) {
            if (!row.empty()) {
                ++row_count;
            }
        }
        require(row_count == 27, "unexpected latency event row count");
    }

    {
        std::ifstream summary_in(summary_path);
        require(summary_in.is_open(), "failed to open latency summary file");

        std::string header;
        std::vector<std::string> rows;
        std::getline(summary_in, header);
        std::string row;
        while (std::getline(summary_in, row)) {
            if (!row.empty()) {
                rows.push_back(row);
            }
        }

        require(header.find("entry_foreign_fill_queue_ns") != std::string::npos,
                "summary header missing fill queue columns");
        require(header.find("entry_loop_prep_ns") != std::string::npos,
                "summary header missing entry loop prep column");
        require(header.find("exit_loop_prep_ns") != std::string::npos,
                "summary header missing exit loop prep column");
        require(header.find("reentry_loop_prep_ns") != std::string::npos,
                "summary header missing reentry loop prep column");
        require(rows.size() == 2, "expected two summary rows");

        const auto entry_cols = split_csv_row(rows[0]);
        require(entry_cols.size() >= 35, "entry summary row missing expected columns");
        require(entry_cols[2] == "BTC/KRW", "entry summary symbol mismatch");
        require(entry_cols[3] == "entry_completed", "entry summary terminal stage mismatch");
        require(to_i64(entry_cols[4]) >= 0, "entry summary total_ns invalid");
        require(to_i64(entry_cols[5]) >= 0, "entry summary detect_to_enqueue_ns invalid");
        require(to_i64(entry_cols[6]) >= 0, "entry summary queue_to_worker_ns invalid");
        require(to_i64(entry_cols[8]) >= 0, "entry summary entry_loop_prep_ns invalid");
        require(to_i64(entry_cols[9]) >= 0, "entry summary entry_foreign_submit_ns invalid");
        require(to_i64(entry_cols[10]) >= 0, "entry summary entry_foreign_fill_queue_ns invalid");
        require(to_i64(entry_cols[11]) >= 0, "entry summary entry_foreign_fill_ns invalid");
        require(to_i64(entry_cols[13]) >= 0, "entry summary entry_korean_submit_ns invalid");
        require(to_i64(entry_cols[14]) >= 0, "entry summary entry_korean_fill_queue_ns invalid");
        require(to_i64(entry_cols[15]) >= 0, "entry summary entry_korean_fill_ns invalid");
        require(to_i64(entry_cols[16]) >= 0, "entry summary entry_total_ns invalid");
        require(to_i64(entry_cols[25]) == -1, "entry summary exit_total_ns should be unset");

        const auto exit_cols = split_csv_row(rows[1]);
        require(exit_cols.size() >= 35, "exit summary row missing expected columns");
        require(exit_cols[2] == "BTC/KRW", "exit summary symbol mismatch");
        require(exit_cols[3] == "exit_completed", "exit summary terminal stage mismatch");
        require(to_i64(exit_cols[4]) > 0, "exit summary total_ns invalid");
        require(to_i64(exit_cols[17]) >= 0, "exit summary exit_loop_prep_ns invalid");
        require(to_i64(exit_cols[18]) >= 0, "exit summary exit_foreign_submit_ns invalid");
        require(to_i64(exit_cols[19]) >= 0, "exit summary exit_foreign_fill_queue_ns invalid");
        require(to_i64(exit_cols[20]) >= 0, "exit summary exit_foreign_fill_ns invalid");
        require(to_i64(exit_cols[22]) >= 0, "exit summary exit_korean_submit_ns invalid");
        require(to_i64(exit_cols[23]) >= 0, "exit summary exit_korean_fill_queue_ns invalid");
        require(to_i64(exit_cols[24]) >= 0, "exit summary exit_korean_fill_ns invalid");
        require(to_i64(exit_cols[25]) > 0, "exit summary exit_total_ns invalid");
        require(to_i64(exit_cols[25]) < to_i64(exit_cols[4]),
                "exit_total_ns should measure only the exit segment span");
        require(to_i64(exit_cols[16]) == -1, "exit summary entry_total_ns should be unset");
    }

    std::filesystem::remove_all(base_dir);
    std::cout << "*** PASS: latency probe writes nanosecond events and summary spans ***\n";
    return 0;
}
