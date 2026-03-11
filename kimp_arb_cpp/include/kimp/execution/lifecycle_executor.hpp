#pragma once

#include "kimp/core/optimization.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace kimp::execution {

struct LifecycleExecutorOptions {
    std::size_t worker_count{1};
    uint32_t push_spin_count{256};
    uint32_t empty_spin_count{2048};
    std::chrono::microseconds idle_wait{150};
};

template <typename Task, std::size_t Capacity = 64>
class LifecycleExecutor {
public:
    using TaskHandler = std::function<void(Task&&, std::size_t)>;
    using WorkerInit = std::function<void(std::size_t)>;

    explicit LifecycleExecutor(TaskHandler handler)
        : handler_(std::move(handler)) {}

    ~LifecycleExecutor() {
        stop();
    }

    LifecycleExecutor(const LifecycleExecutor&) = delete;
    LifecycleExecutor& operator=(const LifecycleExecutor&) = delete;

    void start(LifecycleExecutorOptions options = {}, WorkerInit worker_init = {}) {
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        options_ = options;
        if (options_.worker_count == 0) {
            options_.worker_count = 1;
        }
        worker_init_ = std::move(worker_init);

        workers_.reserve(options_.worker_count);
        for (std::size_t i = 0; i < options_.worker_count; ++i) {
            workers_.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    void stop() {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        if (!was_running && workers_.empty()) {
            return;
        }

        wake_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        pending_.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool enqueue(const Task& task) {
        return enqueue_impl(task);
    }

    [[nodiscard]] bool enqueue(Task&& task) {
        return enqueue_impl(std::move(task));
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        return pending_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return workers_.size();
    }

private:
    template <typename U>
    bool enqueue_impl(U&& task) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }

        for (uint32_t i = 0; i < options_.push_spin_count; ++i) {
            if (queue_.try_push(std::forward<U>(task))) {
                pending_.fetch_add(1, std::memory_order_release);
                wake_cv_.notify_one();
                return true;
            }
            opt::cpu_pause();
        }

        if (!queue_.try_push(std::forward<U>(task))) {
            return false;
        }

        pending_.fetch_add(1, std::memory_order_release);
        wake_cv_.notify_one();
        return true;
    }

    void worker_loop(std::size_t worker_index) {
        if (worker_init_) {
            worker_init_(worker_index);
        }

        while (true) {
            if (drain_one(worker_index)) {
                continue;
            }

            if (!running_.load(std::memory_order_acquire) && queue_.empty()) {
                break;
            }

            for (uint32_t i = 0; i < options_.empty_spin_count; ++i) {
                if (drain_one(worker_index)) {
                    break;
                }
                if (!running_.load(std::memory_order_acquire) && queue_.empty()) {
                    return;
                }
                opt::cpu_pause();
            }

            if (!queue_.empty()) {
                continue;
            }
            if (!running_.load(std::memory_order_acquire) && queue_.empty()) {
                break;
            }

            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, options_.idle_wait, [this]() {
                return !running_.load(std::memory_order_acquire) || !queue_.empty();
            });
        }
    }

    bool drain_one(std::size_t worker_index) {
        auto task = queue_.try_pop();
        if (!task) {
            return false;
        }

        pending_.fetch_sub(1, std::memory_order_acq_rel);
        handler_(std::move(*task), worker_index);
        return true;
    }

    TaskHandler handler_;
    WorkerInit worker_init_;
    LifecycleExecutorOptions options_{};
    memory::MPMCRingBuffer<Task, Capacity> queue_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> pending_{0};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::vector<std::thread> workers_;
};

}  // namespace kimp::execution
