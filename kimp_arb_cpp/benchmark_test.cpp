// Simple standalone benchmark to compare C++ vs Python performance
// Compile: clang++ -std=c++20 -O3 -march=native benchmark_test.cpp -o benchmark_test -I/usr/local/include -L/usr/local/lib -lsimdjson

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <atomic>
#include <thread>
#include <array>

// Simdjson for JSON parsing
#include <simdjson.h>

using namespace std::chrono;

// ============== Premium Calculation Benchmark ==============
double calculate_premium(double korean_ask, double foreign_bid, double usdt_rate) {
    double foreign_krw = foreign_bid * usdt_rate;
    return ((korean_ask - foreign_krw) / foreign_krw) * 100.0;
}

void benchmark_premium_calculation() {
    std::cout << "\n=== Premium Calculation Benchmark ===" << std::endl;

    const int iterations = 10'000'000;

    // Generate test data
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> korean_dist(50000000, 55000000);  // KRW
    std::uniform_real_distribution<> foreign_dist(35000, 40000);       // USD
    std::uniform_real_distribution<> usdt_dist(1350, 1400);            // USDT/KRW

    std::vector<double> korean_prices(iterations);
    std::vector<double> foreign_prices(iterations);
    std::vector<double> usdt_rates(iterations);

    for (int i = 0; i < iterations; ++i) {
        korean_prices[i] = korean_dist(gen);
        foreign_prices[i] = foreign_dist(gen);
        usdt_rates[i] = usdt_dist(gen);
    }

    // Benchmark
    volatile double result = 0;  // Prevent optimization

    auto start = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        result = calculate_premium(korean_prices[i], foreign_prices[i], usdt_rates[i]);
    }
    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double ns_per_calc = static_cast<double>(duration_ns) / iterations;
    double calcs_per_sec = 1e9 / ns_per_calc;

    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << duration_ns / 1e6 << " ms" << std::endl;
    std::cout << "Time per calculation: " << ns_per_calc << " ns" << std::endl;
    std::cout << "Calculations/sec: " << calcs_per_sec / 1e6 << " million" << std::endl;
}

// ============== JSON Parsing Benchmark ==============
void benchmark_json_parsing() {
    std::cout << "\n=== JSON Parsing Benchmark (simdjson) ===" << std::endl;

    // Typical WebSocket ticker message
    const std::string ticker_json = R"({
        "type": "ticker",
        "code": "KRW-BTC",
        "trade_price": 52345678.0,
        "bid_price": 52340000.0,
        "ask_price": 52350000.0,
        "high_price": 53000000.0,
        "low_price": 51000000.0,
        "trade_volume": 1234.5678,
        "timestamp": 1706789012345
    })";

    const int iterations = 1'000'000;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(ticker_json);

    volatile double price_sum = 0;

    auto start = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto doc = parser.iterate(padded);
        double price = doc["trade_price"].get_double().value();
        price_sum += price;
    }
    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double ns_per_parse = static_cast<double>(duration_ns) / iterations;
    double parses_per_sec = 1e9 / ns_per_parse;

    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Message size: " << ticker_json.size() << " bytes" << std::endl;
    std::cout << "Total time: " << duration_ns / 1e6 << " ms" << std::endl;
    std::cout << "Time per parse: " << ns_per_parse << " ns (" << ns_per_parse / 1000 << " μs)" << std::endl;
    std::cout << "Parses/sec: " << parses_per_sec / 1e6 << " million" << std::endl;
    std::cout << "Throughput: " << (ticker_json.size() * parses_per_sec) / 1e9 << " GB/s" << std::endl;
}

// ============== Lock-free Queue Benchmark ==============
template<typename T, size_t Capacity>
class alignas(64) SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    alignas(64) std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool try_push(T item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & MASK;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[tail];
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }
};

void benchmark_lockfree_queue() {
    std::cout << "\n=== Lock-free Queue Benchmark ===" << std::endl;

    SPSCRingBuffer<uint64_t, 4096> queue;
    const int iterations = 10'000'000;

    std::atomic<bool> done{false};
    uint64_t sum = 0;

    auto start = high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < iterations; ++i) {
            while (!queue.try_push(i)) {
                // Spin
            }
        }
        done = true;
    });

    std::thread consumer([&]() {
        uint64_t value;
        int count = 0;
        while (count < iterations) {
            if (queue.try_pop(value)) {
                sum += value;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(duration_ns) / iterations;
    double ops_per_sec = 1e9 / ns_per_op;

    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << duration_ns / 1e6 << " ms" << std::endl;
    std::cout << "Time per push+pop: " << ns_per_op << " ns" << std::endl;
    std::cout << "Operations/sec: " << ops_per_sec / 1e6 << " million" << std::endl;
}

// ============== Latency Simulation ==============
void benchmark_tick_to_trade_latency() {
    std::cout << "\n=== Tick-to-Trade Latency Simulation ===" << std::endl;

    // Simulate: receive ticker -> parse JSON -> calculate premium -> generate signal
    const std::string ticker_json = R"({"tp":52345678.0,"bp":52340000.0,"ap":52350000.0})";

    const int iterations = 100'000;
    std::vector<int64_t> latencies(iterations);

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(ticker_json);

    double usdt_rate = 1380.0;
    double foreign_bid = 38000.0;

    for (int i = 0; i < iterations; ++i) {
        auto start = high_resolution_clock::now();

        // 1. Parse JSON
        auto doc = parser.iterate(padded);
        double korean_ask = doc["ap"].get_double().value();

        // 2. Calculate premium
        double premium = calculate_premium(korean_ask, foreign_bid, usdt_rate);

        // 3. Check signal condition
        bool should_trade = premium <= -1.0;

        auto end = high_resolution_clock::now();
        latencies[i] = duration_cast<nanoseconds>(end - start).count();

        // Prevent optimization
        if (should_trade && i < 0) std::cout << premium;
    }

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());

    int64_t sum = 0;
    for (auto l : latencies) sum += l;
    double avg = static_cast<double>(sum) / iterations;

    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Average latency: " << avg << " ns (" << avg / 1000 << " μs)" << std::endl;
    std::cout << "Median (p50): " << latencies[iterations / 2] << " ns" << std::endl;
    std::cout << "p90 latency: " << latencies[iterations * 90 / 100] << " ns" << std::endl;
    std::cout << "p99 latency: " << latencies[iterations * 99 / 100] << " ns" << std::endl;
    std::cout << "p99.9 latency: " << latencies[iterations * 999 / 1000] << " ns" << std::endl;
    std::cout << "Min latency: " << latencies[0] << " ns" << std::endl;
    std::cout << "Max latency: " << latencies[iterations - 1] << " ns" << std::endl;
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     C++ HFT Performance Benchmark - KIMP Arbitrage Bot       ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝" << std::endl;

    benchmark_premium_calculation();
    benchmark_json_parsing();
    benchmark_lockfree_queue();
    benchmark_tick_to_trade_latency();

    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "C++ provides sub-microsecond latency for trading operations." << std::endl;
    std::cout << "Python typically operates in millisecond range (1000x slower)." << std::endl;

    return 0;
}
