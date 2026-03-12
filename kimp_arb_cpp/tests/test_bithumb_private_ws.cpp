#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/core/config.hpp"

#include <boost/asio/io_context.hpp>
#include <cmath>
#include <iostream>

namespace {

class TestBithumbExchange : public kimp::exchange::bithumb::BithumbExchange {
public:
    using kimp::exchange::bithumb::BithumbExchange::BithumbExchange;
    using kimp::exchange::bithumb::BithumbExchange::on_private_ws_message;
    using kimp::exchange::bithumb::BithumbExchange::query_order_detail;
};

bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

void assert_fill(TestBithumbExchange& exchange,
                 std::string_view order_id,
                 double expected_price,
                 double expected_qty) {
    kimp::Order order;
    kimp::SymbolId symbol("BTC", "KRW");

    if (!exchange.query_order_detail(std::string(order_id), symbol, order)) {
        std::cerr << "FAIL: expected WS fill for " << order_id << "\n";
        std::exit(1);
    }
    if (!nearly_equal(order.average_price, expected_price)) {
        std::cerr << "FAIL: avg price mismatch for " << order_id
                  << " got=" << order.average_price << " expected=" << expected_price << "\n";
        std::exit(1);
    }
    if (!nearly_equal(order.filled_quantity, expected_qty)) {
        std::cerr << "FAIL: filled qty mismatch for " << order_id
                  << " got=" << order.filled_quantity << " expected=" << expected_qty << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    boost::asio::io_context io_context;
    kimp::ExchangeCredentials creds;
    TestBithumbExchange exchange(io_context, creds);

    const std::string simple_payload =
        R"({"ty":"myOrder","cd":"KRW-BTC","uid":"simple-order","s":"done","ev":"0.1000","ef":"6990.5","tms":1773342407942})";
    exchange.on_private_ws_message(simple_payload);
    assert_fill(exchange, "simple-order", 69905.0, 0.1);

    const std::string default_payload =
        R"({"type":"myOrder","code":"KRW-BTC","uuid":"default-order","state":"trade","executed_volume":"0.2500","executed_funds":"17476.25","timestamp":1773342407943})";
    exchange.on_private_ws_message(default_payload);
    assert_fill(exchange, "default-order", 69905.0, 0.25);

    std::cout << "*** PASS: Bithumb private myOrder WS messages populate fill cache for query_order_detail ***\n";
    return 0;
}
