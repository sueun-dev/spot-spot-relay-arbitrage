#!/usr/bin/env python3
"""Python benchmark to compare with C++ performance"""

import json
import time
import random
import queue
import threading
from typing import List
import statistics

def calculate_premium(korean_ask: float, foreign_bid: float, usdt_rate: float) -> float:
    """Calculate kimchi premium percentage"""
    foreign_krw = foreign_bid * usdt_rate
    return ((korean_ask - foreign_krw) / foreign_krw) * 100.0

def benchmark_premium_calculation():
    print("\n=== Premium Calculation Benchmark ===")

    iterations = 1_000_000  # 10x less than C++ due to speed

    # Generate test data
    korean_prices = [random.uniform(50000000, 55000000) for _ in range(iterations)]
    foreign_prices = [random.uniform(35000, 40000) for _ in range(iterations)]
    usdt_rates = [random.uniform(1350, 1400) for _ in range(iterations)]

    # Benchmark
    start = time.perf_counter_ns()
    for i in range(iterations):
        result = calculate_premium(korean_prices[i], foreign_prices[i], usdt_rates[i])
    end = time.perf_counter_ns()

    duration_ns = end - start
    ns_per_calc = duration_ns / iterations
    calcs_per_sec = 1e9 / ns_per_calc

    print(f"Iterations: {iterations}")
    print(f"Total time: {duration_ns / 1e6:.3f} ms")
    print(f"Time per calculation: {ns_per_calc:.1f} ns")
    print(f"Calculations/sec: {calcs_per_sec / 1e6:.3f} million")

def benchmark_json_parsing():
    print("\n=== JSON Parsing Benchmark (Python json) ===")

    ticker_json = '''{
        "type": "ticker",
        "code": "KRW-BTC",
        "trade_price": 52345678.0,
        "bid_price": 52340000.0,
        "ask_price": 52350000.0,
        "high_price": 53000000.0,
        "low_price": 51000000.0,
        "trade_volume": 1234.5678,
        "timestamp": 1706789012345
    }'''

    iterations = 100_000  # 10x less than C++

    price_sum = 0

    start = time.perf_counter_ns()
    for i in range(iterations):
        data = json.loads(ticker_json)
        price = data["trade_price"]
        price_sum += price
    end = time.perf_counter_ns()

    duration_ns = end - start
    ns_per_parse = duration_ns / iterations
    parses_per_sec = 1e9 / ns_per_parse

    print(f"Iterations: {iterations}")
    print(f"Message size: {len(ticker_json)} bytes")
    print(f"Total time: {duration_ns / 1e6:.3f} ms")
    print(f"Time per parse: {ns_per_parse:.1f} ns ({ns_per_parse / 1000:.1f} μs)")
    print(f"Parses/sec: {parses_per_sec / 1e6:.3f} million")
    print(f"Throughput: {(len(ticker_json) * parses_per_sec) / 1e9:.3f} GB/s")

def benchmark_queue():
    print("\n=== Queue Benchmark (Python queue.Queue) ===")

    q = queue.Queue(maxsize=4096)
    iterations = 1_000_000  # 10x less than C++

    done = threading.Event()
    total_sum = [0]

    def producer():
        for i in range(iterations):
            q.put(i)
        done.set()

    def consumer():
        count = 0
        while count < iterations:
            try:
                value = q.get(timeout=0.001)
                total_sum[0] += value
                count += 1
            except queue.Empty:
                pass

    start = time.perf_counter_ns()

    producer_thread = threading.Thread(target=producer)
    consumer_thread = threading.Thread(target=consumer)

    producer_thread.start()
    consumer_thread.start()

    producer_thread.join()
    consumer_thread.join()

    end = time.perf_counter_ns()

    duration_ns = end - start
    ns_per_op = duration_ns / iterations
    ops_per_sec = 1e9 / ns_per_op

    print(f"Iterations: {iterations}")
    print(f"Total time: {duration_ns / 1e6:.3f} ms")
    print(f"Time per push+pop: {ns_per_op:.1f} ns")
    print(f"Operations/sec: {ops_per_sec / 1e6:.3f} million")

def benchmark_tick_to_trade_latency():
    print("\n=== Tick-to-Trade Latency Simulation ===")

    ticker_json = '{"tp":52345678.0,"bp":52340000.0,"ap":52350000.0}'

    iterations = 10_000  # Much less due to speed
    latencies: List[int] = []

    usdt_rate = 1380.0
    foreign_bid = 38000.0

    for i in range(iterations):
        start = time.perf_counter_ns()

        # 1. Parse JSON
        data = json.loads(ticker_json)
        korean_ask = data["ap"]

        # 2. Calculate premium
        premium = calculate_premium(korean_ask, foreign_bid, usdt_rate)

        # 3. Check signal condition
        should_trade = premium <= -1.0

        end = time.perf_counter_ns()
        latencies.append(end - start)

    # Calculate statistics
    latencies.sort()
    avg = statistics.mean(latencies)

    print(f"Iterations: {iterations}")
    print(f"Average latency: {avg:.0f} ns ({avg / 1000:.1f} μs)")
    print(f"Median (p50): {latencies[len(latencies) // 2]} ns")
    print(f"p90 latency: {latencies[int(len(latencies) * 0.90)]} ns")
    print(f"p99 latency: {latencies[int(len(latencies) * 0.99)]} ns")
    print(f"Min latency: {latencies[0]} ns")
    print(f"Max latency: {latencies[-1]} ns")

def main():
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║     Python Performance Benchmark - KIMP Arbitrage Bot        ║")
    print("╚══════════════════════════════════════════════════════════════╝")

    benchmark_premium_calculation()
    benchmark_json_parsing()
    benchmark_queue()
    benchmark_tick_to_trade_latency()

    print("\n=== Summary ===")
    print("Python typically operates in microsecond to millisecond range.")
    print("Compare with C++ results to see the performance difference.")

if __name__ == "__main__":
    main()
