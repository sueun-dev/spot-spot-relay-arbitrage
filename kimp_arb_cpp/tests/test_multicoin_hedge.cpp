#include "test_live_smoke_common.hpp"

int main() {
    try {
        live_smoke::init_test_logger("kimp_test_multicoin_hedge");
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

        const auto all_four = live_smoke::set_intersection(
            live_smoke::base_set(bithumb_symbols),
            live_smoke::base_set(upbit_symbols));
        const auto foreign_bases = live_smoke::set_intersection(
            live_smoke::base_set(bybit_symbols),
            live_smoke::base_set(okx_symbols));

        std::unordered_set<std::string> all_four_set(all_four.begin(), all_four.end());
        const auto relay_bases = live_smoke::set_intersection(all_four_set,
                                                              std::unordered_set<std::string>(foreign_bases.begin(),
                                                                                              foreign_bases.end()));

        std::vector<std::string> filtered;
        for (const auto& base : relay_bases) {
            if (base != "BTC" && base != "ETH") {
                filtered.push_back(base);
            }
        }
        std::sort(filtered.begin(), filtered.end());

        std::cout << "=== Multi-Coin Relay Smoke ===\n";
        std::cout << "All-four KR venues overlap: " << all_four.size() << "\n";
        std::cout << "All-four foreign overlap:   " << foreign_bases.size() << "\n";
        std::cout << "Relay-ready bases sample:   " << filtered.size() << "\n";
        const std::size_t limit = std::min<std::size_t>(filtered.size(), 8);
        for (std::size_t i = 0; i < limit; ++i) {
            if (i == 0) {
                std::cout << "Sample: ";
            } else {
                std::cout << ", ";
            }
            std::cout << filtered[i];
        }
        if (limit > 0) {
            std::cout << "\n";
        }

        if (filtered.size() < 5) {
            std::cerr << "[FAIL] fewer than 5 multi-coin relay candidates\n";
            return 1;
        }

        std::cout << "[PASS] multi-coin relay smoke passed\n";
        kimp::Logger::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
