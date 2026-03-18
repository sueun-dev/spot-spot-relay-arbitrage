#include "kimp/exchange/upbit/upbit.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/utils/crypto.hpp"

#include <zlib.h>
#include <simdjson.h>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
#include <thread>
#include <unordered_set>

namespace kimp::exchange::upbit {

namespace {

// Fast extraction of a quoted JSON string value after a marker.
// Example: marker = R"("code":")", source = ...,"code":"KRW-BTC",...
// Returns the value between the quotes after the marker.
bool extract_quoted(std::string_view source, std::string_view marker,
                    size_t from, std::string_view& out, size_t& end) {
    auto pos = source.find(marker, from);
    if (pos == std::string_view::npos) return false;
    size_t start = pos + marker.size();
    size_t close = source.find('"', start);
    if (close == std::string_view::npos) return false;
    out = source.substr(start, close - start);
    end = close;
    return true;
}

// Fast extraction of a numeric JSON value (unquoted).
// Example: marker = R"("trade_price":)", value = 137500000.0
bool extract_number(std::string_view source, std::string_view marker,
                    size_t from, double& out, size_t& end) {
    auto pos = source.find(marker, from);
    if (pos == std::string_view::npos) return false;
    size_t start = pos + marker.size();
    // Find end of number (comma, }, or ])
    size_t i = start;
    while (i < source.size() && source[i] != ',' && source[i] != '}' && source[i] != ']') ++i;
    if (i == start) return false;
    out = opt::fast_stod(source.substr(start, i - start));
    end = i;
    return true;
}

std::string format_upbit_number(double value, int precision = 16) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    std::string out = oss.str();
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    if (out.empty()) {
        return "0";
    }
    return out;
}

double parse_dom_double(simdjson::dom::element elem, double fallback = 0.0) {
    if (elem.is_null()) {
        return fallback;
    }
    auto dbl = elem.get_double();
    if (!dbl.error()) {
        return dbl.value();
    }
    auto int64_val = elem.get_int64();
    if (!int64_val.error()) {
        return static_cast<double>(int64_val.value());
    }
    auto uint64_val = elem.get_uint64();
    if (!uint64_val.error()) {
        return static_cast<double>(uint64_val.value());
    }
    auto str = elem.get_string();
    if (!str.error()) {
        return opt::fast_stod(str.value());
    }
    return fallback;
}

std::string parse_dom_string(simdjson::dom::element elem) {
    auto str = elem.get_string();
    if (!str.error()) {
        return std::string(str.value());
    }
    return {};
}

OrderStatus parse_upbit_order_status(std::string_view state,
                                     double executed_volume,
                                     double remaining_volume) {
    if (state == "done") {
        return OrderStatus::Filled;
    }
    if (state == "cancel") {
        return executed_volume > 0.0 ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
    }
    if (state == "wait" || state == "watch") {
        if (executed_volume > 0.0 && remaining_volume > 0.0) {
            return OrderStatus::PartiallyFilled;
        }
        return OrderStatus::New;
    }
    return OrderStatus::Rejected;
}

bool populate_upbit_order_from_body(const std::string& body, Order& order) {
    try {
        simdjson::dom::parser parser;
        auto doc = parser.parse(body);

        std::string uuid = parse_dom_string(doc["uuid"]);
        if (uuid.empty()) {
            return false;
        }

        const double executed_volume = parse_dom_double(doc["executed_volume"]);
        const double remaining_volume = parse_dom_double(doc["remaining_volume"]);
        double total_trade_qty = 0.0;
        double total_trade_value = 0.0;

        auto trades = doc["trades"].get_array();
        if (!trades.error()) {
            for (auto trade : trades.value()) {
                const double trade_price = parse_dom_double(trade["price"]);
                const double trade_volume = parse_dom_double(trade["volume"]);
                if (trade_price > 0.0 && trade_volume > 0.0) {
                    total_trade_qty += trade_volume;
                    total_trade_value += trade_price * trade_volume;
                }
            }
        }

        order.exchange = Exchange::Upbit;
        order.order_id_str = uuid;
        order.exchange_order_id = std::hash<std::string>{}(uuid);
        order.filled_quantity = executed_volume > 0.0 ? executed_volume : total_trade_qty;
        if (total_trade_qty > 0.0 && total_trade_value > 0.0) {
            order.average_price = total_trade_value / total_trade_qty;
        }

        const std::string state = parse_dom_string(doc["state"]);
        order.status = parse_upbit_order_status(state, executed_volume, remaining_volume);

        const std::string side = parse_dom_string(doc["side"]);
        if (side == "bid") {
            order.side = Side::Buy;
        } else if (side == "ask") {
            order.side = Side::Sell;
        }

        const std::string ord_type = parse_dom_string(doc["ord_type"]);
        if (ord_type == "limit") {
            order.type = OrderType::Limit;
        } else {
            order.type = OrderType::Market;
        }

        const double order_price = parse_dom_double(doc["price"]);
        if (order_price > 0.0) {
            order.price = order_price;
        }
        const double order_volume = parse_dom_double(doc["volume"]);
        if (order_volume > 0.0) {
            order.quantity = order_volume;
        }

        return true;
    } catch (const simdjson::simdjson_error&) {
        return false;
    }
}

} // namespace

