#pragma once

#include "kimp/core/optimization.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <condition_variable>

namespace kimp::network {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

/**
 * Configuration for connection pool
 */
struct ConnectionPoolConfig {
    std::size_t pool_size{4};                    // Number of connections to maintain
    std::size_t max_pool_size{8};                // Maximum connections under load
    std::chrono::seconds connect_timeout{5};     // Connection timeout
    std::chrono::seconds idle_timeout{30};       // Close idle connections after this
    std::chrono::seconds keepalive_interval{15}; // Send keepalive every N seconds
    bool enable_tcp_nodelay{true};               // Disable Nagle's algorithm
    int tcp_keepalive_idle{10};                  // TCP keepalive idle time (seconds)
    int tcp_keepalive_interval{5};               // TCP keepalive interval (seconds)
};

/**
 * Pooled SSL connection with health tracking
 */
class PooledConnection {
public:
    using SslStream = beast::ssl_stream<beast::tcp_stream>;

    enum class State {
        Disconnected,
        Connecting,
        Connected,
        InUse,
        Failed
    };

private:
    std::unique_ptr<SslStream> stream_;
    std::atomic<State> state_{State::Disconnected};
    std::chrono::steady_clock::time_point last_used_;
    std::chrono::steady_clock::time_point created_;
    std::atomic<uint64_t> request_count_{0};
    std::atomic<uint64_t> error_count_{0};

public:
    PooledConnection() = default;

    // Non-copyable, non-movable (due to atomics)
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&) = delete;
    PooledConnection& operator=(PooledConnection&&) = delete;

    void set_stream(std::unique_ptr<SslStream> stream) {
        stream_ = std::move(stream);
        created_ = std::chrono::steady_clock::now();
        last_used_ = created_;
        state_.store(State::Connected, std::memory_order_release);
    }

    SslStream* stream() { return stream_.get(); }
    const SslStream* stream() const { return stream_.get(); }

    State state() const { return state_.load(std::memory_order_acquire); }
    void set_state(State s) { state_.store(s, std::memory_order_release); }

    bool try_acquire() {
        State expected = State::Connected;
        return state_.compare_exchange_strong(expected, State::InUse,
                                               std::memory_order_acq_rel);
    }

    void release() {
        last_used_ = std::chrono::steady_clock::now();
        request_count_.fetch_add(1, std::memory_order_relaxed);
        state_.store(State::Connected, std::memory_order_release);
    }

    void mark_failed() {
        error_count_.fetch_add(1, std::memory_order_relaxed);
        state_.store(State::Failed, std::memory_order_release);
    }

    void reset() {
        stream_.reset();
        state_.store(State::Disconnected, std::memory_order_release);
    }

    bool is_idle(std::chrono::seconds timeout) const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - last_used_) > timeout;
    }

    auto age() const {
        return std::chrono::steady_clock::now() - created_;
    }

    uint64_t request_count() const { return request_count_.load(std::memory_order_relaxed); }
    uint64_t error_count() const { return error_count_.load(std::memory_order_relaxed); }
};

/**
 * Thread-safe SSL connection pool with DNS caching
 *
 * Optimized for HFT:
 * - Pre-established connections (no handshake latency)
 * - DNS resolution cached
 * - HTTP/1.1 keep-alive
 * - TCP_NODELAY for minimal latency
 * - Automatic reconnection on failure
 */
class ConnectionPool {
public:
    using SslStream = PooledConnection::SslStream;

private:
    net::io_context& io_context_;
    ssl::context ssl_context_;
    std::string host_;
    std::string port_;
    ConnectionPoolConfig config_;

    // Connection pool
    std::deque<std::unique_ptr<PooledConnection>> connections_;
    mutable std::mutex pool_mutex_;
    std::condition_variable pool_cv_;

    // Cached DNS resolution
    tcp::resolver::results_type cached_endpoints_;
    std::chrono::steady_clock::time_point dns_cache_time_;
    static constexpr auto DNS_CACHE_TTL = std::chrono::minutes(5);
    mutable std::mutex dns_mutex_;

    // Stats
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> pool_hits_{0};
    std::atomic<uint64_t> pool_misses_{0};
    std::atomic<uint64_t> reconnections_{0};

