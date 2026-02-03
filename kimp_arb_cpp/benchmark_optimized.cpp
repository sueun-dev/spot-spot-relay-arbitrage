// Optimized benchmark with free HFT optimizations
// Compile: clang++ -std=c++20 -O3 -march=native -flto benchmark_optimized.cpp -o benchmark_optimized -I/usr/local/include -L/usr/local/lib -lsimdjson

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <atomic>
#include <thread>
#include <array>
#include <algorithm>

#include <simdjson.h>

using namespace std::chrono;

// ============== Optimization Macros ==============
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

inline void prefetch_read(const void* ptr) {
    __builtin_prefetch(ptr, 0, 3);
}

inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// ============== Optimized Premium Calculation ==============
// Force inline, no branches
__attribute__((always_inline, hot))
inline double calculate_premium_opt(double korean_ask, double foreign_bid, double usdt_rate) noexcept {
    const double foreign_krw = foreign_bid * usdt_rate;
    return ((korean_ask - foreign_krw) / foreign_krw) * 100.0;
}

// SIMD vectorized version for batch processing (AVX2)
#if defined(__AVX2__)
#include <immintrin.h>

void calculate_premium_simd(const double* korean, const double* foreign,
                            const double* usdt, double* result, size_t count) {
    const size_t simd_count = count / 4;

    for (size_t i = 0; i < simd_count; ++i) {
        __m256d korean_v = _mm256_loadu_pd(&korean[i * 4]);
        __m256d foreign_v = _mm256_loadu_pd(&foreign[i * 4]);
        __m256d usdt_v = _mm256_loadu_pd(&usdt[i * 4]);
        __m256d hundred = _mm256_set1_pd(100.0);

        __m256d foreign_krw = _mm256_mul_pd(foreign_v, usdt_v);
        __m256d diff = _mm256_sub_pd(korean_v, foreign_krw);
        __m256d premium = _mm256_div_pd(diff, foreign_krw);
        premium = _mm256_mul_pd(premium, hundred);

        _mm256_storeu_pd(&result[i * 4], premium);
    }

    // Handle remaining elements
    for (size_t i = simd_count * 4; i < count; ++i) {
        result[i] = calculate_premium_opt(korean[i], foreign[i], usdt[i]);
    }
}
#endif

// ============== Optimized Lock-free Queue ==============
template<typename T, size_t Capacity>
class alignas(64) OptimizedSPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    alignas(64) std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) size_t cached_tail_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) size_t cached_head_{0};

public:
    bool try_push(T item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & MASK;

        if (UNLIKELY(next_head == cached_tail_)) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head == cached_tail_) {
                return false;
            }
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (UNLIKELY(tail == cached_head_)) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return false;
            }
        }

        // Prefetch next item
        prefetch_read(&buffer_[(tail + 1) & MASK]);

        item = buffer_[tail];
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }
};

void benchmark_premium_optimized() {
    std::cout << "\n=== Optimized Premium Calculation ===" << std::endl;

    const int iterations = 10'000'000;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> korean_dist(50000000, 55000000);
    std::uniform_real_distribution<> foreign_dist(35000, 40000);
    std::uniform_real_distribution<> usdt_dist(1350, 1400);

    // Aligned memory allocation
    alignas(64) std::vector<double> korean_prices(iterations);
    alignas(64) std::vector<double> foreign_prices(iterations);
    alignas(64) std::vector<double> usdt_rates(iterations);
    alignas(64) std::vector<double> results(iterations);

    for (int i = 0; i < iterations; ++i) {
        korean_prices[i] = korean_dist(gen);
        foreign_prices[i] = foreign_dist(gen);
        usdt_rates[i] = usdt_dist(gen);
    }

    // Standard version
    volatile double result = 0;
    auto start1 = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        result = calculate_premium_opt(korean_prices[i], foreign_prices[i], usdt_rates[i]);
    }
    auto end1 = high_resolution_clock::now();
    auto ns1 = duration_cast<nanoseconds>(end1 - start1).count();

    std::cout << "Standard (force-inline): " << static_cast<double>(ns1) / iterations << " ns/calc" << std::endl;

#if defined(__AVX2__)
    // SIMD version
    auto start2 = high_resolution_clock::now();
    calculate_premium_simd(korean_prices.data(), foreign_prices.data(),
                          usdt_rates.data(), results.data(), iterations);
    auto end2 = high_resolution_clock::now();
    auto ns2 = duration_cast<nanoseconds>(end2 - start2).count();

    std::cout << "SIMD (AVX2, 4x parallel): " << static_cast<double>(ns2) / iterations << " ns/calc" << std::endl;
    std::cout << "SIMD speedup: " << static_cast<double>(ns1) / ns2 << "x" << std::endl;
#else
    std::cout << "(AVX2 not available on this CPU)" << std::endl;
