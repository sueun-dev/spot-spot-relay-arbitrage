#pragma once

#include "test_live_smoke_common.hpp"

#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"

#include <simdjson.h>

#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace live_trade {

using kimp::Exchange;
using kimp::Order;
using kimp::OrderStatus;
using kimp::Price;
using kimp::Quantity;
using kimp::SymbolId;
using kimp::exchange::AccountBalance;
using kimp::exchange::HttpResponse;
using kimp::exchange::RestClient;

struct PairSpec {
    Exchange korean{Exchange::Bithumb};
    Exchange foreign{Exchange::Bybit};
    std::string label{"Bi-By"};
};

struct TopQuote {
    Price bid{0.0};
    Price ask{0.0};
    Quantity bid_qty{0.0};
    Quantity ask_qty{0.0};
    bool valid() const noexcept {
        return bid > 0.0 && ask > 0.0 && bid_qty > 0.0 && ask_qty > 0.0;
    }
};

inline std::string extract_host(const std::string& url) {
    std::string host = url;
    if (host.rfind("https://", 0) == 0) {
        host = host.substr(8);
    } else if (host.rfind("http://", 0) == 0) {
        host = host.substr(7);
    }
    auto slash = host.find('/');
    if (slash != std::string::npos) {
        host = host.substr(0, slash);
    }
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    return host;
}

inline PairSpec parse_pair(const std::string& value) {
    if (value == "Bi-By") return {Exchange::Bithumb, Exchange::Bybit, value};
    if (value == "Bi-Ok") return {Exchange::Bithumb, Exchange::OKX, value};
    if (value == "Up-By") return {Exchange::Upbit, Exchange::Bybit, value};
    if (value == "Up-Ok") return {Exchange::Upbit, Exchange::OKX, value};
    throw std::runtime_error("unsupported pair: " + value);
}

inline double total_balance_of(const std::vector<AccountBalance>& balances, const std::string& currency) {
    for (const auto& balance : balances) {
        if (balance.currency == currency) {
            return balance.total;
        }
    }
    return 0.0;
}

inline double liability_of(const std::vector<AccountBalance>& balances, const std::string& currency) {
    for (const auto& balance : balances) {
        if (balance.currency == currency) {
            return balance.liability;
        }
    }
    return 0.0;
}

inline TopQuote parse_bithumb_quote(const HttpResponse& response) {
    TopQuote quote;
    simdjson::dom::parser parser;
    auto doc = parser.parse(response.body);
    auto data = doc["data"];
    auto bids = data["bids"];
    auto asks = data["asks"];
    if (bids.error() || asks.error()) {
        return quote;
    }
    auto bid0 = bids.at(0);
    auto ask0 = asks.at(0);
    quote.bid = std::stod(std::string(bid0["price"].get_string().value()));
    quote.bid_qty = std::stod(std::string(bid0["quantity"].get_string().value()));
    quote.ask = std::stod(std::string(ask0["price"].get_string().value()));
    quote.ask_qty = std::stod(std::string(ask0["quantity"].get_string().value()));
    return quote;
}

inline TopQuote parse_upbit_quote(const HttpResponse& response) {
    TopQuote quote;
    simdjson::dom::parser parser;
    auto doc = parser.parse(response.body);
    auto first = doc.at(0);
    auto units = first["orderbook_units"];
    if (units.error()) {
        return quote;
    }
    auto level0 = units.at(0);
    quote.bid = double(level0["bid_price"]);
    quote.bid_qty = double(level0["bid_size"]);
    quote.ask = double(level0["ask_price"]);
    quote.ask_qty = double(level0["ask_size"]);
    return quote;
}

