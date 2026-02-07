#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <array>
#include <optional>
#include <functional>

namespace kimp {

// High-precision timestamps
using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
using SystemTimestamp = std::chrono::time_point<std::chrono::system_clock>;

// Price types
using Price = double;
using Quantity = double;

// Exchange identifiers
enum class Exchange : uint8_t {
    Upbit = 0,
    Bithumb = 1,
    Bybit = 2,
    GateIO = 3,
    Count = 4
};

inline constexpr const char* exchange_name(Exchange ex) noexcept {
    switch (ex) {
        case Exchange::Upbit: return "Upbit";
        case Exchange::Bithumb: return "Bithumb";
        case Exchange::Bybit: return "Bybit";
        case Exchange::GateIO: return "GateIO";
        default: return "Unknown";
    }
}

inline constexpr bool is_korean_exchange(Exchange ex) noexcept {
    return ex == Exchange::Upbit || ex == Exchange::Bithumb;
}

inline constexpr bool is_foreign_exchange(Exchange ex) noexcept {
    return ex == Exchange::Bybit || ex == Exchange::GateIO;
}

// Market type
enum class MarketType : uint8_t {
    Spot = 0,
    Futures = 1,
    Perpetual = 2
};

// Order side
enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

inline constexpr const char* side_name(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
}

// Order type
enum class OrderType : uint8_t {
    Market = 0,
    Limit = 1
};

// Order status
enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
    Expired = 5
};

// Symbol identifier (fixed-size for cache efficiency)
struct SymbolId {
    std::array<char, 12> base{};   // e.g., "BTC", "ETH"
    std::array<char, 8> quote{};   // e.g., "KRW", "USDT"

    SymbolId() = default;

    SymbolId(std::string_view b, std::string_view q) {
        set_base(b);
        set_quote(q);
    }

    void set_base(std::string_view b) noexcept {
        base.fill(0);
        std::copy_n(b.begin(), std::min(b.size(), base.size() - 1), base.begin());
    }

    void set_quote(std::string_view q) noexcept {
        quote.fill(0);
        std::copy_n(q.begin(), std::min(q.size(), quote.size() - 1), quote.begin());
    }

    std::string_view get_base() const noexcept {
        return std::string_view(base.data());
    }

    std::string_view get_quote() const noexcept {
        return std::string_view(quote.data());
    }

    std::string to_string() const {
        return std::string(get_base()) + "/" + std::string(get_quote());
    }

    // Korean format: "KRW-BTC" for Upbit, "BTC_KRW" for Bithumb
    std::string to_upbit_format() const {
        return std::string(get_quote()) + "-" + std::string(get_base());
    }

    std::string to_bithumb_format() const {
        return std::string(get_base()) + "_" + std::string(get_quote());
    }

    // Foreign format: "BTCUSDT" for Bybit, "BTC_USDT" for Gate.io futures
    std::string to_bybit_format() const {
        return std::string(get_base()) + std::string(get_quote());
    }

    std::string to_gateio_futures_format() const {
        return std::string(get_base()) + "_" + std::string(get_quote());
    }

    bool operator==(const SymbolId& other) const noexcept {
        return base == other.base && quote == other.quote;
    }

    bool operator!=(const SymbolId& other) const noexcept {
        return !(*this == other);
    }

    std::size_t hash() const noexcept {
        std::size_t h = 0;
        for (char c : base) h = h * 31 + static_cast<unsigned char>(c);
        for (char c : quote) h = h * 31 + static_cast<unsigned char>(c);
        return h;
    }

    // Fast check for USDT/KRW (hot path optimization)
    // Checks first chars only - much faster than full string comparison
    bool is_usdt_krw() const noexcept {
        return base[0] == 'U' && base[1] == 'S' && base[2] == 'D' && base[3] == 'T' &&
               quote[0] == 'K' && quote[1] == 'R' && quote[2] == 'W';
    }
};

} // namespace kimp

// Hash specialization for SymbolId
namespace std {
template<>
struct hash<kimp::SymbolId> {
    std::size_t operator()(const kimp::SymbolId& s) const noexcept {
        return s.hash();
    }
};
} // namespace std