#endif
}

void benchmark_json_optimized() {
    std::cout << "\n=== Optimized JSON Parsing ===" << std::endl;

    // Minimal JSON for fastest parsing
    const std::string ticker_json_full = R"({"tp":52345678.0,"bp":52340000.0,"ap":52350000.0})";

    const int iterations = 1'000'000;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(ticker_json_full);

    volatile double price_sum = 0;

    // Warm up cache
    for (int i = 0; i < 1000; ++i) {
        auto doc = parser.iterate(padded);
        price_sum += doc["tp"].get_double().value();
    }

    auto start = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto doc = parser.iterate(padded);
        double price = doc["tp"].get_double().value();
        price_sum += price;
    }
    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double ns_per_parse = static_cast<double>(duration_ns) / iterations;

    std::cout << "Time per parse: " << ns_per_parse << " ns" << std::endl;
    std::cout << "Parses/sec: " << 1e9 / ns_per_parse / 1e6 << " million" << std::endl;
}

void benchmark_queue_optimized() {
    std::cout << "\n=== Optimized Lock-free Queue ===" << std::endl;

    OptimizedSPSCQueue<uint64_t, 4096> queue;
    const int iterations = 10'000'000;

    uint64_t sum = 0;

    auto start = high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < iterations; ++i) {
            while (!queue.try_push(i)) {
                cpu_pause();  // No context switch!
            }
        }
    });

    std::thread consumer([&]() {
        uint64_t value;
        int count = 0;
        while (count < iterations) {
            if (queue.try_pop(value)) {
                sum += value;
                ++count;
            } else {
                cpu_pause();  // No context switch!
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(duration_ns) / iterations;

    std::cout << "Time per push+pop: " << ns_per_op << " ns" << std::endl;
    std::cout << "Operations/sec: " << 1e9 / ns_per_op / 1e6 << " million" << std::endl;
}

void benchmark_full_pipeline() {
    std::cout << "\n=== Full Pipeline (WebSocket → Signal) ===" << std::endl;

    const std::string ticker_json = R"({"ap":52350000.0})";
    const int iterations = 100'000;
    std::vector<int64_t> latencies(iterations);

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(ticker_json);

    const double usdt_rate = 1380.0;
    const double foreign_bid = 38000.0;

    // Warm up
    for (int i = 0; i < 1000; ++i) {
        auto doc = parser.iterate(padded);
        volatile double p = doc["ap"].get_double().value();
    }

    for (int i = 0; i < iterations; ++i) {
        auto start = high_resolution_clock::now();

        // 1. Parse (simulated WebSocket message)
        auto doc = parser.iterate(padded);
        double korean_ask = doc["ap"].get_double().value();

        // 2. Calculate premium
        double premium = calculate_premium_opt(korean_ask, foreign_bid, usdt_rate);

        // 3. Check condition (branch prediction optimized)
        bool should_trade = UNLIKELY(premium <= -1.0);

        auto end = high_resolution_clock::now();
        latencies[i] = duration_cast<nanoseconds>(end - start).count();

        if (should_trade && i < 0) std::cout << premium;  // Prevent optimization
    }

    std::sort(latencies.begin(), latencies.end());

    std::cout << "Average: " << std::accumulate(latencies.begin(), latencies.end(), 0LL) / iterations << " ns" << std::endl;
    std::cout << "Median (p50): " << latencies[iterations / 2] << " ns" << std::endl;
    std::cout << "p90: " << latencies[iterations * 90 / 100] << " ns" << std::endl;
    std::cout << "p99: " << latencies[iterations * 99 / 100] << " ns" << std::endl;
    std::cout << "p99.9: " << latencies[iterations * 999 / 1000] << " ns" << std::endl;
    std::cout << "Min: " << latencies[0] << " ns" << std::endl;
    std::cout << "Max: " << latencies[iterations - 1] << " ns" << std::endl;
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   OPTIMIZED C++ HFT Benchmark - Free Optimizations Applied   ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;

    benchmark_premium_optimized();
    benchmark_json_optimized();
    benchmark_queue_optimized();
    benchmark_full_pipeline();

    std::cout << "\n=== Optimizations Applied ===" << std::endl;
    std::cout << "✓ Force-inline functions" << std::endl;
    std::cout << "✓ Branch prediction hints (LIKELY/UNLIKELY)" << std::endl;
    std::cout << "✓ Cache prefetching" << std::endl;
    std::cout << "✓ CPU pause instead of yield (no context switch)" << std::endl;
    std::cout << "✓ Cached head/tail in queue" << std::endl;
    std::cout << "✓ Memory alignment (64-byte cache line)" << std::endl;
#if defined(__AVX2__)
    std::cout << "✓ SIMD (AVX2) vectorization" << std::endl;
#endif

    return 0;
}