inline TopQuote parse_bybit_quote(const HttpResponse& response) {
    TopQuote quote;
    simdjson::dom::parser parser;
    auto doc = parser.parse(response.body);
    if (int64_t(doc["retCode"]) != 0) {
        return quote;
    }
    auto bids = doc["result"]["b"];
    auto asks = doc["result"]["a"];
    auto bid0 = bids.at(0);
    auto ask0 = asks.at(0);
    quote.bid = std::stod(std::string(bid0.at(0).get_string().value()));
    quote.bid_qty = std::stod(std::string(bid0.at(1).get_string().value()));
    quote.ask = std::stod(std::string(ask0.at(0).get_string().value()));
    quote.ask_qty = std::stod(std::string(ask0.at(1).get_string().value()));
    return quote;
}

inline TopQuote parse_okx_quote(const HttpResponse& response) {
    TopQuote quote;
    simdjson::dom::parser parser;
    auto doc = parser.parse(response.body);
    if (std::string(doc["code"].get_string().value()) != "0") {
        return quote;
    }
    auto book = doc["data"].at(0);
    auto bid0 = book["bids"].at(0);
    auto ask0 = book["asks"].at(0);
    quote.bid = std::stod(std::string(bid0.at(0).get_string().value()));
    quote.bid_qty = std::stod(std::string(bid0.at(1).get_string().value()));
    quote.ask = std::stod(std::string(ask0.at(0).get_string().value()));
    quote.ask_qty = std::stod(std::string(ask0.at(1).get_string().value()));
    return quote;
}

inline TopQuote fetch_bithumb_quote(boost::asio::io_context& io_context,
                                    const live_smoke::RuntimeConfig& config,
                                    const SymbolId& symbol) {
    RestClient client(io_context, extract_host(config.exchanges.at(Exchange::Bithumb).rest_endpoint));
    if (!client.initialize()) {
        throw std::runtime_error("Bithumb public REST init failed");
    }
    auto response = client.get("/public/orderbook/" + symbol.to_bithumb_format() + "?count=1");
    client.shutdown();
    if (!response.success) {
        throw std::runtime_error("Bithumb quote fetch failed: " + response.error);
    }
    auto quote = parse_bithumb_quote(response);
    if (!quote.valid()) {
        throw std::runtime_error("Bithumb quote parse failed for " + symbol.to_string());
    }
    return quote;
}

inline TopQuote fetch_upbit_quote(boost::asio::io_context& io_context,
                                  const live_smoke::RuntimeConfig& config,
                                  const SymbolId& symbol) {
    RestClient client(io_context, extract_host(config.exchanges.at(Exchange::Upbit).rest_endpoint));
    if (!client.initialize()) {
        throw std::runtime_error("Upbit public REST init failed");
    }
    auto response = client.get("/v1/orderbook?markets=" + symbol.to_upbit_format());
    client.shutdown();
    if (!response.success) {
        throw std::runtime_error("Upbit quote fetch failed: " + response.error);
    }
    auto quote = parse_upbit_quote(response);
    if (!quote.valid()) {
        throw std::runtime_error("Upbit quote parse failed for " + symbol.to_string());
    }
    return quote;
}

inline TopQuote fetch_bybit_quote(boost::asio::io_context& io_context,
                                  const live_smoke::RuntimeConfig& config,
                                  const SymbolId& symbol) {
    RestClient client(io_context, extract_host(config.exchanges.at(Exchange::Bybit).rest_endpoint));
    if (!client.initialize()) {
        throw std::runtime_error("Bybit public REST init failed");
    }
    auto response = client.get("/v5/market/orderbook?category=spot&symbol=" + symbol.to_bybit_format() + "&limit=1");
    client.shutdown();
    if (!response.success) {
        throw std::runtime_error("Bybit quote fetch failed: " + response.error);
    }
    auto quote = parse_bybit_quote(response);
    if (!quote.valid()) {
        throw std::runtime_error("Bybit quote parse failed for " + symbol.to_string());
    }
    return quote;
}