bool UpbitExchange::decompress_gzip(const char* data, size_t len, std::string& output) {
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    strm.avail_in = static_cast<uInt>(len);

    // 15 + 16 = gzip decoding
    if (inflateInit2(&strm, 15 + 16) != Z_OK) {
        return false;
    }

    output.clear();
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(decompress_buf_.data());
        strm.avail_out = static_cast<uInt>(decompress_buf_.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        size_t produced = decompress_buf_.size() - strm.avail_out;
        output.append(decompress_buf_.data(), produced);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return true;
}

bool UpbitExchange::connect() {
    Logger::info("[Upbit] Connecting...");

    if (!initialize_rest()) {
        Logger::error("[Upbit] Failed to initialize REST client");
        return false;
    }

    // Create WebSocket client
    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "Upbit-WS");

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType /*type*/) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Upbit] WebSocket connect failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& /*reason*/) {
        on_ws_disconnected();
    });

    const std::string ws_url = credentials_.ws_endpoint.empty() ? endpoints::UPBIT_WS : credentials_.ws_endpoint;
    ws_client_->connect(ws_url);
    return true;
}

void UpbitExchange::disconnect() {
    Logger::info("[Upbit] Disconnecting...");
    if (ws_client_) {
        ws_client_->disconnect();
    }
    shutdown_rest();
    connected_.store(false);
}

void UpbitExchange::on_ws_connected() {
    Logger::info("[Upbit] WebSocket connected");
    connected_.store(true);

    // Re-subscribe if we have stored subscriptions
    std::lock_guard lock(subscription_mutex_);
    if (!subscribed_orderbooks_.empty()) {
        subscribe_orderbook(subscribed_orderbooks_);
    } else if (!subscribed_tickers_.empty()) {
        subscribe_ticker(subscribed_tickers_);
    }
}

void UpbitExchange::on_ws_disconnected() {
    Logger::warn("[Upbit] WebSocket disconnected");
    connected_.store(false);
}

void UpbitExchange::on_ws_message(std::string_view message) {
    // Upbit sends gzip-compressed binary frames.
    // First try to decompress; if it fails, treat as plaintext JSON.
    std::string decompressed;
    std::string_view json_view;

    if (!message.empty() && (static_cast<uint8_t>(message[0]) == 0x1f)) {
        // Gzip magic number detected
        if (!decompress_gzip(message.data(), message.size(), decompressed)) {
            return;
        }
        json_view = decompressed;
    } else {
        json_view = message;
    }

    // Route based on message type
    if (json_view.find(R"("type":"orderbook")") != std::string_view::npos) {
        parse_orderbook_message(json_view);
    } else if (json_view.find(R"("type":"ticker")") != std::string_view::npos) {
        parse_ticker_message(json_view);
    }
}