    std::atomic<bool> running_{false};

public:
    ConnectionPool(net::io_context& ioc,
                   const std::string& host,
                   const std::string& port = "443",
                   ConnectionPoolConfig config = {})
        : io_context_(ioc)
        , ssl_context_(ssl::context::tlsv12_client)
        , host_(host)
        , port_(port)
        , config_(config) {

        // Configure SSL
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_peer);
    }

    ~ConnectionPool() {
        shutdown();
    }

    // Initialize pool with pre-established connections
    bool initialize() {
        if (running_.exchange(true)) {
            return true;  // Already running
        }

        // Pre-resolve DNS
        if (!refresh_dns_cache()) {
            running_.store(false);
            return false;
        }

        // Pre-establish connections
        std::lock_guard lock(pool_mutex_);
        for (std::size_t i = 0; i < config_.pool_size; ++i) {
            auto conn = create_connection();
            if (conn) {
                connections_.push_back(std::move(conn));
            }
        }

        return !connections_.empty();
    }

    void shutdown() {
        if (!running_.exchange(false)) {
            return;
        }

        std::lock_guard lock(pool_mutex_);
        for (auto& conn : connections_) {
            if (conn && conn->stream()) {
                beast::error_code ec;
                conn->stream()->shutdown(ec);
            }
        }
        connections_.clear();
    }

    /**
     * Acquire a connection from the pool
     * Returns nullptr if no connection available within timeout
     */
    PooledConnection* acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        total_requests_.fetch_add(1, std::memory_order_relaxed);

        std::unique_lock lock(pool_mutex_);

        // Try to find an available connection
        for (auto& conn : connections_) {
            if (conn && conn->try_acquire()) {
                pool_hits_.fetch_add(1, std::memory_order_relaxed);
                return conn.get();
            }
        }

        // No available connection - try to create new one if under max
        if (connections_.size() < config_.max_pool_size) {
            lock.unlock();
            auto new_conn = create_connection();
            lock.lock();

            if (new_conn) {
                new_conn->set_state(PooledConnection::State::InUse);
                connections_.push_back(std::move(new_conn));
                pool_misses_.fetch_add(1, std::memory_order_relaxed);
                return connections_.back().get();
            }
        }

        // Wait for a connection to become available
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            pool_cv_.wait_until(lock, deadline);

            for (auto& conn : connections_) {
                if (conn && conn->try_acquire()) {
                    pool_hits_.fetch_add(1, std::memory_order_relaxed);
                    return conn.get();
                }
            }
        }

        pool_misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    /**
     * Release a connection back to the pool
     */
    void release(PooledConnection* conn) {
        if (!conn) return;

        conn->release();
        pool_cv_.notify_one();
    }

    /**
     * Mark connection as failed and trigger replacement
     */
    void mark_failed(PooledConnection* conn) {
        if (!conn) return;

        conn->mark_failed();
        reconnections_.fetch_add(1, std::memory_order_relaxed);

        // Synchronous replacement so retry gets a fresh connection immediately
        replace_connection(conn);

        pool_cv_.notify_one();
    }

    // Accessors
    const std::string& host() const { return host_; }
    const std::string& port() const { return port_; }
    ssl::context& ssl_context() { return ssl_context_; }

    // Stats
    struct Stats {
        uint64_t total_requests;
        uint64_t pool_hits;
        uint64_t pool_misses;
        uint64_t reconnections;
        std::size_t pool_size;
        std::size_t available_connections;
    };

    Stats get_stats() const {
        std::lock_guard lock(pool_mutex_);
        std::size_t available = 0;
        for (const auto& conn : connections_) {
            if (conn && conn->state() == PooledConnection::State::Connected) {
                ++available;
            }
        }
        return Stats{
            total_requests_.load(),
            pool_hits_.load(),
            pool_misses_.load(),
            reconnections_.load(),
            connections_.size(),
            available
        };
    }

private:
    bool refresh_dns_cache() {
        try {
            tcp::resolver resolver(io_context_);
            cached_endpoints_ = resolver.resolve(host_, port_);
            dns_cache_time_ = std::chrono::steady_clock::now();
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    tcp::resolver::results_type get_endpoints() {
        std::lock_guard lock(dns_mutex_);

        auto now = std::chrono::steady_clock::now();
        if (now - dns_cache_time_ > DNS_CACHE_TTL) {
            refresh_dns_cache();
        }

        return cached_endpoints_;
    }

    std::unique_ptr<PooledConnection> create_connection() {
        try {
            auto conn = std::make_unique<PooledConnection>();
            auto stream = std::make_unique<SslStream>(io_context_, ssl_context_);

            // Set SNI hostname
            if (!SSL_set_tlsext_host_name(stream->native_handle(), host_.c_str())) {
                return nullptr;
            }

            // Get cached endpoints
            auto endpoints = get_endpoints();

            // Connect with timeout
            auto& tcp_stream = beast::get_lowest_layer(*stream);
            tcp_stream.expires_after(config_.connect_timeout);
            tcp_stream.connect(endpoints);

            // Configure TCP socket for low latency
            auto& socket = tcp_stream.socket();
            if (config_.enable_tcp_nodelay) {
                socket.set_option(tcp::no_delay(true));
            }

            // Set TCP keepalive
            socket.set_option(boost::asio::socket_base::keep_alive(true));

            // SSL handshake
            stream->set_verify_callback(ssl::host_name_verification(host_));
            stream->handshake(ssl::stream_base::client);

            conn->set_stream(std::move(stream));
            return conn;

        } catch (const std::exception&) {
            return nullptr;
        }
    }

    void replace_connection(PooledConnection* failed_conn) {
        std::lock_guard lock(pool_mutex_);

        // Find and remove failed connection
        for (auto it = connections_.begin(); it != connections_.end(); ++it) {
            if (it->get() == failed_conn) {
                // Try to create replacement
                auto new_conn = create_connection();
                if (new_conn) {
                    *it = std::move(new_conn);
                } else {
                    connections_.erase(it);
                }
                break;
            }
        }
    }
};

/**
 * RAII guard for pooled connections
 */
class PooledConnectionGuard {
private:
    ConnectionPool& pool_;
    PooledConnection* conn_;
    bool failed_{false};

public:
    explicit PooledConnectionGuard(ConnectionPool& pool,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds(100))
        : pool_(pool)
        , conn_(pool.acquire(timeout)) {}

    ~PooledConnectionGuard() {
        if (conn_) {
            if (failed_) {
                pool_.mark_failed(conn_);
            } else {
                pool_.release(conn_);
            }
        }
    }

    // Non-copyable, non-movable
    PooledConnectionGuard(const PooledConnectionGuard&) = delete;
    PooledConnectionGuard& operator=(const PooledConnectionGuard&) = delete;

    PooledConnection* get() { return conn_; }
    PooledConnection* operator->() { return conn_; }
    explicit operator bool() const { return conn_ != nullptr; }

    void mark_failed() { failed_ = true; }
};

} // namespace kimp::network
