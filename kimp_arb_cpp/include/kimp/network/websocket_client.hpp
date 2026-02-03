#pragma once

#include "kimp/core/types.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/memory/ring_buffer.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

namespace kimp::network {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

// WebSocket message types
enum class MessageType {
    Text,
    Binary
};

// Connection state
enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed
};

// Callbacks
using MessageCallback = std::function<void(std::string_view message, MessageType type)>;
using ConnectCallback = std::function<void(bool success, const std::string& error)>;
using DisconnectCallback = std::function<void(const std::string& reason)>;
using ErrorCallback = std::function<void(const std::string& error)>;

/**
 * High-performance WebSocket client using Boost.Beast
 *
 * Features:
 * - SSL/TLS support
 * - Automatic reconnection
 * - Heartbeat/ping-pong
 * - Async message sending
 * - Thread-safe operation
 */
class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    using WebSocketStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

private:
    // Networking
    net::io_context& io_context_;
    ssl::context ssl_context_;
    std::unique_ptr<WebSocketStream> ws_;
    tcp::resolver resolver_;
    net::steady_timer reconnect_timer_;
    net::steady_timer ping_timer_;

    // Connection info
    std::string host_;
    std::string port_;
    std::string path_;
    std::string name_;  // For logging

    // State - cache-line aligned to prevent false sharing
    alignas(64) std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    alignas(64) std::atomic<bool> should_reconnect_{true};
    std::atomic<int> reconnect_attempts_{0};  // Same cache line as should_reconnect_ (both infrequent)
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr int RECONNECT_DELAY_MS = 1000;
    static constexpr int PING_INTERVAL_MS = 30000;

    // Buffers - cache-line aligned for write path
    beast::flat_buffer read_buffer_;
    // Lock-free write queue: MPMC for thread-safe send() from any thread
    alignas(64) memory::MPMCRingBuffer<std::string, 256> write_queue_;  // Power of 2 capacity
    alignas(64) std::atomic<bool> is_writing_{false};

    // Callbacks
    MessageCallback on_message_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
    ErrorCallback on_error_;

public:
    explicit WebSocketClient(net::io_context& ioc, std::string name = "WebSocket")
        : io_context_(ioc)
        , ssl_context_(ssl::context::tlsv12_client)
        , resolver_(net::make_strand(ioc))
        , reconnect_timer_(ioc)
        , ping_timer_(ioc)
        , name_(std::move(name)) {

        // Configure SSL context
        ssl_context_.set_default_verify_paths();
        ssl_context_.set_verify_mode(ssl::verify_peer);
    }

    ~WebSocketClient() {
        disconnect();
    }

    // Non-copyable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Set callbacks
    void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_connect_callback(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { on_disconnect_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }

    // Connection management
    void connect(const std::string& url);
    void disconnect();
    void reconnect();

    // Send message
    void send(std::string message);
    void send(std::string_view message);

    // State
    ConnectionState state() const noexcept { return state_.load(); }
    bool is_connected() const noexcept { return state_.load() == ConnectionState::Connected; }
    const std::string& name() const noexcept { return name_; }

    // Disable auto-reconnect
    void disable_reconnect() { should_reconnect_ = false; }

private:
    // Parse URL into host, port, path
    bool parse_url(const std::string& url);

    // Async operations
    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void do_close();
    void on_close(beast::error_code ec);

    // Reconnection
    void schedule_reconnect();
    void on_reconnect_timer(beast::error_code ec);

    // Ping/pong
    void schedule_ping();
    void on_ping_timer(beast::error_code ec);

    // Error handling
    void handle_error(const std::string& operation, beast::error_code ec);
    void notify_error(const std::string& error);
};

/**
 * WebSocket connection pool for managing multiple connections
 */
class WebSocketPool {
private:
    net::io_context& io_context_;
    std::vector<std::shared_ptr<WebSocketClient>> clients_;
    std::mutex mutex_;

public:
    explicit WebSocketPool(net::io_context& ioc) : io_context_(ioc) {}

    std::shared_ptr<WebSocketClient> create(const std::string& name) {
        auto client = std::make_shared<WebSocketClient>(io_context_, name);
        std::lock_guard lock(mutex_);
        clients_.push_back(client);
        return client;
    }

    void disconnect_all() {
        std::lock_guard lock(mutex_);
        for (auto& client : clients_) {
            client->disconnect();
        }
    }

    std::size_t size() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return clients_.size();
    }
};

} // namespace kimp::network
