#include "test_live_trade_common.hpp"

namespace {

struct Options {
    live_trade::PairSpec pair{};
    std::string symbol;
    double target_usdt{35.0};
    bool execute_entry_only{false};
    bool execute_roundtrip{false};
    bool confirmed{false};
};

void print_help(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " --pair Bi-By|Bi-Ok|Up-By|Up-Ok --symbol BASE [options]\n"
        << "Options:\n"
        << "  --usdt <n>               Target top-of-book size in USDT (default: 35)\n"
        << "  --execute-entry-only     Actually place entry legs only\n"
        << "  --execute-roundtrip      Actually place entry then immediate exit\n"
        << "  --confirm LIVE           Required with execute flags\n";
}

Options parse_args(int argc, char* argv[]) {
    Options options;
    bool pair_set = false;
    bool symbol_set = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pair" && i + 1 < argc) {
            options.pair = live_trade::parse_pair(argv[++i]);
            pair_set = true;
        } else if (arg == "--symbol" && i + 1 < argc) {
            options.symbol = argv[++i];
            symbol_set = true;
        } else if (arg == "--usdt" && i + 1 < argc) {
            options.target_usdt = std::stod(argv[++i]);
        } else if (arg == "--execute-entry-only") {
            options.execute_entry_only = true;
        } else if (arg == "--execute-roundtrip") {
            options.execute_roundtrip = true;
        } else if (arg == "--confirm" && i + 1 < argc) {
            options.confirmed = std::string(argv[++i]) == "LIVE";
        } else if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown arg: " + arg);
        }
    }

    if (!pair_set || !symbol_set) {
        throw std::runtime_error("--pair and --symbol are required");
    }
    if ((options.execute_entry_only || options.execute_roundtrip) && !options.confirmed) {
        throw std::runtime_error("--confirm LIVE is required for execute modes");
    }
    if (options.execute_entry_only && options.execute_roundtrip) {
        throw std::runtime_error("choose only one execute mode");
    }
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        using kimp::Order;
        using kimp::OrderStatus;
        using kimp::SymbolId;
        live_smoke::init_test_logger("kimp_test_live_pair_roundtrip");
        const auto options = parse_args(argc, argv);
        const auto config = live_smoke::load_runtime_config_or_throw();
        live_smoke::require_exchange_creds(config, options.pair.korean);
        if (options.pair.foreign == kimp::Exchange::OKX) {
            live_smoke::require_exchange_creds(config, options.pair.foreign, true, true);
        } else {
            live_smoke::require_exchange_creds(config, options.pair.foreign);
        }

        boost::asio::io_context io_context;
        const SymbolId korean_symbol(options.symbol, "KRW");
        const SymbolId foreign_symbol(options.symbol, "USDT");

        auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bithumb));
        auto upbit = std::make_shared<kimp::exchange::upbit::UpbitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Upbit));
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bybit));
        auto okx = std::make_shared<kimp::exchange::okx::OkxExchange>(
            io_context, config.exchanges.at(kimp::Exchange::OKX));

        std::shared_ptr<kimp::exchange::KoreanExchangeBase> korean_ex =
            options.pair.korean == kimp::Exchange::Bithumb
                ? std::static_pointer_cast<kimp::exchange::KoreanExchangeBase>(bithumb)
                : std::static_pointer_cast<kimp::exchange::KoreanExchangeBase>(upbit);
        std::shared_ptr<kimp::exchange::ForeignShortExchangeBase> foreign_ex =
            options.pair.foreign == kimp::Exchange::OKX
                ? std::static_pointer_cast<kimp::exchange::ForeignShortExchangeBase>(okx)
                : std::static_pointer_cast<kimp::exchange::ForeignShortExchangeBase>(bybit);

        if (!korean_ex->initialize_rest() || !foreign_ex->initialize_rest()) {
            throw std::runtime_error("authenticated REST init failed");
        }

        // Warm symbol metadata so venue-specific quantity normalization uses the live lot size.
        if (options.pair.foreign == kimp::Exchange::OKX) {
            okx->get_available_symbols();
        } else {
            bybit->get_available_symbols();
        }

        const auto korean_quote = live_trade::fetch_korean_quote(io_context, config, options.pair.korean, korean_symbol);
        const auto foreign_quote = live_trade::fetch_foreign_quote(io_context, config, options.pair.foreign, foreign_symbol);
        const auto usdt_quote = live_trade::fetch_korean_quote(io_context, config, options.pair.korean, SymbolId("USDT", "KRW"));

        const double korean_top_usdt = (korean_quote.ask * korean_quote.ask_qty) / usdt_quote.ask;
        const double foreign_top_usdt = foreign_quote.bid * foreign_quote.bid_qty;
        const double target_qty = std::min({options.target_usdt / foreign_quote.bid,
                                            korean_quote.ask_qty,
                                            foreign_quote.bid_qty});
        const double estimated_krw_cost = korean_quote.ask * target_qty;

        const auto before_korean = korean_ex->get_all_balances();
        const auto before_foreign = foreign_ex->get_all_balances();

        std::cout << "=== Live Pair Roundtrip Test ===\n";
        std::cout << "Pair: " << options.pair.label << " | Symbol: " << options.symbol << "\n";
        std::cout << "Korean ask=" << korean_quote.ask << " qty=" << korean_quote.ask_qty
                  << " | Foreign bid=" << foreign_quote.bid << " qty=" << foreign_quote.bid_qty << "\n";
        std::cout << "USDT/KRW ask=" << usdt_quote.ask << "\n";
        std::cout << "Korean top USDT=" << korean_top_usdt
                  << " | Foreign top USDT=" << foreign_top_usdt << "\n";
        std::cout << "Target qty=" << target_qty << " | Est KRW cost=" << estimated_krw_cost << "\n";
        std::cout << "Before Korean " << options.symbol << "="
                  << live_trade::total_balance_of(before_korean, options.symbol)
                  << " | Foreign liability=" << live_trade::liability_of(before_foreign, options.symbol) << "\n";

        if (!options.execute_entry_only && !options.execute_roundtrip) {
            std::cout << "[DRY-RUN] no orders placed\n";
            korean_ex->shutdown_rest();
            foreign_ex->shutdown_rest();
            kimp::Logger::shutdown();
            return 0;
        }

        if (target_qty <= 0.0 || estimated_krw_cost < 5000.0) {
            throw std::runtime_error("top-of-book size too small for live roundtrip");
        }

        if (!foreign_ex->prepare_shorting(foreign_symbol)) {
            throw std::runtime_error("foreign prepare_shorting failed");
        }

        Order foreign_entry = foreign_ex->open_short(foreign_symbol, target_qty);
        if (foreign_entry.status == OrderStatus::Rejected && foreign_entry.order_id_str.empty()) {
            throw std::runtime_error("foreign entry rejected before order id issuance");
        }
        live_trade::wait_foreign_fill(options.pair.foreign, foreign_symbol, foreign_ex, foreign_entry);
        const double foreign_entry_qty = live_trade::resolved_fill_quantity(foreign_entry);
        const double foreign_entry_px = live_trade::resolved_fill_price(foreign_entry, foreign_quote.bid);
        live_trade::print_order_summary("foreign-entry", foreign_entry, foreign_quote.bid);
        if (foreign_entry.status != OrderStatus::Filled || foreign_entry_qty <= 0.0) {
            throw std::runtime_error("foreign entry failed");
        }

        Order korean_entry;
        if (options.pair.korean == kimp::Exchange::Bithumb) {
            korean_entry = std::dynamic_pointer_cast<kimp::exchange::bithumb::BithumbExchange>(korean_ex)
                               ->place_market_buy_quantity(korean_symbol, foreign_entry_qty);
        } else {
            korean_entry = korean_ex->place_market_buy_cost(korean_symbol, foreign_entry_qty * korean_quote.ask);
        }
        live_trade::wait_korean_fill(options.pair.korean, korean_symbol, korean_ex, korean_entry);
        const double korean_entry_qty = live_trade::resolved_fill_quantity(korean_entry);
        const double korean_entry_px = live_trade::resolved_fill_price(korean_entry, korean_quote.ask);
        live_trade::print_order_summary("korean-entry", korean_entry, korean_quote.ask);
        if (korean_entry.status != OrderStatus::Filled || korean_entry_qty <= 0.0) {
            throw std::runtime_error("korean entry failed");
        }

        const double entry_delta = korean_entry_qty - foreign_entry_qty;
        std::cout << "ENTRY AUDIT | kr_qty=" << korean_entry_qty
                  << " | fr_qty=" << foreign_entry_qty
                  << " | delta=" << entry_delta
                  << " | " << (live_trade::quantities_match(korean_entry_qty, foreign_entry_qty) ? "MATCH" : "MISMATCH")
                  << "\n";

        if (options.execute_entry_only) {
            const auto after_entry_korean = korean_ex->get_all_balances();
            const auto after_entry_foreign = foreign_ex->get_all_balances();
            std::cout << "After entry Korean " << options.symbol << "="
                      << live_trade::total_balance_of(after_entry_korean, options.symbol)
                      << " | Foreign liability="
                      << live_trade::liability_of(after_entry_foreign, options.symbol) << "\n";
            korean_ex->shutdown_rest();
            foreign_ex->shutdown_rest();
            kimp::Logger::shutdown();
            return live_trade::quantities_match(korean_entry_qty, foreign_entry_qty) ? 0 : 2;
        }

        Order foreign_exit = foreign_ex->close_short(foreign_symbol, foreign_entry_qty);
        if (foreign_exit.status == OrderStatus::Rejected && foreign_exit.order_id_str.empty()) {
            throw std::runtime_error("foreign exit rejected before order id issuance");
        }
        live_trade::wait_foreign_fill(options.pair.foreign, foreign_symbol, foreign_ex, foreign_exit);
        const double foreign_exit_qty = live_trade::resolved_fill_quantity(foreign_exit);
        const double foreign_exit_px = live_trade::resolved_fill_price(foreign_exit, foreign_quote.ask);
        live_trade::print_order_summary("foreign-exit", foreign_exit, foreign_quote.ask);
        if (foreign_exit.status != OrderStatus::Filled || foreign_exit_qty <= 0.0) {
            throw std::runtime_error("foreign exit failed");
        }

        Order korean_exit = korean_ex->place_market_order(korean_symbol, kimp::Side::Sell, korean_entry_qty);
        live_trade::wait_korean_fill(options.pair.korean, korean_symbol, korean_ex, korean_exit);
        const double korean_exit_qty = live_trade::resolved_fill_quantity(korean_exit);
        const double korean_exit_px = live_trade::resolved_fill_price(korean_exit, korean_quote.bid);
        live_trade::print_order_summary("korean-exit", korean_exit, korean_quote.bid);
        if (korean_exit.status != OrderStatus::Filled || korean_exit_qty <= 0.0) {
            throw std::runtime_error("korean exit failed");
        }

        const double exit_delta = korean_exit_qty - foreign_exit_qty;
        const auto after_korean = korean_ex->get_all_balances();
        const auto after_foreign = foreign_ex->get_all_balances();
        const double after_korean_base = live_trade::total_balance_of(after_korean, options.symbol);
        const double after_foreign_liability = live_trade::liability_of(after_foreign, options.symbol);
        const double before_korean_base = live_trade::total_balance_of(before_korean, options.symbol);
        const double before_foreign_liability = live_trade::liability_of(before_foreign, options.symbol);
        const double residual_korean = after_korean_base - before_korean_base;
        const double residual_liability = after_foreign_liability - before_foreign_liability;

        const double korean_pnl_krw = (korean_exit_px - korean_entry_px) * korean_exit_qty;
        const double foreign_pnl_krw = (foreign_entry_px - foreign_exit_px) * foreign_exit_qty * usdt_quote.ask;

        std::cout << "EXIT AUDIT | kr_qty=" << korean_exit_qty
                  << " | fr_qty=" << foreign_exit_qty
                  << " | delta=" << exit_delta
                  << " | " << (live_trade::quantities_match(korean_exit_qty, foreign_exit_qty) ? "MATCH" : "MISMATCH")
                  << "\n";
        std::cout << "RESIDUAL AUDIT | korean_delta=" << residual_korean
                  << " | foreign_liability_delta=" << residual_liability << "\n";
        std::cout << "PNL ROUGH | korean=" << korean_pnl_krw
                  << " KRW | foreign=" << foreign_pnl_krw
                  << " KRW | total=" << (korean_pnl_krw + foreign_pnl_krw) << " KRW\n";

        korean_ex->shutdown_rest();
        foreign_ex->shutdown_rest();
        kimp::Logger::shutdown();

        if (!live_trade::quantities_match(korean_entry_qty, foreign_entry_qty) ||
            !live_trade::quantities_match(korean_exit_qty, foreign_exit_qty)) {
            return 2;
        }
        return (std::fabs(residual_korean) <= 1e-6 && std::fabs(residual_liability) <= 1e-6) ? 0 : 3;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
