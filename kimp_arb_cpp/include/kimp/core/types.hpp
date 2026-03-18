#pragma once

#include <cstdint>
#include <cstring>
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
    Bithumb = 0,
    Bybit = 1,
    OKX = 2,
    Upbit = 3,
    Count = 4
};

inline constexpr const char* exchange_name(Exchange ex) noexcept {
    switch (ex) {
        case Exchange::Bithumb: return "Bithumb";
        case Exchange::Bybit: return "Bybit";
        case Exchange::OKX: return "OKX";
        case Exchange::Upbit: return "Upbit";
        default: return "Unknown";
    }
}

inline constexpr bool is_korean_exchange(Exchange ex) noexcept {
    return ex == Exchange::Bithumb || ex == Exchange::Upbit;
}

inline constexpr bool is_foreign_exchange(Exchange ex) noexcept {
    return ex == Exchange::Bybit || ex == Exchange::OKX;
}

// Market type
enum class MarketType : uint8_t {
    Spot = 0,
    MarginSpot = 1
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

    // Korean format: "BTC_KRW" for Bithumb
    std::string to_bithumb_format() const {
        return std::string(get_base()) + "_" + std::string(get_quote());
    }

    // Parse "BTC_KRW" → SymbolId (zero-heap-alloc)
    static SymbolId from_bithumb_format(std::string_view sv) noexcept {
        SymbolId id;
        auto pos = sv.find('_');
        if (pos != std::string_view::npos) {
            id.set_base(sv.substr(0, pos));
            id.set_quote(sv.substr(pos + 1));
        }
        return id;
    }

    // Upbit format: "KRW-BTC" (quote-base)
    std::string to_upbit_format() const {
        return std::string(get_quote()) + "-" + std::string(get_base());
    }

    // Parse "KRW-BTC" → SymbolId (zero-heap-alloc)
    static SymbolId from_upbit_format(std::string_view sv) noexcept {
        SymbolId id;
        auto pos = sv.find('-');
        if (pos != std::string_view::npos) {
            id.set_quote(sv.substr(0, pos));   // KRW
            id.set_base(sv.substr(pos + 1));   // BTC
        }
        return id;
    }

    // Foreign format: "BTCUSDT" for Bybit spot margin
    std::string to_bybit_format() const {
        return std::string(get_base()) + std::string(get_quote());
    }

    bool operator==(const SymbolId& other) const noexcept {
        return base == other.base && quote == other.quote;
    }

    bool operator!=(const SymbolId& other) const noexcept {
        return !(*this == other);
    }

    std::size_t hash() const noexcept {
        // Load 20 bytes as integers for proper avalanche (no null-byte weakness)
        uint64_t h1, h2;
        uint32_t h3;
        std::memcpy(&h1, base.data(), 8);
        std::memcpy(&h3, base.data() + 8, 4);
        std::memcpy(&h2, quote.data(), 8);
        // Splitmix64-style finalizer for good distribution in power-of-2 tables
        h1 ^= static_cast<uint64_t>(h3) * 0x9E3779B97F4A7C15ULL;
        h1 ^= h2 * 0xBF58476D1CE4E5B9ULL;
        h1 ^= h1 >> 31;
        h1 *= 0xBF58476D1CE4E5B9ULL;
        h1 ^= h1 >> 27;
        return static_cast<std::size_t>(h1);
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
    Quantity bid_qty{0.0};
    Quantity ask_qty{0.0};
    Price high_24h{0.0};
    Price low_24h{0.0};
    Quantity volume_24h{0.0};

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

    std::string order_id_str;  // Exchange-native order ID for async fill queries

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
    uint64_t trace_id{0};
    uint64_t trace_start_ns{0};
    std::array<char, 24> trace_symbol{};
    SymbolId symbol;
    Exchange korean_exchange{};
    Exchange foreign_exchange{};

    double premium{0.0};
    Price korean_ask{0.0};    // Buy price on Korean
    Quantity korean_ask_qty{0.0};
    Price foreign_bid{0.0};   // Short price on foreign
    Quantity foreign_bid_qty{0.0};
    Quantity match_qty{0.0};
    Quantity target_coin_qty{0.0};
    double max_tradable_usdt_at_best{0.0};
    double gross_edge_pct{0.0};
    double net_edge_pct{0.0};
    double net_profit_krw{0.0};
    bool both_can_fill_target{false};
    Price usdt_krw_rate{0.0};

    Timestamp timestamp{};
};

// Exit signal
struct ExitSignal {
    uint64_t trace_id{0};
    uint64_t trace_start_ns{0};
    std::array<char, 24> trace_symbol{};
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
    static constexpr double CAPITAL_PER_EXCHANGE_USD = 3000.0; // Risk budget per venue (Bithumb $3000, Bybit $3000)
    static constexpr double TOTAL_CAPITAL_USD = CAPITAL_PER_EXCHANGE_USD * 2.0;
    static constexpr double TARGET_ENTRY_USDT = 35.0;         // Per-check entry unit: exact 35 USDT notional per add
    static constexpr double POSITION_SIZE_USD = CAPITAL_PER_EXCHANGE_USD; // Max side exposure per symbol / per venue
    static constexpr double ORDER_SIZE_USD = TARGET_ENTRY_USDT;           // Each submit adds 35 USDT
    static constexpr int SPLIT_ORDERS = 1;                    // Each cycle submits one 35 USDT order, then re-checks
    static constexpr int ORDER_INTERVAL_MS = 100;             // Re-check quickly on next market update / retry

