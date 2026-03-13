#include "kimp/execution/order_manager.hpp"
#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/exchange/bybit/bybit.hpp"

#include <boost/asio.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace net = boost::asio;

struct FillSpec {
    double quantity{0.0};
    double price{0.0};
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeBybitExchange final : public kimp::exchange::bybit::BybitExchange {
public:
    FakeBybitExchange(net::io_context& ioc, std::vector<FillSpec> fills)
        : kimp::exchange::bybit::BybitExchange(ioc, {}),
          fills_(std::move(fills)) {}

    bool connect() override { return true; }
    void disconnect() override {}
    void subscribe_ticker(const std::vector<kimp::SymbolId>&) override {}
    void subscribe_orderbook(const std::vector<kimp::SymbolId>&) override {}
    std::vector<kimp::SymbolId> get_available_symbols() override { return {}; }
    std::vector<kimp::Ticker> fetch_all_tickers() override { return {}; }
    kimp::Order place_market_order(const kimp::SymbolId&, kimp::Side, kimp::Quantity) override { return {}; }
    bool cancel_order(uint64_t) override { return false; }
    bool prepare_shorting(const kimp::SymbolId&) override { return true; }
    std::vector<kimp::Position> get_short_positions() override { return {}; }
    bool close_short_position(const kimp::SymbolId&) override { return true; }
    kimp::Order open_short(const kimp::SymbolId&, kimp::Quantity) override { return {}; }
    double get_balance(const std::string&) override { return 0.0; }

    kimp::Order close_short(const kimp::SymbolId& symbol, kimp::Quantity quantity) override {
        require(next_fill_ < fills_.size(), "missing fake bybit close fill");
        const auto& fill = fills_[next_fill_++];
        kimp::Order order;
        order.exchange = kimp::Exchange::Bybit;
        order.symbol = symbol;
        order.side = kimp::Side::Buy;
        order.type = kimp::OrderType::Market;
        order.status = kimp::OrderStatus::Filled;
        order.quantity = quantity;
        order.filled_quantity = fill.quantity;
        order.average_price = fill.price;
        return order;
    }

private:
    std::vector<FillSpec> fills_;
    std::size_t next_fill_{0};
};

class FakeBithumbExchange final : public kimp::exchange::bithumb::BithumbExchange {
public:
    FakeBithumbExchange(net::io_context& ioc, std::vector<FillSpec> sells)
        : kimp::exchange::bithumb::BithumbExchange(ioc, {}),
          sells_(std::move(sells)) {}

    bool connect() override { return true; }
    void disconnect() override {}
    void subscribe_ticker(const std::vector<kimp::SymbolId>&) override {}
    void subscribe_orderbook(const std::vector<kimp::SymbolId>&) override {}
    std::vector<kimp::SymbolId> get_available_symbols() override { return {}; }
    std::vector<kimp::Ticker> fetch_all_tickers() override { return {}; }
    double get_usdt_krw_price() override { return 1300.0; }
    bool cancel_order(uint64_t) override { return false; }
    double get_balance(const std::string&) override { return 0.0; }

    kimp::Order place_market_order(const kimp::SymbolId& symbol,
                                   kimp::Side side,
                                   kimp::Quantity quantity) override {
        require(side == kimp::Side::Sell, "fake bithumb only supports sell");
        require(next_sell_ < sells_.size(), "missing fake bithumb sell fill");
        const auto& fill = sells_[next_sell_++];
        kimp::Order order;
        order.exchange = kimp::Exchange::Bithumb;
        order.symbol = symbol;
        order.side = side;
        order.type = kimp::OrderType::Market;
        order.status = kimp::OrderStatus::Filled;
        order.quantity = quantity;
        order.filled_quantity = fill.quantity;
        order.average_price = fill.price;
        return order;
    }

    kimp::Order place_market_buy_cost(const kimp::SymbolId&, kimp::Price) override {
        return {};
    }

private:
    std::vector<FillSpec> sells_;
    std::size_t next_sell_{0};
};

}  // namespace

int main() {
    net::io_context ioc;

    auto bithumb = std::make_shared<FakeBithumbExchange>(
        ioc, std::vector<FillSpec>{{7.0, 13100.0}, {3.0, 12900.0}});
    auto bybit = std::make_shared<FakeBybitExchange>(
        ioc, std::vector<FillSpec>{{7.0, 9.5}, {3.0, 9.8}});

    kimp::execution::OrderManager manager;
    manager.set_exchange(kimp::Exchange::Bithumb, bithumb);
    manager.set_exchange(kimp::Exchange::Bybit, bybit);

    kimp::Position position;
    position.symbol = kimp::SymbolId("BTC", "KRW");
    position.korean_exchange = kimp::Exchange::Bithumb;
    position.foreign_exchange = kimp::Exchange::Bybit;
    position.korean_amount = 10.0;
    position.foreign_amount = 10.0;
    position.korean_entry_price = 10000.0;
    position.foreign_entry_price = 10.0;
    position.entry_premium = 0.0;
    position.position_size_usd = 100.0;
    position.is_active = true;

    kimp::ExitSignal signal;
    signal.trace_id = 1;
    signal.trace_start_ns = kimp::LatencyProbe::instance().capture_now_ns();
    signal.symbol = position.symbol;
    signal.korean_exchange = position.korean_exchange;
    signal.foreign_exchange = position.foreign_exchange;
    signal.premium = 0.7692307692;
    signal.korean_bid = 13100.0;
    signal.foreign_ask = 10.0;
    signal.usdt_krw_rate = 1300.0;

    const auto result = manager.execute_spot_relay_exit(signal, position);

    const double expected_total_pnl_krw =
        ((13100.0 - 10000.0) * 7.0 + (10.0 - 9.5) * 1300.0 * 7.0) +
        ((12900.0 - 10000.0) * 3.0 + (10.0 - 9.8) * 1300.0 * 3.0);
    const double expected_korean_exit =
        ((13100.0 * 7.0) + (12900.0 * 3.0)) / 10.0;
    const double expected_foreign_exit =
        ((9.5 * 7.0) + (9.8 * 3.0)) / 10.0;

    require(result.success, "exit execution should succeed");
    require(!result.position.is_active, "position should be closed");
    require(std::abs(result.position.realized_pnl_krw - expected_total_pnl_krw) < 1e-6,
            "realized_pnl_krw mismatch");
    require(std::abs(result.position.korean_exit_price - expected_korean_exit) < 1e-9,
            "korean_exit_price mismatch");
    require(std::abs(result.position.foreign_exit_price - expected_foreign_exit) < 1e-9,
            "foreign_exit_price mismatch");

    std::cout << "*** PASS: order manager exit P&L matches actual split fills ***\n";
    return 0;
}
