#include "kimp/strategy/arbitrage_engine.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
    using namespace kimp;
    using namespace kimp::strategy;

    std::cout << "=== Export Async Stability Smoke Test ===\n";

    constexpr int kIterations = 500;
    constexpr int kSymbolsPerIter = 400;

    for (int it = 0; it < kIterations; ++it) {
        ArbitrageEngine engine;
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);

        for (int i = 0; i < kSymbolsPerIter; ++i) {
            engine.add_symbol(SymbolId("T" + std::to_string(i), "KRW"));
        }

        engine.export_to_json_async("/tmp/kimp_export_async_smoke.json");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "*** PASS: no crash during async export stress run ***\n";
    return 0;
}