    // Entry threshold
    static constexpr double ENTRY_PREMIUM_THRESHOLD = 0.0;    // Legacy/monitor alias only
    static constexpr double MIN_NET_EDGE_PCT = 0.0;           // Enter only when net edge is positive
    static constexpr double MIN_ENTRY_NET_PROFIT_KRW = 300.0; // Enter only when projected NetKRW for the executable entry size is >= 300

    // Fee structure for the relay model (per-exchange taker rates)
    static constexpr int KOREAN_FEE_EVENTS = 1;               // Korean side: 1 trade (buy or sell)
    static constexpr int FOREIGN_FEE_EVENTS = 3;              // Foreign side: 3 trades (short + cover + transfer)
    static constexpr int BITHUMB_FEE_EVENTS = KOREAN_FEE_EVENTS;   // Legacy alias
    static constexpr int BYBIT_FEE_EVENTS = FOREIGN_FEE_EVENTS;    // Legacy alias
    static constexpr double BITHUMB_FEE_RATE = 0.0004;        // 0.04% (쿠폰 적용 taker)
    static constexpr double UPBIT_FEE_RATE   = 0.0005;        // 0.05% (KRW마켓 taker)
    static constexpr double BYBIT_FEE_RATE   = 0.0010;        // 0.10% (VIP0 taker)
    static constexpr double OKX_FEE_RATE     = 0.0010;        // 0.10% (Regular taker)

    static constexpr double get_korean_fee_rate(Exchange ex) noexcept {
        return (ex == Exchange::Upbit) ? UPBIT_FEE_RATE : BITHUMB_FEE_RATE;
    }
    static constexpr double get_foreign_fee_rate(Exchange ex) noexcept {
        return (ex == Exchange::OKX) ? OKX_FEE_RATE : BYBIT_FEE_RATE;
    }

    static constexpr double BITHUMB_FEE_PCT = BITHUMB_FEE_RATE * 100.0;
    static constexpr double BYBIT_FEE_PCT = BYBIT_FEE_RATE * 100.0;
    static constexpr double ENTRY_TOTAL_FEE_PCT =
        (BITHUMB_FEE_PCT * KOREAN_FEE_EVENTS) + (BYBIT_FEE_PCT * FOREIGN_FEE_EVENTS);  // 0.34% (Bi-By worst case)
    static constexpr double ROUND_TRIP_FEE_PCT = ENTRY_TOTAL_FEE_PCT;  // Legacy alias for older diagnostics/tests
    static constexpr double MIN_NET_PROFIT_PCT = 0.00;
    static constexpr double DYNAMIC_EXIT_SPREAD = ENTRY_TOTAL_FEE_PCT + MIN_NET_PROFIT_PCT;

    static constexpr bool meets_entry_profit_floor(double net_profit_krw) noexcept {
        return net_profit_krw >= MIN_ENTRY_NET_PROFIT_KRW;
    }

    static constexpr bool entry_gate_passes(bool both_can_fill_target,
                                            double match_qty,
                                            double net_edge_pct,
                                            double net_profit_krw) noexcept {
        return both_can_fill_target &&
               match_qty > 0.0 &&
               net_edge_pct > MIN_NET_EDGE_PCT &&
               meets_entry_profit_floor(net_profit_krw);
    }

    // Fallback fixed exit threshold (used when no position entry_premium available)
    static constexpr double EXIT_PREMIUM_THRESHOLD = 0.25;    // Exit floor: premium >= +0.25%

    static constexpr double MAX_PRICE_DIFF_PERCENT = 50.0;
    static constexpr double MIN_ORDER_KRW = 5000.0;           // Minimum order in KRW

    // Quote quality guards — ENTRY (strict: avoid bad fills on new positions)
    static constexpr uint64_t MAX_QUOTE_AGE_MS = 700;         // Require sub-second-ish quote freshness on entry
    static constexpr uint64_t MAX_QUOTE_DESYNC_MS = 350;      // Reject KR/Foreign quotes that are materially out of sync
    static constexpr double MAX_KOREAN_SPREAD_PCT = 1.20;     // Skip illiquid KRW books
    static constexpr double MAX_FOREIGN_SPREAD_PCT = 0.40;    // Skip illiquid Bybit spot books

    // Quote quality guards — EXIT (relaxed: must be able to close positions)
    static constexpr uint64_t MAX_QUOTE_AGE_MS_EXIT = 8000;       // 8s (small coins tick less often)
    static constexpr uint64_t MAX_QUOTE_DESYNC_MS_EXIT = 5000;    // 5s desync tolerance
    static constexpr double MAX_KOREAN_SPREAD_PCT_EXIT = 3.50;    // 3.5% (spread already in bid/ask premium)
    static constexpr double MAX_FOREIGN_SPREAD_PCT_EXIT = 1.50;   // 1.5%
    static constexpr double MAX_USDT_JUMP_PCT = 1.50;         // Filter abnormal USDT/KRW jumps
    static constexpr uint64_t USDT_FULL_SCAN_DEBOUNCE_MS = 40;   // Keep FX refresh reactive without excessive rescans
    static constexpr uint64_t ENTRY_FAST_SCAN_COOLDOWN_MS = 20;  // Faster event-driven entry rescans on fresh ticks
    static constexpr uint64_t ENTRY_STALL_TIMEOUT_MS = 120000;   // 2min: finalize partial position if no split progress
};

// Callback types
using TickerCallback = std::function<void(const Ticker&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;
using OrderCallback = std::function<void(const Order&)>;
using SignalCallback = std::function<void(const ArbitrageSignal&)>;

} // namespace kimp