bool UpbitExchange::parse_orderbook_message(std::string_view message) {
    // Extract code (e.g., "KRW-BTC")
    static constexpr std::string_view code_marker = R"("code":")";
    std::string_view code;
    size_t pos = 0;
    if (!extract_quoted(message, code_marker, 0, code, pos)) return false;

    // Parse "KRW-BTC" → base="BTC", quote="KRW"
    auto dash = code.find('-');
    if (dash == std::string_view::npos) return false;
    std::string_view quote_sv = code.substr(0, dash);
    std::string_view base_sv = code.substr(dash + 1);
    if (quote_sv != "KRW") return false;

    SymbolId symbol(base_sv, "KRW");

    // Extract first orderbook unit's bid/ask prices and sizes
    // "orderbook_units":[{"ask_price":137002000,"bid_price":137001000,"ask_size":0.106,"bid_size":0.036},...]
    static constexpr std::string_view ask_price_marker = R"("ask_price":)";
    static constexpr std::string_view bid_price_marker = R"("bid_price":)";
    static constexpr std::string_view ask_size_marker = R"("ask_size":)";
    static constexpr std::string_view bid_size_marker = R"("bid_size":)";

    double ask_price = 0.0, bid_price = 0.0, ask_size = 0.0, bid_size = 0.0;
    size_t end = 0;

    if (!extract_number(message, ask_price_marker, pos, ask_price, end)) return false;
    if (!extract_number(message, bid_price_marker, pos, bid_price, end)) return false;
    if (!extract_number(message, ask_size_marker, pos, ask_size, end)) return false;
    if (!extract_number(message, bid_size_marker, pos, bid_size, end)) return false;

    if (ask_price <= 0 || bid_price <= 0) return false;

    // Update BBO cache (lock-free)
    auto it = orderbook_bbo_.find(symbol);
    if (it != orderbook_bbo_.end()) {
        it->second.best_bid.store(bid_price, std::memory_order_relaxed);
        it->second.best_ask.store(ask_price, std::memory_order_relaxed);
        it->second.best_bid_qty.store(bid_size, std::memory_order_relaxed);
        it->second.best_ask_qty.store(ask_size, std::memory_order_relaxed);
    }

    // Check if this is USDT/KRW
    if (base_sv == "USDT") {
        double mid = (bid_price + ask_price) * 0.5;
        usdt_krw_price_.store(mid, std::memory_order_release);
    }

    // Get last trade price for the ticker
    double last = 0.0;
    {
        std::lock_guard lk(last_price_mutex_);
        auto pit = last_price_cache_.find(symbol);
        if (pit != last_price_cache_.end()) {
            last = pit->second;
        }
    }

    // Dispatch ticker with real bid/ask from orderbook
    Ticker ticker;
    ticker.exchange = Exchange::Upbit;
    ticker.symbol = symbol;
    ticker.timestamp = std::chrono::steady_clock::now();
    ticker.bid = bid_price;
    ticker.ask = ask_price;
    ticker.bid_qty = bid_size;
    ticker.ask_qty = ask_size;
    ticker.last = last > 0.0 ? last : (bid_price + ask_price) * 0.5;
    dispatch_ticker(ticker);

    return true;
}

