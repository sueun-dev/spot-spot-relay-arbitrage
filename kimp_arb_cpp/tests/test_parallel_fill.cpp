#include "test_live_smoke_common.hpp"

int main() {
    try {
        live_smoke::init_test_logger("kimp_test_parallel_fill");
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

        auto bithumb_future = std::async(std::launch::async, [&] {
            return live_smoke::fetch_balances_or_throw("Bithumb", bithumb);
        });
        auto upbit_future = std::async(std::launch::async, [&] {
            return live_smoke::fetch_balances_or_throw("Upbit", upbit);
        });
        auto bybit_future = std::async(std::launch::async, [&] {
            return live_smoke::fetch_balances_or_throw("Bybit", bybit);
        });
        auto okx_future = std::async(std::launch::async, [&] {
            return live_smoke::fetch_balances_or_throw("OKX", okx);
        });

        const auto bithumb_balances = bithumb_future.get();
        const auto upbit_balances = upbit_future.get();
        const auto bybit_balances = bybit_future.get();
        const auto okx_balances = okx_future.get();

        std::cout << "=== Parallel Private REST Smoke ===\n";
        live_smoke::print_balance_line("Bithumb", bithumb_balances, "KRW");
        live_smoke::print_balance_line("Upbit", upbit_balances, "KRW");
        live_smoke::print_balance_line("Bybit", bybit_balances, "USDT");
        live_smoke::print_balance_line("OKX", okx_balances, "USDT");
        std::cout << "[PASS] parallel authenticated balance fetch passed\n";
        kimp::Logger::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
