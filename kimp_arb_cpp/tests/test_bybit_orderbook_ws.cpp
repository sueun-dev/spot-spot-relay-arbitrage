#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/core/config.hpp"

#include <boost/asio/io_context.hpp>
#include <cmath>
#include <iostream>

namespace {

class TestBybitExchange : public kimp::exchange::bybit::BybitExchange {
public:
    using kimp::exchange::bybit::BybitExchange::BybitExchange;
    using kimp::exchange::bybit::BybitExchange::on_ws_message;
    using kimp::exchange::bybit::BybitExchange::set_ticker_callback;
};

bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
    boost::asio::io_context io_context;
    kimp::ExchangeCredentials creds;
    TestBybitExchange exchange(io_context, creds);

    bool callback_fired = false;
    kimp::Ticker captured;
    exchange.set_ticker_callback([&](const kimp::Ticker& ticker) {
        callback_fired = true;
        captured = ticker;
    });

    const std::string payload =
        R"({"topic":"orderbook.1.BTCUSDT","ts":1773342407942,"type":"snapshot","data":{"s":"BTCUSDT","b":[["69904.3","0.347143"]],"a":[["69904.4","0.390684"]],"u":141506058,"seq":101969058574},"cts":1773342407934})";

    exchange.on_ws_message(payload);

    if (!callback_fired) {
        std::cerr << "FAIL: Bybit orderbook callback did not fire\n";
        return 1;
    }
    if (captured.exchange != kimp::Exchange::Bybit) {
        std::cerr << "FAIL: exchange mismatch\n";
        return 1;
    }
    if (captured.symbol.to_string() != "BTC/USDT") {
        std::cerr << "FAIL: symbol mismatch: " << captured.symbol.to_string() << "\n";
        return 1;
    }
    if (!nearly_equal(captured.bid, 69904.3) ||
        !nearly_equal(captured.ask, 69904.4) ||
        !nearly_equal(captured.bid_qty, 0.347143) ||
        !nearly_equal(captured.ask_qty, 0.390684)) {
        std::cerr << "FAIL: parsed BBO mismatch\n";
        return 1;
    }
    if (captured.last <= 0.0) {
        std::cerr << "FAIL: derived last price missing\n";
        return 1;
    }

    std::cout << "*** PASS: Bybit orderbook WS message reaches callback with correct BBO ***\n";
    return 0;
}