bool UpbitExchange::parse_ticker_message(std::string_view message) {
    static constexpr std::string_view code_marker = R"("code":")";
    static constexpr std::string_view trade_price_marker = R"("trade_price":)";

    std::string_view code;
    size_t pos = 0;
    if (!extract_quoted(message, code_marker, 0, code, pos)) return false;

    auto dash = code.find('-');
    if (dash == std::string_view::npos) return false;
    std::string_view quote_sv = code.substr(0, dash);
    std::string_view base_sv = code.substr(dash + 1);
    if (quote_sv != "KRW") return false;

    double trade_price = 0.0;
    size_t end = 0;
    if (!extract_number(message, trade_price_marker, pos, trade_price, end)) return false;
    if (trade_price <= 0) return false;

    SymbolId symbol(base_sv, "KRW");

    // Cache last trade price
    {
        std::lock_guard lk(last_price_mutex_);
        last_price_cache_[symbol] = trade_price;
    }

    // If USDT/KRW, update the rate
    if (base_sv == "USDT") {
        usdt_krw_price_.store(trade_price, std::memory_order_release);
    }

    // Only dispatch full ticker if we have BBO data
    auto it = orderbook_bbo_.find(symbol);
    if (it != orderbook_bbo_.end()) {
        double bid = it->second.best_bid.load(std::memory_order_relaxed);
        double ask = it->second.best_ask.load(std::memory_order_relaxed);
        if (bid > 0 && ask > 0) {
            Ticker ticker;
            ticker.exchange = Exchange::Upbit;
            ticker.symbol = symbol;
            ticker.timestamp = std::chrono::steady_clock::now();
            ticker.bid = bid;
            ticker.ask = ask;
            ticker.bid_qty = it->second.best_bid_qty.load(std::memory_order_relaxed);
            ticker.ask_qty = it->second.best_ask_qty.load(std::memory_order_relaxed);
            ticker.last = trade_price;
            dispatch_ticker(ticker);
        }
    }

    return true;
}

std::vector<SymbolId> UpbitExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/v1/market/all");
    if (!response.success) {
        Logger::error("[Upbit] Failed to fetch markets: {}", response.error);
        return symbols;
    }

    try {
        simdjson::dom::parser parser;
        auto doc = parser.parse(response.body);

        for (auto item : doc.get_array()) {
            std::string_view market = item["market"].get_string().value();
            // Only KRW markets
            if (market.substr(0, 4) != "KRW-") continue;
            std::string_view base = market.substr(4);
            symbols.emplace_back(std::string(base), "KRW");
        }

        Logger::info("[Upbit] Found {} KRW markets", symbols.size());
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse markets: {}", e.what());
    }

    return symbols;
}

std::vector<Ticker> UpbitExchange::fetch_all_tickers() {
    // Not needed for monitor-only mode — orderbook WS provides real-time data
    return {};
}

double UpbitExchange::get_usdt_krw_price() {
    return usdt_krw_price_.load(std::memory_order_acquire);
}

void UpbitExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_tickers_ = symbols;
    }

    if (!ws_client_ || !connected_.load()) return;

    // Build Upbit subscription JSON array
    // [{"ticket":"uuid"},{"type":"ticker","codes":["KRW-BTC","KRW-ETH"],"isOnlyRealtime":true}]
    std::string codes;
    for (const auto& s : symbols) {
        if (!codes.empty()) codes += ",";
        codes += "\"" + symbol_to_upbit(s) + "\"";
    }

    std::string sub_msg = R"([{"ticket":"kimp-upbit-ticker"},)"
        R"({"type":"ticker","codes":[)" + codes + R"(],"isOnlyRealtime":true}])";

    ws_client_->send(sub_msg);
    Logger::info("[Upbit] Subscribed to {} ticker symbols", symbols.size());
}

void UpbitExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_orderbooks_ = symbols;
    }

    // Pre-allocate BBO entries
    for (const auto& s : symbols) {
        orderbook_bbo_[s];
    }

    if (!ws_client_ || !connected_.load()) return;

    // Build Upbit subscription: orderbook for real bid/ask
    // [{"ticket":"uuid"},{"type":"orderbook","codes":["KRW-BTC","KRW-ETH"],"isOnlyRealtime":true}]
    std::string codes;
    for (const auto& s : symbols) {
        if (!codes.empty()) codes += ",";
        codes += "\"" + symbol_to_upbit(s) + "\"";
    }

    std::string sub_msg = R"([{"ticket":"kimp-upbit-ob"},)"
        R"({"type":"orderbook","codes":[)" + codes + R"(],"isOnlyRealtime":true}])";

    ws_client_->send(sub_msg);
    Logger::info("[Upbit] Subscribed to {} orderbook symbols", symbols.size());
}

