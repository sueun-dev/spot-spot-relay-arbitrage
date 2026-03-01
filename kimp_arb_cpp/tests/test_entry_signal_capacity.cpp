#include "kimp/strategy/arbitrage_engine.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

kimp::Ticker make_ticker(kimp::Exchange ex,
                         const kimp::SymbolId& symbol,
                         double bid,
                         double ask,
                         double last,
                         double funding_rate = 0.0001,
                         int funding_interval_hours = 8) {
    kimp::Ticker t;
    t.exchange = ex;
    t.symbol = symbol;
    t.timestamp = std::chrono::steady_clock::now();
    t.bid = bid;
    t.ask = ask;
    t.last = last;
    t.funding_rate = funding_rate;
    t.funding_interval_hours = funding_interval_hours;
    return t;
}

}  // namespace

int main() {
    using namespace kimp;
    using namespace kimp::strategy;

    std::cout << "=== Entry Signal Capacity Regression Test ===\n";

    const int prev_max_positions = TradingConfig::MAX_POSITIONS;
    TradingConfig::MAX_POSITIONS = 2;

    ArbitrageEngine engine;
    engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);

    const std::vector<std::string> bases = {"AAA", "BBB", "CCC", "DDD", "EEE"};
    for (const auto& base : bases) {
        engine.add_symbol(SymbolId(base, "KRW"));
    }

    // Seed USDT/KRW.
    engine.on_ticker_update(make_ticker(
        Exchange::Bithumb, SymbolId("USDT", "KRW"), 1000.0, 1000.2, 1000.1));

    // Make all symbols qualify for entry (about -2% premium).
    for (const auto& base : bases) {
        engine.on_ticker_update(make_ticker(
            Exchange::Bybit, SymbolId(base, "USDT"), 100.0, 100.1, 100.05, 0.0001, 8));
        engine.on_ticker_update(make_ticker(
            Exchange::Bithumb, SymbolId(base, "KRW"), 97900.0, 98000.0, 97950.0));
    }

    // Trigger one more full pass after all quotes are populated.
    engine.on_ticker_update(make_ticker(
        Exchange::Bithumb, SymbolId("USDT", "KRW"), 1000.0, 1000.2, 1000.1));

    int total_signals = 0;
    std::set<std::string> unique_symbols;
    while (auto sig = engine.get_entry_signal()) {
        ++total_signals;
        unique_symbols.insert(sig->symbol.to_string());
    }

    const bool within_capacity =
        static_cast<int>(unique_symbols.size()) <= TradingConfig::MAX_POSITIONS;

    std::cout << "  MAX_POSITIONS=" << TradingConfig::MAX_POSITIONS << "\n";
    std::cout << "  total_signals=" << total_signals << "\n";
    std::cout << "  unique_signal_symbols=" << unique_symbols.size() << "\n";

    TradingConfig::MAX_POSITIONS = prev_max_positions;

    if (within_capacity) {
        std::cout << "*** PASS: signal emission respects position capacity ***\n";
        return 0;
    }

    std::cout << "*** FAIL: emitted signals exceed available position slots ***\n";
    return 1;
}