namespace kimp {

// Ticker data
struct alignas(64) Ticker {
    Exchange exchange{};
    SymbolId symbol;
    Timestamp timestamp{};
    uint64_t sequence{0};

    Price last{0.0};
    Price bid{0.0};
    Price ask{0.0};
    Price high_24h{0.0};
    Price low_24h{0.0};
    Quantity volume_24h{0.0};

    // Funding rate info (futures only)
    double funding_rate{0.0};       // Current funding rate (e.g., 0.0001 = 0.01%)
    int funding_interval_hours{8};   // Funding interval in hours (default 8h for most exchanges)
    uint64_t next_funding_time{0};   // Unix timestamp ms of next funding

    Price mid_price() const noexcept { return (bid + ask) / 2.0; }
    Price spread() const noexcept { return ask - bid; }
};

// Order book level
struct OrderBookLevel {
    Price price{0.0};
    Quantity quantity{0.0};
};

// Order book (top N levels)
struct alignas(64) OrderBook {
    static constexpr std::size_t MAX_LEVELS = 10;

    Exchange exchange{};
    SymbolId symbol;
    Timestamp timestamp{};
    uint64_t sequence{0};

    std::array<OrderBookLevel, MAX_LEVELS> bids{};
    std::array<OrderBookLevel, MAX_LEVELS> asks{};
    uint8_t bid_count{0};
    uint8_t ask_count{0};

    Price best_bid() const noexcept { return bid_count > 0 ? bids[0].price : 0.0; }
    Price best_ask() const noexcept { return ask_count > 0 ? asks[0].price : 0.0; }
    Quantity best_bid_qty() const noexcept { return bid_count > 0 ? bids[0].quantity : 0.0; }
    Quantity best_ask_qty() const noexcept { return ask_count > 0 ? asks[0].quantity : 0.0; }
    Price mid_price() const noexcept { return (best_bid() + best_ask()) / 2.0; }
    Price spread() const noexcept { return best_ask() - best_bid(); }
};

// Position state
struct Position {
    SymbolId symbol;
    Exchange korean_exchange{};
    Exchange foreign_exchange{};

    // Entry info
    SystemTimestamp entry_time{};
    double entry_premium{0.0};
    Price korean_entry_price{0.0};  // KRW
    Price foreign_entry_price{0.0}; // USD

    // Position sizes
    double position_size_usd{0.0};
    Quantity korean_amount{0.0};    // Coins held on Korean exchange
    Quantity foreign_amount{0.0};   // Contracts/coins shorted on foreign exchange

    // Status
    bool is_active{false};

    // Exit info (filled on close)
    SystemTimestamp exit_time{};
    double exit_premium{0.0};
    Price korean_exit_price{0.0};
    Price foreign_exit_price{0.0};
    double realized_pnl_krw{0.0};   // Total P&L in KRW
};

// Order representation
struct Order {
    uint64_t client_order_id{0};
    uint64_t exchange_order_id{0};
    Exchange exchange{};
    SymbolId symbol;
    Side side{};
    OrderType type{};
    OrderStatus status{};

    Price price{0.0};
    Quantity quantity{0.0};
    Quantity filled_quantity{0.0};
    Price average_price{0.0};

    SystemTimestamp create_time{};
    SystemTimestamp update_time{};

    bool is_complete() const noexcept {
        return status == OrderStatus::Filled ||
               status == OrderStatus::Cancelled ||
               status == OrderStatus::Rejected ||
               status == OrderStatus::Expired;
    }
};

// Arbitrage signal
struct ArbitrageSignal {
    SymbolId symbol;
    Exchange korean_exchange{};
    Exchange foreign_exchange{};

    double premium{0.0};
    Price korean_ask{0.0};    // Buy price on Korean
    Price foreign_bid{0.0};   // Short price on foreign
    double funding_rate{0.0};
    Price usdt_krw_rate{0.0};

