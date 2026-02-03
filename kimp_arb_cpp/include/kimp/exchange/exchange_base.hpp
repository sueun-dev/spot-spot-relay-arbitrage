#pragma once

#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"
#include "kimp/network/websocket_client.hpp"
#include "kimp/network/connection_pool.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <future>

namespace kimp::exchange {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

/**
 * HTTP response wrapper
 */
struct HttpResponse {
    int status_code{0};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    bool success{false};
    std::string error;
};

/**
 * REST API client for exchanges with connection pooling
 *
 * Optimized for HFT:
 * - Persistent SSL connections (no handshake per request)
 * - DNS caching
 * - HTTP/1.1 keep-alive
 * - TCP_NODELAY for minimal latency
 */
class RestClient {
private:
    std::string host_;
    std::string port_;
    std::unique_ptr<network::ConnectionPool> connection_pool_;

public:
    RestClient(net::io_context& ioc, const std::string& host, const std::string& port = "443")
        : host_(host)
        , port_(port) {

        // Configure connection pool for HFT
        network::ConnectionPoolConfig config;
        config.pool_size = 4;           // 4 pre-established connections
        config.max_pool_size = 8;       // Up to 8 under load
        config.connect_timeout = std::chrono::seconds(5);
        config.idle_timeout = std::chrono::seconds(30);
        config.enable_tcp_nodelay = true;

        connection_pool_ = std::make_unique<network::ConnectionPool>(ioc, host, port, config);
    }

    // Initialize connection pool (call before use)
    bool initialize() {
        return connection_pool_->initialize();
    }

    // Shutdown connection pool
    void shutdown() {
        connection_pool_->shutdown();
    }

    // Get connection pool stats
    network::ConnectionPool::Stats get_pool_stats() const {
        return connection_pool_->get_stats();
    }

    // Synchronous GET request
    HttpResponse get(const std::string& target,
                     const std::unordered_map<std::string, std::string>& headers = {});

    // Synchronous POST request
    HttpResponse post(const std::string& target,
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers = {});

    // Synchronous DELETE request
    HttpResponse del(const std::string& target,
                     const std::unordered_map<std::string, std::string>& headers = {});

    // Async versions
    std::future<HttpResponse> get_async(const std::string& target,
                                         const std::unordered_map<std::string, std::string>& headers = {});

    std::future<HttpResponse> post_async(const std::string& target,
                                          const std::string& body,
                                          const std::unordered_map<std::string, std::string>& headers = {});

private:
    HttpResponse do_request(http::verb method,
                            const std::string& target,
                            const std::string& body,
                            const std::unordered_map<std::string, std::string>& headers);
};

/**
 * Base exchange interface
 */
class IExchange {
public:
    virtual ~IExchange() = default;

    // Exchange info
    virtual Exchange get_exchange_id() const = 0;
    virtual MarketType get_market_type() const = 0;
    virtual const std::string& get_name() const = 0;

    // Connection
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // WebSocket subscriptions
    virtual void subscribe_ticker(const std::vector<SymbolId>& symbols) = 0;
    virtual void subscribe_orderbook(const std::vector<SymbolId>& symbols) = 0;

    // Callbacks
    virtual void set_ticker_callback(TickerCallback cb) = 0;
    virtual void set_orderbook_callback(OrderBookCallback cb) = 0;
    virtual void set_order_callback(OrderCallback cb) = 0;

    // Market data queries
    virtual std::vector<SymbolId> get_available_symbols() = 0;
    virtual double get_funding_rate(const SymbolId& symbol) = 0;
    virtual std::vector<Ticker> fetch_all_tickers() = 0;  // REST API bulk fetch

    // Order execution
    virtual Order place_market_order(const SymbolId& symbol, Side side, Quantity quantity) = 0;
    virtual Order place_market_buy_cost(const SymbolId& symbol, Price cost) = 0;  // For Korean exchanges
    virtual bool cancel_order(uint64_t order_id) = 0;

    // Futures specific
    virtual bool set_leverage(const SymbolId& symbol, int leverage) = 0;
    virtual std::vector<Position> get_positions() = 0;
    virtual bool close_position(const SymbolId& symbol) = 0;

    // Balance
    virtual double get_balance(const std::string& currency) = 0;
};

/**
 * Base exchange implementation with common functionality
 */
class ExchangeBase : public IExchange, public std::enable_shared_from_this<ExchangeBase> {
protected:
    Exchange exchange_id_;
    MarketType market_type_;
    std::string name_;
    ExchangeCredentials credentials_;

    net::io_context& io_context_;
    std::shared_ptr<network::WebSocketClient> ws_client_;
    std::unique_ptr<RestClient> rest_client_;

    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> next_order_id_{1};

    // Callbacks
    TickerCallback ticker_callback_;
    OrderBookCallback orderbook_callback_;
    OrderCallback order_callback_;

    // Price cache
    mutable std::mutex price_mutex_;
    std::unordered_map<SymbolId, Ticker> ticker_cache_;

