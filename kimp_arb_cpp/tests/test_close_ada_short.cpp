#include "test_live_smoke_common.hpp"
#include "kimp/exchange/exchange_base.hpp"

#include <cmath>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    try {
        live_smoke::init_test_logger("kimp_close_ada_short");
        const auto config = live_smoke::load_runtime_config_or_throw();
        live_smoke::require_exchange_creds(config, kimp::Exchange::Bybit);

        boost::asio::io_context io_context;
        auto bybit = std::make_shared<kimp::exchange::bybit::BybitExchange>(
            io_context, config.exchanges.at(kimp::Exchange::Bybit));

        if (!bybit->initialize_rest()) {
            throw std::runtime_error("Bybit REST init failed");
        }

        bybit->get_available_symbols();

        auto balances = bybit->get_all_balances();
        double ada_available = 0.0;
        for (const auto& b : balances) {
            if (b.currency == "ADA") {
                ada_available = b.available;
                std::cout << "ADA total=" << b.total
                          << " available=" << b.available
                          << " liability=" << b.liability << "\n";
            }
            if (b.currency == "USDT") {
                std::cout << "USDT total=" << b.total
                          << " available=" << b.available << "\n";
            }
        }

        if (ada_available <= 0.0) {
            std::cout << "No ADA to sell.\n";
            bybit->shutdown_rest();
            kimp::Logger::shutdown();
            return 0;
        }

        // ADA lot step is 1 on Bybit — floor to integer
        double sell_qty = std::floor(ada_available);
        if (sell_qty <= 0.0) {
            std::cout << "ADA available < 1, nothing to sell.\n";
            bybit->shutdown_rest();
            kimp::Logger::shutdown();
            return 0;
        }

        kimp::SymbolId ada_usdt("ADA", "USDT");
        std::cout << "Selling " << sell_qty << " ADA (floored from " << ada_available << ")...\n";
        auto order = bybit->place_market_order(ada_usdt, kimp::Side::Sell, sell_qty);
        std::cout << "Order status=" << static_cast<int>(order.status)
                  << " orderId=" << order.order_id_str
                  << " filled=" << order.filled_quantity
                  << " avg_price=" << order.average_price << "\n";

        if (order.status != kimp::OrderStatus::Filled && !order.order_id_str.empty()) {
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (bybit->query_order_fill(order.order_id_str, order)) {
                    if (order.status == kimp::OrderStatus::Filled) break;
                }
            }
            std::cout << "After poll: status=" << static_cast<int>(order.status)
                      << " filled=" << order.filled_quantity << "\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto after = bybit->get_all_balances();
        for (const auto& b : after) {
            if (b.currency == "ADA" || b.currency == "USDT") {
                std::cout << "AFTER " << b.currency
                          << " total=" << b.total
                          << " available=" << b.available
                          << " liability=" << b.liability << "\n";
            }
        }

        bybit->shutdown_rest();
        kimp::Logger::shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        kimp::Logger::shutdown();
        return 1;
    }
}