Order UpbitExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    if (side != Side::Sell) {
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = Exchange::Upbit;
        Logger::error("[Upbit] Market BUY by quantity is not supported; use place_market_buy_cost");
        return order;
    }
    if (quantity <= 0.0) {
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = Exchange::Upbit;
        Logger::error("[Upbit] Sell quantity {} invalid", quantity);
        return order;
    }
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = Exchange::Upbit;
        Logger::error("[Upbit] API credentials missing");
        return order;
    }

    Order order;
    order.exchange = Exchange::Upbit;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    const std::string market = symbol_to_upbit(symbol);
    const std::string query = "market=" + symbol_to_upbit(symbol) +
                              "&side=ask&ord_type=market&volume=" + format_upbit_number(quantity);
    const std::string body = std::string("{\"market\":\"") + market +
                             "\",\"side\":\"ask\",\"ord_type\":\"market\",\"volume\":\"" +
                             format_upbit_number(quantity) + "\"}";
    const std::string token = generate_jwt_token_with_query(query);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token},
        {"accept", "application/json"},
        {"Content-Type", "application/json; charset=utf-8"}
    };

    auto response = rest_client_->post("/v1/orders", body, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Market sell failed: {}", response.body);
        return order;
    }
    if (!populate_upbit_order_from_body(response.body, order)) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Failed to parse market sell response: {}", response.body);
        return order;
    }
    if (order.status != OrderStatus::Cancelled &&
        order.status != OrderStatus::Rejected &&
        order.status != OrderStatus::Expired) {
        // Upbit market orders may briefly report "wait" before turning to "done".
        // Treat accepted order submission as fill-pending so the async detail query can finalize it.
        order.status = OrderStatus::Filled;
    }
    return order;
}

Order UpbitExchange::place_market_buy_cost(const SymbolId& symbol, Price cost) {
    const double normalized_cost = std::floor(cost);
    if (normalized_cost < MIN_ORDER_KRW) {
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = Exchange::Upbit;
        Logger::error("[Upbit] Order cost {} below minimum {}", normalized_cost, MIN_ORDER_KRW);
        return order;
    }
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Order order;
        order.status = OrderStatus::Rejected;
        order.exchange = Exchange::Upbit;
        Logger::error("[Upbit] API credentials missing");
        return order;
    }

    Order order;
    order.exchange = Exchange::Upbit;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    order.price = normalized_cost;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    const std::string market = symbol_to_upbit(symbol);
    const std::string query = "market=" + symbol_to_upbit(symbol) +
                              "&side=bid&ord_type=price&price=" + format_upbit_number(normalized_cost, 0);
    const std::string body = std::string("{\"market\":\"") + market +
                             "\",\"side\":\"bid\",\"ord_type\":\"price\",\"price\":\"" +
                             format_upbit_number(normalized_cost, 0) + "\"}";
    const std::string token = generate_jwt_token_with_query(query);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token},
        {"accept", "application/json"},
        {"Content-Type", "application/json; charset=utf-8"}
    };

    auto response = rest_client_->post("/v1/orders", body, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Market buy failed: {}", response.body);
        return order;
    }
    if (!populate_upbit_order_from_body(response.body, order)) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Failed to parse market buy response: {}", response.body);
        return order;
    }
    if (order.status != OrderStatus::Cancelled &&
        order.status != OrderStatus::Rejected &&
        order.status != OrderStatus::Expired) {
        order.status = OrderStatus::Filled;
    }
    return order;
}

bool UpbitExchange::cancel_order(uint64_t /*order_id*/) {
    return false;
}

