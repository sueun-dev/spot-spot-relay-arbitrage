#include "test_live_smoke_common.hpp"

int main() {
    try {
        live_smoke::init_test_logger("kimp_test_live_entry");
        const auto config = live_smoke::load_runtime_config_or_throw();
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bithumb);
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bybit);
        live_smoke::require_exchange_creds(config, kimp::Exchange::OKX, true, true);
        live_smoke::require_exchange_creds(config, kimp::Exchange::Upbit);

        boost::asio::io_context io_context;
        auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bithumb));
        auto upbit = std::make_shared<kimp::exchange::upbit::UpbitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Upbit));
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bybit));
        auto okx = std::make_shared<kimp::exchange::okx::OkxExchange>(
            io_context, config.exchanges.at(kimp::Exchange::OKX));

        const auto bithumb_symbols = live_smoke::fetch_symbols_or_throw("Bithumb", bithumb);
        const auto upbit_symbols = live_smoke::fetch_symbols_or_throw("Upbit", upbit);
        const auto bybit_symbols = live_smoke::fetch_symbols_or_throw("Bybit", bybit);
        const auto okx_symbols = live_smoke::fetch_symbols_or_throw("OKX", okx);

        const auto kr_union = live_smoke::set_union(
            live_smoke::base_set(bithumb_symbols),
            live_smoke::base_set(upbit_symbols));
        const auto foreign_union = live_smoke::set_union(
            live_smoke::base_set(bybit_symbols),
            live_smoke::base_set(okx_symbols));
        const auto common = live_smoke::set_intersection(kr_union, foreign_union);

        const auto bithumb_balances = live_smoke::fetch_balances_or_throw("Bithumb", bithumb);
        const auto upbit_balances = live_smoke::fetch_balances_or_throw("Upbit", upbit);
        const auto bybit_balances = live_smoke::fetch_balances_or_throw("Bybit", bybit);
        const auto okx_balances = live_smoke::fetch_balances_or_throw("OKX", okx);

        std::cout << "=== Live Entry Smoke ===\n";
        std::cout << "Bithumb KRW symbols: " << bithumb_symbols.size() << "\n";
        std::cout << "Upbit KRW symbols:   " << upbit_symbols.size() << "\n";
        std::cout << "Bybit USDT symbols:  " << bybit_symbols.size() << "\n";
        std::cout << "OKX USDT symbols:    " << okx_symbols.size() << "\n";
        std::cout << "KR/Foreign common:   " << common.size() << "\n";
        live_smoke::print_balance_line("Bithumb", bithumb_balances, "KRW");
        live_smoke::print_balance_line("Upbit", upbit_balances, "KRW");
        live_smoke::print_balance_line("Bybit", bybit_balances, "USDT");
        live_smoke::print_balance_line("OKX", okx_balances, "USDT");
        if (!common.empty()) {
            std::cout << "Sample common bases: ";
            const std::size_t limit = std::min<std::size_t>(common.size(), 8);
            for (std::size_t i = 0; i < limit; ++i) {
                if (i) {
                    std::cout << ", ";
                }
                std::cout << common[i];
            }
            std::cout << "\n";
        }

        if (common.empty()) {
            std::cerr << "[FAIL] no common KR/foreign symbols\n";
            return 1;
        }

        std::cout << "[PASS] dotenv-loaded live entry smoke passed\n";
        kimp::Logger::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
