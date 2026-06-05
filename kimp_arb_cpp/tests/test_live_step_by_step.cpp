#include "test_live_trade_common.hpp"

#include "kimp/strategy/arbitrage_engine.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string>

namespace {

using kimp::Exchange;
using kimp::SymbolId;

struct PairCheck {
    live_trade::PairSpec pair;
    bool route_available{false};
    std::string network{"-"};
    double withdraw_fee{0.0};
    live_trade::TopQuote korean_quote{};
    live_trade::TopQuote foreign_quote{};
    double usdt_rate{0.0};
    kimp::strategy::PremiumCalculator::RelayMetrics relay{};
};

std::string pick_symbol(const std::vector<std::string>& common, const std::optional<std::string>& requested) {
    if (requested) {
        if (std::find(common.begin(), common.end(), *requested) == common.end()) {
            throw std::runtime_error("requested symbol is not common across all 4 venues: " + *requested);
        }
        return *requested;
    }
    for (const auto& preferred : {"ADA", "SOL", "DOGE", "XRP", "TRX"}) {
        auto it = std::find(common.begin(), common.end(), preferred);
        if (it != common.end()) {
            return *it;
        }
    }
    if (common.empty()) {
        throw std::runtime_error("no 4-way common symbol");
    }
    return common.front();
}

std::optional<std::string> parse_symbol_arg(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--symbol") {
            return std::string(argv[i + 1]);
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        using kimp::strategy::ArbitrageEngine;
        using kimp::strategy::PriceCache;
        using kimp::strategy::PremiumCalculator;

        live_smoke::init_test_logger("kimp_test_live_step_by_step");
        const auto requested_symbol = parse_symbol_arg(argc, argv);
        const auto config = live_smoke::load_runtime_config_or_throw();

        live_smoke::require_exchange_creds(config, Exchange::Bithumb);
        live_smoke::require_exchange_creds(config, Exchange::Upbit);
        live_smoke::require_exchange_creds(config, Exchange::Bybit);
        live_smoke::require_exchange_creds(config, Exchange::OKX, true, true);

        boost::asio::io_context io_context;
        auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
            io_context, config.exchanges.at(Exchange::Bithumb));
        auto upbit = std::make_shared<kimp::exchange::upbit::UpbitExchange>(
            io_context, config.exchanges.at(Exchange::Upbit));
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(Exchange::Bybit));
        auto okx = std::make_shared<kimp::exchange::okx::OkxExchange>(
            io_context, config.exchanges.at(Exchange::OKX));

        if (!bithumb->initialize_rest() || !upbit->initialize_rest() ||
            !bybit->initialize_rest() || !okx->initialize_rest()) {
            throw std::runtime_error("authenticated REST init failed");
        }

        const auto bithumb_balances = bithumb->get_all_balances();
        const auto upbit_balances = upbit->get_all_balances();
        const auto bybit_balances = bybit->get_all_balances();
        const auto okx_balances = okx->get_all_balances();

        const auto bithumb_symbols = bithumb->get_available_symbols();
        const auto upbit_symbols = upbit->get_available_symbols();
        const auto bybit_symbols = bybit->get_available_symbols();
        const auto okx_symbols = okx->get_available_symbols();

        const auto kr_union = live_smoke::set_union(
            live_smoke::base_set(bithumb_symbols), live_smoke::base_set(upbit_symbols));
        const auto foreign_union = live_smoke::set_union(
            live_smoke::base_set(bybit_symbols), live_smoke::base_set(okx_symbols));
        auto common = live_smoke::set_intersection(kr_union, foreign_union);
        const auto symbol = pick_symbol(common, requested_symbol);
        const SymbolId korean_symbol(symbol, "KRW");
        const SymbolId foreign_symbol(symbol, "USDT");

        const auto bithumb_fees = bithumb->fetch_withdrawal_fees();
        const auto bithumb_statuses = bithumb->fetch_asset_statuses();
        const auto upbit_fees = upbit->fetch_withdrawal_fees({symbol});
        const auto bybit_nets = bybit->fetch_deposit_networks();
        const auto okx_nets = okx->fetch_deposit_networks();

        const auto bi_quote = live_trade::fetch_bithumb_quote(io_context, config, korean_symbol);
        const auto up_quote = live_trade::fetch_upbit_quote(io_context, config, korean_symbol);
        const auto by_quote = live_trade::fetch_bybit_quote(io_context, config, foreign_symbol);
        const auto ok_quote = live_trade::fetch_okx_quote(io_context, config, foreign_symbol);
        const auto bi_usdt = live_trade::fetch_bithumb_quote(io_context, config, SymbolId("USDT", "KRW"));
        const auto up_usdt = live_trade::fetch_upbit_quote(io_context, config, SymbolId("USDT", "KRW"));

        PriceCache cache;
        auto load_korean = [&](Exchange ex, const auto& fee_map, bool withdraw_enabled) {
            auto fee_it = fee_map.find(symbol);
            if (fee_it != fee_map.end()) {
                std::vector<PriceCache::NetworkFee> converted;
                converted.reserve(fee_it->second.size());
                for (const auto& fee : fee_it->second) {
                    converted.push_back({fee.network, fee.fee_coins});
                }
                cache.set_withdraw_network_fees(ex, symbol, std::move(converted));
            }
            cache.set_korean_withdraw_enabled(ex, symbol, withdraw_enabled);
        };
        auto load_foreign = [&](Exchange ex, const auto& net_map) {
            auto it = net_map.find(symbol);
            if (it != net_map.end()) {
                cache.set_foreign_deposit_networks(ex, symbol, it->second);
            }
        };

        const bool bithumb_withdraw_ok =
            bithumb_statuses.contains(symbol) && bithumb_statuses.at(symbol).withdraw_enabled;
        load_korean(Exchange::Bithumb, bithumb_fees, bithumb_withdraw_ok);
        load_korean(Exchange::Upbit, upbit_fees, true);
        load_foreign(Exchange::Bybit, bybit_nets);
        load_foreign(Exchange::OKX, okx_nets);
        cache.finalize_withdraw_fees();

        std::array<PairCheck, 4> checks{{
            {{Exchange::Bithumb, Exchange::Bybit, "Bi-By"}},
            {{Exchange::Bithumb, Exchange::OKX,   "Bi-Ok"}},
            {{Exchange::Upbit,   Exchange::Bybit, "Up-By"}},
            {{Exchange::Upbit,   Exchange::OKX,   "Up-Ok"}},
        }};

        for (auto& check : checks) {
            check.route_available = cache.is_transfer_route_available(check.pair.korean, check.pair.foreign, symbol);
            if (const auto route = cache.get_transfer_route(check.pair.korean, check.pair.foreign, symbol)) {
                check.network = route->network.empty() ? "-" : route->network;
                check.withdraw_fee = route->fee_coins;
            }
            check.korean_quote = (check.pair.korean == Exchange::Bithumb) ? bi_quote : up_quote;
            check.foreign_quote = (check.pair.foreign == Exchange::Bybit) ? by_quote : ok_quote;
            check.usdt_rate = (check.pair.korean == Exchange::Bithumb) ? bi_usdt.ask : up_usdt.ask;
            check.relay = PremiumCalculator::calculate_relay_metrics(
                check.korean_quote.ask, check.korean_quote.ask_qty,
                check.foreign_quote.bid, check.foreign_quote.bid_qty,
                check.usdt_rate,
                kimp::TradingConfig::get_korean_fee_rate(check.pair.korean),
                kimp::TradingConfig::get_foreign_fee_rate(check.pair.foreign),
                cache.get_withdraw_fee(check.pair.korean, check.pair.foreign, symbol));
        }

        ArbitrageEngine engine;
        std::optional<kimp::ArbitrageSignal> captured;
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::Bybit);
        engine.add_exchange_pair(Exchange::Bithumb, Exchange::OKX);
        engine.add_exchange_pair(Exchange::Upbit, Exchange::Bybit);
        engine.add_exchange_pair(Exchange::Upbit, Exchange::OKX);
        engine.add_symbol(korean_symbol);
        engine.set_entry_callback([&](const kimp::ArbitrageSignal& signal) { captured = signal; });

        auto& pc = engine.get_price_cache();
        for (const auto& check : checks) {
            const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            pc.update(check.pair.korean, korean_symbol, check.korean_quote.bid, check.korean_quote.ask,
                      (check.korean_quote.bid + check.korean_quote.ask) * 0.5, now,
                      check.korean_quote.bid_qty, check.korean_quote.ask_qty);
            pc.update(check.pair.foreign, foreign_symbol, check.foreign_quote.bid, check.foreign_quote.ask,
                      (check.foreign_quote.bid + check.foreign_quote.ask) * 0.5, now,
                      check.foreign_quote.bid_qty, check.foreign_quote.ask_qty);
            if (const auto route = cache.get_transfer_route(check.pair.korean, check.pair.foreign, symbol)) {
                pc.set_korean_withdraw_enabled(check.pair.korean, symbol, route->withdraw_open);
                if (route->fee_coins >= 0.0) {
                    pc.set_withdraw_network_fees(
                        check.pair.korean, symbol, {PriceCache::NetworkFee{route->network, route->fee_coins}});
                }
            }
        }
        load_foreign(Exchange::Bybit, bybit_nets);
        load_foreign(Exchange::OKX, okx_nets);
        pc.finalize_withdraw_fees();

        auto push_usdt = [&](Exchange ex, double rate) {
            kimp::Ticker ticker;
            ticker.exchange = ex;
            ticker.symbol = SymbolId("USDT", "KRW");
            ticker.timestamp = std::chrono::steady_clock::now();
            ticker.bid = rate;
            ticker.ask = rate;
            ticker.last = rate;
            ticker.bid_qty = 1'000'000.0;
            ticker.ask_qty = 1'000'000.0;
            engine.on_ticker_update(ticker);
        };
        push_usdt(Exchange::Bithumb, bi_usdt.ask);
        push_usdt(Exchange::Upbit, up_usdt.ask);

        const auto premiums = engine.get_all_premiums();

        std::cout << "=== Live Step-by-Step Example ===\n";
        std::cout << "Step 1. Auth/REST init: PASS\n";
        std::cout << "Step 2. Balance fetch: PASS | "
                  << "Bi KRW=" << live_smoke::available_of(bithumb_balances, "KRW")
                  << " | Up KRW=" << live_smoke::available_of(upbit_balances, "KRW")
                  << " | By USDT=" << live_smoke::available_of(bybit_balances, "USDT")
                  << " | Ok USDT=" << live_smoke::available_of(okx_balances, "USDT") << "\n";
        std::cout << "Step 3. Universe: Bi=" << bithumb_symbols.size()
                  << " Up=" << upbit_symbols.size()
                  << " By=" << bybit_symbols.size()
                  << " Ok=" << okx_symbols.size()
                  << " | common=" << common.size()
                  << " | sample=" << symbol << "\n";
        std::cout << "Step 4. Transfer routes + live entry metrics:\n";
        for (const auto& check : checks) {
            std::cout << "  - " << check.pair.label
                      << " | route=" << (check.route_available ? "PASS" : "BLOCK")
                      << " | net=" << check.network
                      << " | wdFee=" << check.withdraw_fee
                      << " | Kask=" << check.korean_quote.ask
                      << " | Fbid=" << check.foreign_quote.bid
                      << " | targetQty=" << check.relay.target_coin_qty
                      << " | matchQty=" << check.relay.match_qty
                      << " | bothFill=" << (check.relay.both_can_fill_target ? "YES" : "NO")
                      << " | netEdge=" << check.relay.net_edge_pct
                      << "% | netKRW=" << check.relay.net_profit_krw << "\n";
        }

        if (!premiums.empty()) {
            const auto& best = premiums.front();
            std::cout << "Step 5. Engine best pair: "
                      << kimp::exchange_name(best.best_korean_exchange) << "-"
                      << kimp::exchange_name(best.best_foreign_exchange)
                      << " | quoteUsable=" << (best.quote_usable ? "YES" : "NO")
                      << " | entrySignal=" << (best.entry_signal ? "YES" : "NO")
                      << " | netKRW=" << best.net_profit_krw
                      << " | age=" << best.age_ms << "ms\n";
        }

        std::cout << "Step 6. Callback emission: "
                  << (captured.has_value() ? "SIGNAL" : "NO SIGNAL") << "\n";
        if (captured) {
            std::cout << "  -> " << captured->symbol.to_string()
                      << " | " << kimp::exchange_name(captured->korean_exchange)
                      << "-" << kimp::exchange_name(captured->foreign_exchange)
                      << " | premium=" << captured->premium
                      << "% | netKRW=" << captured->net_profit_krw << "\n";
        }

        bithumb->shutdown_rest();
        upbit->shutdown_rest();
        bybit->shutdown_rest();
        okx->shutdown_rest();
        kimp::Logger::shutdown();
        return premiums.empty() ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
