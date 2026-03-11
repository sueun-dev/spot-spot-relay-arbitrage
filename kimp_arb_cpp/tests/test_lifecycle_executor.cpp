#include "kimp/execution/lifecycle_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

struct TestTask {
    int id{0};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using kimp::execution::LifecycleExecutor;
    using kimp::execution::LifecycleExecutorOptions;

    std::cout << "=== Lifecycle Executor Regression Test ===\n";

    std::atomic<int> completed{0};
    std::mutex ids_mutex;
    std::set<std::thread::id> worker_ids;
    std::mutex seen_mutex;
    std::vector<int> seen_ids;

    LifecycleExecutor<TestTask, 32> executor(
        [&](TestTask&& task, std::size_t) {
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                worker_ids.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(5ms);
            {
                std::lock_guard<std::mutex> lock(seen_mutex);
                seen_ids.push_back(task.id);
            }
            completed.fetch_add(1, std::memory_order_release);
        });

    LifecycleExecutorOptions options;
    options.worker_count = 2;
    options.push_spin_count = 128;
    options.empty_spin_count = 256;
    options.idle_wait = std::chrono::microseconds(200);
    executor.start(options);

    for (int i = 0; i < 8; ++i) {
        const bool ok = executor.enqueue(TestTask{i});
        assert(ok);
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (completed.load(std::memory_order_acquire) < 8 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    executor.stop();

    assert(completed.load(std::memory_order_acquire) == 8);
    assert(worker_ids.size() <= 2);
    assert(executor.pending() == 0);

    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        std::sort(seen_ids.begin(), seen_ids.end());
        assert(seen_ids.size() == 8);
        for (int i = 0; i < 8; ++i) {
            assert(seen_ids[static_cast<std::size_t>(i)] == i);
        }
    }

    std::cout << "*** PASS: fixed workers reused and queue drained cleanly ***\n";
    return 0;
}
