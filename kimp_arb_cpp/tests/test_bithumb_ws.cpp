#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/core/config.hpp"

#include <boost/asio/io_context.hpp>
#include <cmath>
#include <iostream>

namespace {

class TestBithumbExchange : public kimp::exchange::bithumb::BithumbExchange {
public:
    using kimp::exchange::bithumb::BithumbExchange::BithumbExchange;
    using kimp::exchange::bithumb::BithumbExchange::on_ws_message;
    using kimp::exchange::bithumb::BithumbExchange::set_ticker_callback;
};

bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
    boost::asio::io_context io_context;
    kimp::ExchangeCredentials creds;
    TestBithumbExchange exchange(io_context, creds);

    bool callback_fired = false;
    kimp::Ticker captured;
    exchange.set_ticker_callback([&](const kimp::Ticker& ticker) {
        callback_fired = true;
        captured = ticker;
    });

    const std::string payload =
        R"({"type":"ticker","content":{"symbol":"BTC_KRW","closePrice":"69905.0"}})";
    exchange.on_ws_message(payload);

    if (!callback_fired) {
        std::cerr << "FAIL: Bithumb ticker callback did not fire\n";
        return 1;
    }
    if (captured.exchange != kimp::Exchange::Bithumb) {
        std::cerr << "FAIL: exchange mismatch\n";
        return 1;
    }
    if (captured.symbol.to_string() != "BTC/KRW") {
        std::cerr << "FAIL: symbol mismatch: " << captured.symbol.to_string() << "\n";
        return 1;
    }
    if (!nearly_equal(captured.last, 69905.0)) {
        std::cerr << "FAIL: last price mismatch\n";
        return 1;
    }
    if (!nearly_equal(captured.bid, 0.0) || !nearly_equal(captured.ask, 0.0)) {
        std::cerr << "FAIL: ticker-only fast path should not invent BBO\n";
        return 1;
    }

    std::cout << "*** PASS: Bithumb ticker WS fast path reaches callback with correct ticker ***\n";
    return 0;
}