double UpbitExchange::get_balance(const std::string& currency) {
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Logger::warn("[Upbit] No API credentials — cannot fetch balance for {}", currency);
        return -1.0;
    }

    std::string token = generate_jwt_token();
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token},
        {"accept", "application/json"}
    };

    auto response = rest_client_->get("/v1/accounts", headers);
    if (!response.success) {
        Logger::error("[Upbit] Failed to fetch balance: {}", response.error);
        return -1.0;
    }

    try {
        simdjson::dom::parser parser;
        auto doc = parser.parse(response.body);
        for (auto item : doc.get_array()) {
            std::string code = parse_dom_string(item["currency"]);
            if (code == currency) {
                return parse_dom_double(item["balance"]) + parse_dom_double(item["locked"]);
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse balance: {}", e.what());
    }

    return 0.0;
}

bool UpbitExchange::query_order_detail(const std::string& order_id, Order& order) {
    if (order_id.empty()) {
        return false;
    }
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        return false;
    }

    const std::string query = "uuid=" + order_id;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::string token = generate_jwt_token_with_query(query);
        std::unordered_map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + token},
            {"accept", "application/json"}
        };

        auto response = rest_client_->get("/v1/order?" + query, headers);
        if (!response.success) {
            Logger::warn("[Upbit] Failed to query order {}: {}", order_id, response.body);
            return false;
        }

        Order updated = order;
        if (!populate_upbit_order_from_body(response.body, updated)) {
            Logger::warn("[Upbit] Failed to parse order detail {}: {}", order_id, response.body);
            return false;
        }

        updated.symbol = order.symbol;
        updated.update_time = std::chrono::system_clock::now();
        order = std::move(updated);

        if (order.status == OrderStatus::Filled) {
            return true;
        }
        if (order.status == OrderStatus::Cancelled ||
            order.status == OrderStatus::Rejected ||
            order.status == OrderStatus::Expired) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    return order.status == OrderStatus::Filled;
}

// ── JWT Authentication ──────────────────────────────────────────────────

