#include "test_live_trade_common.hpp"

int main(int argc, char* argv[]) {
    try {
        using kimp::SymbolId;
        live_smoke::init_test_logger("kimp_test_live_margin_setup");
        const auto config = live_smoke::load_runtime_config_or_throw();
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bybit);
        live_smoke::require_exchange_creds(config, kimp::Exchange::OKX, true, true);

        std::vector<std::string> requested_bases;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--symbols" && i + 1 < argc) {
                std::stringstream ss(argv[++i]);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (!item.empty()) {
                        requested_bases.push_back(item);
                    }
                }
            }
        }

        boost::asio::io_context io_context;
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bybit));
        auto okx = std::make_shared<kimp::exchange::okx::OkxExchange>(
            io_context, config.exchanges.at(kimp::Exchange::OKX));

        const auto bybit_symbols = live_smoke::fetch_symbols_or_throw("Bybit", bybit);
        const auto okx_symbols = live_smoke::fetch_symbols_or_throw("OKX", okx);
        auto intersection = live_smoke::set_intersection(
            live_smoke::base_set(bybit_symbols),
            live_smoke::base_set(okx_symbols));

        std::vector<std::string> bases = std::move(requested_bases);
        if (bases.empty()) {
            const std::size_t limit = std::min<std::size_t>(intersection.size(), 5);
            for (std::size_t i = 0; i < limit; ++i) {
                bases.push_back(intersection[i]);
            }
        }
        if (bases.empty()) {
            throw std::runtime_error("no shared Bybit/OKX bases available for margin setup test");
        }

        if (!bybit->initialize_rest() || !okx->initialize_rest()) {
            throw std::runtime_error("failed to initialize authenticated REST clients");
        }

        const auto bybit_before = bybit->get_all_balances();
        const auto okx_before = okx->get_all_balances();

        bool all_pass = true;

        std::cout << "=== Live Margin Setup Test ===\n";
        for (const auto& base : bases) {
            SymbolId foreign_symbol(base, "USDT");
            const bool bybit_ready = bybit->prepare_shorting(foreign_symbol);
            const bool okx_ready = okx->prepare_shorting(foreign_symbol);
            all_pass = all_pass && bybit_ready && okx_ready;
            std::cout << base
                      << " | BybitMargin=" << (bybit_ready ? "PASS" : "FAIL")
                      << " | OKXCross1x=" << (okx_ready ? "PASS" : "FAIL")
                      << "\n";
        }

        const auto bybit_after = bybit->get_all_balances();
        const auto okx_after = okx->get_all_balances();
        bybit->shutdown_rest();
        okx->shutdown_rest();

        for (const auto& base : bases) {
            const double bybit_before_liab = live_trade::liability_of(bybit_before, base);
            const double bybit_after_liab = live_trade::liability_of(bybit_after, base);
            const double okx_before_liab = live_trade::liability_of(okx_before, base);
            const double okx_after_liab = live_trade::liability_of(okx_after, base);
            std::cout << base
                      << " | BybitLiabBefore=" << bybit_before_liab
                      << " After=" << bybit_after_liab
                      << " | OKXLiabBefore=" << okx_before_liab
                      << " After=" << okx_after_liab
                      << "\n";
        }

        std::cout << (all_pass ? "[PASS]" : "[FAIL]") << " margin/leverage setup test finished\n";
        kimp::Logger::shutdown();
        return all_pass ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
