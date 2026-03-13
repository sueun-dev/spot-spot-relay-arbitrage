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
                         double bid_qty = 100.0,
                         double ask_qty = 100.0) {
    kimp::Ticker t;
    t.exchange = ex;
    t.symbol = symbol;
    t.timestamp = std::chrono::steady_clock::now();
    t.bid = bid;
    t.ask = ask;
    t.last = last;
    t.bid_qty = bid_qty;
    t.ask_qty = ask_qty;
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
        Exchange::Bithumb, SymbolId("USDT", "KRW"), 1000.0, 1000.2, 1000.1, 1000000.0, 1000000.0));

    // Make all symbols qualify for relay entry with enough top-of-book quantity.
    for (const auto& base : bases) {
        engine.on_ticker_update(make_ticker(
            Exchange::Bybit, SymbolId(base, "USDT"), 100.0, 100.1, 100.05, 10.0, 10.0));
        engine.on_ticker_update(make_ticker(
            Exchange::Bithumb, SymbolId(base, "KRW"), 97950.0, 98000.0, 97975.0, 10.0, 10.0));
    }

    // Trigger one more full pass after all quotes are populated.
    engine.on_ticker_update(make_ticker(
        Exchange::Bithumb, SymbolId("USDT", "KRW"), 1000.0, 1000.2, 1000.1, 1000000.0, 1000000.0));

    int total_signals = 0;
    std::set<std::string> unique_symbols;
    while (auto sig = engine.get_entry_signal()) {
        ++total_signals;
        unique_symbols.insert(sig->symbol.to_string());
    }

    const bool within_capacity =
        !unique_symbols.empty() &&
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
