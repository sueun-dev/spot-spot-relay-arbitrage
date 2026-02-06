#pragma once

/**
 * Free Performance Optimizations for HFT
 *
 * No cost, maximum speed!
 */

#include <thread>
#include <atomic>
#include <charconv>
#include <string_view>
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>  // For _mm_pause (x86 only)
#endif

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#endif

namespace kimp::opt {

// ============== Branch Prediction Hints ==============
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// ============== Cache Prefetching ==============
// Prefetch data into L1 cache before we need it
inline void prefetch_read(const void* ptr) {
    __builtin_prefetch(ptr, 0, 3);  // Read, high temporal locality
}

inline void prefetch_write(void* ptr) {
    __builtin_prefetch(ptr, 1, 3);  // Write, high temporal locality
}

// ============== CPU Yield (No Context Switch) ==============
// Use _mm_pause() instead of std::this_thread::yield()
// 140 cycles vs 10000+ cycles!
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// ============== Busy-Wait Spin Loop ==============
template<typename Predicate>
inline void spin_wait(Predicate pred) {
    while (!pred()) {
        cpu_pause();
    }
}

// With timeout (in iterations)
template<typename Predicate>
inline bool spin_wait_timeout(Predicate pred, uint64_t max_iterations) {
    for (uint64_t i = 0; i < max_iterations; ++i) {
        if (pred()) return true;
        cpu_pause();
    }
    return false;
}

// ============== CPU Pinning ==============
/**
 * Pin current thread to specific CPU core
 * Reduces context switches and cache misses
 */
inline bool pin_to_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(__APPLE__)
    // macOS doesn't support hard pinning, but we can set affinity hint
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                            (thread_policy_t)&policy, 1) == KERN_SUCCESS;
#else
    return false;
#endif
}

/**
 * Set thread priority to realtime
 */
inline bool set_realtime_priority() {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#elif defined(__APPLE__)
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    return false;
#endif
}

// ============== Memory Alignment ==============
template<typename T>
inline T* aligned_alloc(size_t count, size_t alignment = 64) {
    void* ptr = nullptr;
#ifdef _WIN32
    ptr = _aligned_malloc(count * sizeof(T), alignment);
#else
    if (posix_memalign(&ptr, alignment, count * sizeof(T)) != 0) {
        return nullptr;
    }
#endif
    return static_cast<T*>(ptr);
}

template<typename T>
inline void aligned_free(T* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

// ============== Compiler Memory Barrier ==============
inline void memory_barrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

// ============== Atomic Operations with Relaxed Ordering ==============
template<typename T>
inline T load_relaxed(const std::atomic<T>& a) {
    return a.load(std::memory_order_relaxed);
}

template<typename T>
inline void store_relaxed(std::atomic<T>& a, T value) {
    a.store(value, std::memory_order_relaxed);
}

// ============== Fast String-to-Double Conversion ==============
/**
 * Zero-allocation string to double conversion
 * Uses std::from_chars when available, falls back to strtod
 * ~5x faster than std::stod(std::string(...))
 */
inline double fast_stod(std::string_view sv, double default_val = 0.0) noexcept {
    if (sv.empty()) return default_val;

#if __cpp_lib_to_chars >= 201611L
    double result = default_val;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    return (ec == std::errc{}) ? result : default_val;
#else
    // Fallback: strtod with null-terminated copy (still faster than std::stod)
    char buf[64];
    size_t len = std::min(sv.size(), size_t{63});
    std::copy_n(sv.data(), len, buf);
    buf[len] = '\0';
    char* end;
    double result = std::strtod(buf, &end);
    return (end != buf) ? result : default_val;
#endif
}

// ============== Thread Configuration ==============
struct ThreadConfig {
    int io_upbit_core = 0;
    int io_bithumb_core = 1;
    int io_bybit_core = 2;
    int io_gateio_core = 3;
    int strategy_core = 4;
    int execution_core = 5;

    static ThreadConfig optimal() {
        ThreadConfig cfg;
        int num_cores = std::thread::hardware_concurrency();

        if (num_cores >= 8) {
            // High-end server: dedicated cores
            cfg.io_upbit_core = 0;
            cfg.io_bithumb_core = 1;
            cfg.io_bybit_core = 2;
            cfg.io_gateio_core = 3;
            cfg.strategy_core = 4;
            cfg.execution_core = 5;
        } else if (num_cores >= 4) {
            // Mid-range: share some cores
            cfg.io_upbit_core = 0;
            cfg.io_bithumb_core = 0;
            cfg.io_bybit_core = 1;
            cfg.io_gateio_core = 1;
            cfg.strategy_core = 2;
            cfg.execution_core = 3;
        } else {
            // Low-end: no pinning
            cfg.io_upbit_core = -1;
            cfg.io_bithumb_core = -1;
            cfg.io_bybit_core = -1;
            cfg.io_gateio_core = -1;
            cfg.strategy_core = -1;
            cfg.execution_core = -1;
        }

        return cfg;
    }
};

} // namespace kimp::opt