inline TopQuote fetch_okx_quote(boost::asio::io_context& io_context,
                                const live_smoke::RuntimeConfig& config,
                                const SymbolId& symbol) {
    RestClient client(io_context, extract_host(config.exchanges.at(Exchange::OKX).rest_endpoint));
    if (!client.initialize()) {
        throw std::runtime_error("OKX public REST init failed");
    }
    auto response = client.get("/api/v5/market/books?instId=" + std::string(symbol.get_base()) + "-" +
                               std::string(symbol.get_quote()) + "&sz=1");
    client.shutdown();
    if (!response.success) {
        throw std::runtime_error("OKX quote fetch failed: " + response.error);
    }
    auto quote = parse_okx_quote(response);
    if (!quote.valid()) {
        throw std::runtime_error("OKX quote parse failed for " + symbol.to_string());
    }
    return quote;
}

inline TopQuote fetch_korean_quote(boost::asio::io_context& io_context,
                                   const live_smoke::RuntimeConfig& config,
                                   Exchange ex,
                                   const SymbolId& symbol) {
    if (ex == Exchange::Bithumb) {
        return fetch_bithumb_quote(io_context, config, symbol);
    }
    return fetch_upbit_quote(io_context, config, symbol);
}

inline TopQuote fetch_foreign_quote(boost::asio::io_context& io_context,
                                    const live_smoke::RuntimeConfig& config,
                                    Exchange ex,
                                    const SymbolId& symbol) {
    if (ex == Exchange::OKX) {
        return fetch_okx_quote(io_context, config, symbol);
    }
    return fetch_bybit_quote(io_context, config, symbol);
}

inline double resolved_fill_quantity(const Order& order) {
    return order.filled_quantity > 0.0 ? order.filled_quantity : order.quantity;
}

inline double resolved_fill_price(const Order& order, double fallback) {
    return order.average_price > 0.0 ? order.average_price : fallback;
}

inline bool quantities_match(double lhs, double rhs) {
    const double scale = std::max({1.0, std::fabs(lhs), std::fabs(rhs)});
    return std::fabs(lhs - rhs) <= scale * 1e-6;
}

template <typename ForeignPtr>
bool wait_foreign_fill(Exchange ex, const SymbolId& symbol, const ForeignPtr& foreign_ex, Order& order) {
    for (int attempt = 0; attempt < 15; ++attempt) {
        bool ok = false;
        if (ex == Exchange::OKX) {
            ok = std::dynamic_pointer_cast<kimp::exchange::okx::OkxExchange>(foreign_ex)
                     ->query_order_fill(order.order_id_str, symbol, order);
        } else {
            ok = std::dynamic_pointer_cast<kimp::exchange::bybit::BybitExchange>(foreign_ex)
                     ->query_order_fill(order.order_id_str, order);
        }
        if (ok && order.status == OrderStatus::Filled) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return order.status == OrderStatus::Filled;
}

template <typename KoreanPtr>
bool wait_korean_fill(Exchange ex, const SymbolId& symbol, const KoreanPtr& korean_ex, Order& order) {
    for (int attempt = 0; attempt < 15; ++attempt) {
        bool ok = false;
        if (ex == Exchange::Bithumb) {
            ok = std::dynamic_pointer_cast<kimp::exchange::bithumb::BithumbExchange>(korean_ex)
                     ->query_order_detail(order.order_id_str, symbol, order);
        } else {
            ok = std::dynamic_pointer_cast<kimp::exchange::upbit::UpbitExchange>(korean_ex)
                     ->query_order_detail(order.order_id_str, order);
        }
        if (ok && order.status == OrderStatus::Filled) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return order.status == OrderStatus::Filled;
}

inline void print_order_summary(const std::string& label, const Order& order, double fallback_price) {
    std::cout << label
              << " status=" << static_cast<int>(order.status)
              << " qty=" << resolved_fill_quantity(order)
              << " avg=" << resolved_fill_price(order, fallback_price)
              << " orderId=" << order.order_id_str << "\n";
}

} // namespace live_trade