    Timestamp timestamp{};
};

// Exit signal
struct ExitSignal {
    SymbolId symbol;
    Exchange korean_exchange{};
    Exchange foreign_exchange{};

    double premium{0.0};
    Price korean_bid{0.0};    // Sell price on Korean
    Price foreign_ask{0.0};   // Cover price on foreign
    Price usdt_krw_rate{0.0};

    Timestamp timestamp{};
};

// Trading configuration (compile-time constants for HFT)
struct TradingConfig {
    // ==========================================================================
    // 병렬 포지션 관리: 조건 만족하는 모든 코인에 동시 진입, 개별 청산
    // ==========================================================================
    static inline int MAX_POSITIONS = 1;                       // 최대 동시 포지션 수 (런타임 설정, 1~4)
    static constexpr double POSITION_SIZE_USD = 250.0;        // $250 per side (총 $500 per position)
    static constexpr double ORDER_SIZE_USD = 25.0;            // $25 per split
    static constexpr int SPLIT_ORDERS = 10;                   // 250 / 25 = 10 splits
    static constexpr int ORDER_INTERVAL_MS = 1000;            // 1 second between splits

    // Entry threshold
    static constexpr double ENTRY_PREMIUM_THRESHOLD = -0.99;  // Entry when premium <= -0.99%

    // Fee structure (bid/ask spread already in premium calc, slippage ~0 at $25 splits)
    static constexpr double BITHUMB_FEE_PCT = 0.04;           // Per trade (buy or sell)
    static constexpr double BYBIT_FEE_PCT = 0.055;            // Per trade (short or cover)
    static constexpr double ROUND_TRIP_FEE_PCT = (BITHUMB_FEE_PCT + BYBIT_FEE_PCT) * 2;  // 0.19%
    static constexpr double MIN_NET_PROFIT_PCT = 0.60;         // 순수익 목표 0.6%
    static constexpr double DYNAMIC_EXIT_SPREAD = ROUND_TRIP_FEE_PCT + MIN_NET_PROFIT_PCT; // 0.79%
    // Dynamic exit: exit_pm >= max(entry_pm + 0.79%, EXIT_PREMIUM_THRESHOLD)
    // e.g., entry -0.99% -> dynamic -0.20%, but floor applies -> exit >= +0.10%
    // e.g., entry -0.30% -> dynamic +0.49% -> exit >= +0.49%

    // Fallback fixed exit threshold (used when no position entry_premium available)
    static constexpr double EXIT_PREMIUM_THRESHOLD = 0.10;    // Exit floor: only when premium >= +0.10%

    // Entry filters
    static constexpr int MIN_FUNDING_INTERVAL_HOURS = 8;      // Only 8h funding coins
    static constexpr bool REQUIRE_POSITIVE_FUNDING = true;    // Only positive funding rate

    static constexpr double MAX_PRICE_DIFF_PERCENT = 50.0;
    static constexpr int USDT_UPDATE_INTERVAL_MS = 180000;    // 3 minutes
    static constexpr double MIN_ORDER_KRW = 5000.0;           // Minimum order in KRW

    // Quote quality guards (outlier/stale protection)
    static constexpr uint64_t MAX_QUOTE_AGE_MS = 2500;        // Reject quotes older than 2.5s
    static constexpr uint64_t MAX_QUOTE_DESYNC_MS = 1200;     // Reject if KR/Foreign quote times differ too much
    static constexpr double MAX_KOREAN_SPREAD_PCT = 1.20;     // Skip illiquid KRW books
    static constexpr double MAX_FOREIGN_SPREAD_PCT = 0.40;    // Skip illiquid futures books
    static constexpr double MAX_USDT_JUMP_PCT = 1.50;         // Filter abnormal USDT/KRW jumps
    static constexpr uint64_t USDT_FULL_SCAN_DEBOUNCE_MS = 250;  // Coalesce bursty USDT updates
};

// Callback types
using TickerCallback = std::function<void(const Ticker&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;
using OrderCallback = std::function<void(const Order&)>;
using SignalCallback = std::function<void(const ArbitrageSignal&)>;

} // namespace kimp