    // Event queue
    memory::SPSCRingBuffer<Ticker, 4096> ticker_queue_;

public:
    ExchangeBase(Exchange id, MarketType type, std::string name,
                 net::io_context& ioc, ExchangeCredentials creds)
        : exchange_id_(id)
        , market_type_(type)
        , name_(std::move(name))
        , credentials_(std::move(creds))
        , io_context_(ioc) {

        // Initialize REST client with connection pooling
        std::string host = extract_host(credentials_.rest_endpoint);
        rest_client_ = std::make_unique<RestClient>(io_context_, host);
    }

    // Initialize REST connection pool (call before using REST API)
    bool initialize_rest() {
        if (rest_client_) {
            return rest_client_->initialize();
        }
        return false;
    }

    // Shutdown REST connection pool
    void shutdown_rest() {
        if (rest_client_) {
            rest_client_->shutdown();
        }
    }

    // Get REST client stats for monitoring
    network::ConnectionPool::Stats get_rest_stats() const {
        if (rest_client_) {
            return rest_client_->get_pool_stats();
        }
        return {};
    }

    // IExchange implementation
    Exchange get_exchange_id() const override { return exchange_id_; }
    MarketType get_market_type() const override { return market_type_; }
    const std::string& get_name() const override { return name_; }
    bool is_connected() const override { return connected_.load(); }

    void set_ticker_callback(TickerCallback cb) override {
        ticker_callback_ = std::move(cb);
    }

    void set_orderbook_callback(OrderBookCallback cb) override {
        orderbook_callback_ = std::move(cb);
    }

    void set_order_callback(OrderCallback cb) override {
        order_callback_ = std::move(cb);
    }

    // Default implementations (futures-specific, override in spot exchanges)
    bool set_leverage(const SymbolId& symbol, int leverage) override {
        return false;  // Not applicable for spot
    }

    std::vector<Position> get_positions() override {
        return {};  // Not applicable for spot
    }

    bool close_position(const SymbolId& symbol) override {
        return false;  // Not applicable for spot
    }

    // Cost-based market buy (override in Korean exchanges)
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override {
        // Default: convert cost to quantity using current price
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = exchange_id_;
        return order;
    }

protected:
    // Dispatch callbacks
    void dispatch_ticker(const Ticker& ticker) {
        // Update cache
        {
            std::lock_guard lock(price_mutex_);
            ticker_cache_[ticker.symbol] = ticker;
        }

        // Notify callback
        if (ticker_callback_) {
            ticker_callback_(ticker);
        }
    }

    void dispatch_orderbook(const OrderBook& orderbook) {
        if (orderbook_callback_) {
            orderbook_callback_(orderbook);
        }
    }

    void dispatch_order(const Order& order) {
        if (order_callback_) {
            order_callback_(order);
        }
    }

    // Get cached ticker
    std::optional<Ticker> get_cached_ticker(const SymbolId& symbol) const {
        std::lock_guard lock(price_mutex_);
        auto it = ticker_cache_.find(symbol);
        if (it != ticker_cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Generate client order ID
    uint64_t generate_order_id() {
        return next_order_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // Extract host from URL
    static std::string extract_host(const std::string& url) {
        // Remove protocol
        std::string host = url;
        if (host.find("https://") == 0) host = host.substr(8);
        else if (host.find("http://") == 0) host = host.substr(7);

        // Remove path
        auto pos = host.find('/');
        if (pos != std::string::npos) host = host.substr(0, pos);

        // Remove port
        pos = host.find(':');
        if (pos != std::string::npos) host = host.substr(0, pos);

        return host;
    }

    // WebSocket message handler (to be overridden)
    virtual void on_ws_message(std::string_view message) = 0;
    virtual void on_ws_connected() = 0;
    virtual void on_ws_disconnected() = 0;
};

/**
 * Korean exchange base (Upbit, Bithumb)
 * Specialized for KRW spot markets
 */
class KoreanExchangeBase : public ExchangeBase {
protected:
    static constexpr double MIN_ORDER_KRW = 5000.0;

public:
    using ExchangeBase::ExchangeBase;

    // Korean exchanges are spot only
    MarketType get_market_type() const override { return MarketType::Spot; }

    // Get USDT/KRW price
    virtual double get_usdt_krw_price() = 0;

    // Korean exchanges need cost-based buying
    Order place_market_buy_cost(const SymbolId& symbol, Price cost) override = 0;
};

/**
 * Foreign futures exchange base (Bybit, Gate.io)
 * Specialized for USDT perpetual futures
 */
class ForeignFuturesExchangeBase : public ExchangeBase {
protected:
    static constexpr int DEFAULT_LEVERAGE = 1;

public:
    using ExchangeBase::ExchangeBase;

    MarketType get_market_type() const override { return MarketType::Perpetual; }

    // Futures-specific
    bool set_leverage(const SymbolId& symbol, int leverage) override = 0;
    std::vector<Position> get_positions() override = 0;
    bool close_position(const SymbolId& symbol) override = 0;
    double get_funding_rate(const SymbolId& symbol) override = 0;

    // Open short position
    virtual Order open_short(const SymbolId& symbol, Quantity quantity) = 0;

    // Close short position
    virtual Order close_short(const SymbolId& symbol, Quantity quantity) = 0;
};

} // namespace kimp::exchange