namespace {

std::string base64url_encode_upbit(std::string_view raw) {
    auto encoded = kimp::utils::Crypto::base64_encode(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    for (char& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

std::string base64url_encode_upbit(const std::vector<uint8_t>& raw) {
    auto encoded = kimp::utils::Crypto::base64_encode(raw);
    for (char& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

std::string sign_upbit_jwt(std::string_view secret_key,
                           std::string_view payload_json) {
    static constexpr std::string_view header_json = R"({"alg":"HS512","typ":"JWT"})";
    std::string header_b64 = base64url_encode_upbit(header_json);
    std::string payload_b64 = base64url_encode_upbit(payload_json);
    std::string signing_input = header_b64 + "." + payload_b64;
    auto signature = kimp::utils::Crypto::hmac_sha512_raw(secret_key, signing_input);
    return signing_input + "." + base64url_encode_upbit(signature);
}

} // namespace

std::string UpbitExchange::generate_jwt_token() const {
    char buf[512];
    int len = std::snprintf(buf, sizeof(buf),
        R"({"access_key":"%s","nonce":"%s"})",
        credentials_.api_key.c_str(),
        utils::Crypto::generate_uuid().c_str());
    return sign_upbit_jwt(credentials_.secret_key, std::string_view(buf, static_cast<size_t>(len)));
}

std::string UpbitExchange::generate_jwt_token_with_query(const std::string& query_string) const {
    std::string hash = utils::Crypto::sha512(query_string);
    char buf[768];
    int len = std::snprintf(buf, sizeof(buf),
        R"({"access_key":"%s","nonce":"%s","query_hash":"%s","query_hash_alg":"SHA512"})",
        credentials_.api_key.c_str(),
        utils::Crypto::generate_uuid().c_str(),
        hash.c_str());
    return sign_upbit_jwt(credentials_.secret_key, std::string_view(buf, static_cast<size_t>(len)));
}

// ── Withdrawal Fee Fetcher ─────────────────────────────────────────────

namespace {

// Try to extract withdraw_fee from a /v1/withdraws/chance JSON response.
// Returns true and sets `fee` on success, false on parse failure.
bool parse_upbit_withdraw_fee(const std::string& body, double& fee) {
    try {
        simdjson::dom::parser parser;
        auto doc = parser.parse(body);
        auto currency_obj = doc["currency"].get_object();
        std::string_view fee_str = currency_obj["withdraw_fee"].get_string().value();
        auto [ptr, ec] = std::from_chars(fee_str.data(), fee_str.data() + fee_str.size(), fee);
        return (ec == std::errc{} && fee >= 0.0);
    } catch (...) {
        return false;
    }
}

} // namespace

std::unordered_map<std::string, std::vector<UpbitExchange::NetworkFee>>
UpbitExchange::fetch_withdrawal_fees(const std::vector<std::string>& coins) {
    std::unordered_map<std::string, std::vector<NetworkFee>> fees;

    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Logger::warn("[Upbit] No API credentials — cannot fetch withdrawal fees");
        return fees;
    }

    auto normalize = [](std::string_view raw) -> std::string {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c)))
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return out;
    };

    // ── Step 1: Fetch /v1/status/wallet to discover net_type for every coin ──
    std::unordered_map<std::string, std::vector<std::string>> coin_net_types;
    {
        std::string token = generate_jwt_token();
        std::unordered_map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + token},
            {"accept", "application/json"}
        };
        auto response = rest_client_->get("/v1/status/wallet", headers);
        if (!response.success) {
            Logger::warn("[Upbit] Failed to fetch wallet status: {} — falling back to guessing net_type",
                         response.error);
        } else {
            try {
                simdjson::dom::parser parser;
                auto doc = parser.parse(response.body);
                for (auto item : doc.get_array()) {
                    std::string_view currency = item["currency"].get_string().value();
                    std::string_view net_type = item["net_type"].get_string().value();
                    coin_net_types[std::string(currency)].emplace_back(net_type);
                }
                Logger::info("[Upbit] Wallet status: {} currency-network entries", coin_net_types.size());
            } catch (const simdjson::simdjson_error& e) {
                Logger::warn("[Upbit] Failed to parse wallet status: {}", e.what());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    // ── Step 2: For each coin, query /v1/withdraws/chance per net_type ──
    std::vector<std::string> failed_coins;

    for (const auto& coin : coins) {
        std::vector<std::string> net_types_to_try;
        auto it = coin_net_types.find(coin);
        if (it != coin_net_types.end() && !it->second.empty()) {
            net_types_to_try = it->second;
        } else {
            net_types_to_try = {"", coin};
        }

        std::vector<NetworkFee> coin_fees;

        for (const auto& nt : net_types_to_try) {
            std::string query = "currency=" + coin;
            if (!nt.empty()) {
                query += "&net_type=" + nt;
            }

            std::string token = generate_jwt_token_with_query(query);
            std::unordered_map<std::string, std::string> headers = {
                {"Authorization", "Bearer " + token},
                {"accept", "application/json"}
            };

            auto response = rest_client_->get("/v1/withdraws/chance?" + query, headers);

            double fee = 0.0;
            if (response.success && parse_upbit_withdraw_fee(response.body, fee)) {
                std::string net_name = nt.empty() ? coin : normalize(nt);
                coin_fees.push_back({std::move(net_name), fee});
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }

        if (!coin_fees.empty()) {
            fees[coin] = std::move(coin_fees);
        } else {
            failed_coins.push_back(coin);
        }
    }

    if (!failed_coins.empty()) {
        std::string missed;
        for (const auto& c : failed_coins) {
            if (!missed.empty()) missed += ", ";
            missed += c;
        }
        Logger::warn("[Upbit] Failed to fetch withdrawal fees for {} coins: {}",
                     failed_coins.size(), missed);
    }
    Logger::info("[Upbit] Loaded withdrawal fees for {} / {} coins", fees.size(), coins.size());
    return fees;
}

} // namespace kimp::exchange::upbit
