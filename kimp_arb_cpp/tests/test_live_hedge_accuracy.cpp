#include "test_live_smoke_common.hpp"

int main() {
    try {
        live_smoke::init_test_logger("kimp_test_live_hedge");
        const auto config = live_smoke::load_runtime_config_or_throw();
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bithumb);
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bybit);

        boost::asio::io_context io_context;
        auto bithumb = std::make_shared<kimp::exchange::bithumb::BithumbExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bithumb));
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bybit));

        const auto bithumb_balances = live_smoke::fetch_balances_or_throw("Bithumb", bithumb);
        const auto bybit_balances = live_smoke::fetch_balances_or_throw("Bybit", bybit);
        const auto bithumb_symbols = live_smoke::fetch_symbols_or_throw("Bithumb", bithumb);
        const auto bybit_symbols = live_smoke::fetch_symbols_or_throw("Bybit", bybit);

        const auto common = live_smoke::set_intersection(
            live_smoke::base_set(bithumb_symbols),
            live_smoke::base_set(bybit_symbols));

        const double bithumb_krw = live_smoke::available_of(bithumb_balances, "KRW");
        const double bybit_usdt = live_smoke::available_of(bybit_balances, "USDT");

        std::cout << "=== Live Hedge Smoke: Bithumb ↔ Bybit ===\n";
        std::cout << "Common bases: " << common.size() << "\n";
        std::cout << "Bithumb KRW available: " << bithumb_krw << "\n";
        std::cout << "Bybit USDT available:  " << bybit_usdt << "\n";
        if (!common.empty()) {
            std::cout << "First common base: " << common.front() << "\n";
        }

        if (common.empty()) {
            std::cerr << "[FAIL] no Bithumb/Bybit common bases\n";
            return 1;
        }
        if (bithumb_krw < 5000.0) {
            std::cerr << "[FAIL] Bithumb KRW below minimum order threshold\n";
            return 1;
        }
        if (bybit_usdt < 20.0) {
            std::cerr << "[FAIL] Bybit USDT too low for smoke readiness\n";
            return 1;
        }

        std::cout << "[PASS] hedge readiness smoke passed\n";
        kimp::Logger::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
