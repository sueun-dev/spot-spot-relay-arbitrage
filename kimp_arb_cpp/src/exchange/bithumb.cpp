#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <array>
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace {

struct FastBithumbDepthUpdate {
    kimp::SymbolId symbol;
    bool is_bid;
    double price;
    double quantity;
};

bool extract_quoted_value(std::string_view source,
                          std::string_view marker,
                          size_t search_from,
                          std::string_view& out_value,
                          size_t& out_end) {
    const size_t value_start = source.find(marker, search_from);
    if (value_start == std::string_view::npos) {
        return false;
    }

    const size_t content_start = value_start + marker.size();
    const size_t content_end = source.find('"', content_start);
    if (content_end == std::string_view::npos) {
        return false;
    }

    out_value = source.substr(content_start, content_end - content_start);
    out_end = content_end;
    return true;
}

bool parse_ticker_fast(std::string_view message, kimp::Ticker& ticker) {
    static constexpr std::string_view type_marker = R"("type":"ticker")";
    static constexpr std::string_view symbol_marker = R"("symbol":")";
    static constexpr std::string_view close_marker = R"("closePrice":")";

    if (message.find(type_marker) == std::string_view::npos) {
        return false;
    }

    std::string_view symbol_sv;
    size_t symbol_end = 0;
    if (!extract_quoted_value(message, symbol_marker, 0, symbol_sv, symbol_end)) {
        return false;
    }

    std::string_view close_sv;
    size_t close_end = 0;
    if (!extract_quoted_value(message, close_marker, symbol_end, close_sv, close_end)) {
        return false;
    }

    const size_t sep = symbol_sv.find('_');
    if (sep == std::string_view::npos) {
        return false;
    }

    ticker.exchange = kimp::Exchange::Bithumb;
    ticker.timestamp = std::chrono::steady_clock::now();
    ticker.symbol.set_base(symbol_sv.substr(0, sep));
    ticker.symbol.set_quote(symbol_sv.substr(sep + 1));
    ticker.last = kimp::opt::fast_stod(close_sv);
    ticker.bid = 0.0;
    ticker.ask = 0.0;
    return ticker.last > 0.0;
}

bool parse_orderbookdepth_fast(std::string_view message,
                               std::vector<FastBithumbDepthUpdate>& updates) {
    static constexpr std::string_view type_marker = R"("type":"orderbookdepth")";
    static constexpr std::string_view list_marker = R"("list":[)";
    static constexpr std::string_view symbol_marker = R"("symbol":")";
    static constexpr std::string_view order_type_marker = R"("orderType":")";
    static constexpr std::string_view price_marker = R"("price":")";
    static constexpr std::string_view quantity_marker = R"("quantity":")";

    if (message.find(type_marker) == std::string_view::npos) {
        return false;
    }

    size_t cursor = message.find(list_marker);
    if (cursor == std::string_view::npos) {
        return false;
    }
    cursor += list_marker.size();

    while (true) {
        const size_t object_start = message.find('{', cursor);
        if (object_start == std::string_view::npos) {
            break;
        }
        const size_t object_end = message.find('}', object_start);
        if (object_end == std::string_view::npos) {
            return false;
        }
        std::string_view item = message.substr(object_start, object_end - object_start + 1);

        std::string_view symbol_sv;
        size_t field_end = 0;
        if (!extract_quoted_value(item, symbol_marker, 0, symbol_sv, field_end)) {
            cursor = object_end + 1;
            continue;
        }

        std::string_view order_type_sv;
        if (!extract_quoted_value(item, order_type_marker, 0, order_type_sv, field_end)) {
            return false;
        }

        std::string_view price_sv;
        if (!extract_quoted_value(item, price_marker, 0, price_sv, field_end)) {
            return false;
        }

        std::string_view quantity_sv;
        if (!extract_quoted_value(item, quantity_marker, 0, quantity_sv, field_end)) {
            return false;
        }

        FastBithumbDepthUpdate update;
        update.symbol = kimp::SymbolId::from_bithumb_format(symbol_sv);
        update.is_bid = (order_type_sv == "bid");
        update.price = kimp::opt::fast_stod(price_sv);
        update.quantity = kimp::opt::fast_stod(quantity_sv);
        updates.push_back(std::move(update));

        cursor = object_end + 1;
    }

    return !updates.empty();
}

std::string base64url_encode(std::string_view raw) {
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

std::string base64url_encode(const std::vector<uint8_t>& raw) {
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

std::string sign_bithumb_v1_jwt(std::string_view secret_key,
                                std::string_view payload_json) {
    static constexpr std::string_view header_json = R"({"alg":"HS256","typ":"JWT"})";
    std::string header_b64 = base64url_encode(header_json);
    std::string payload_b64 = base64url_encode(payload_json);
    std::string signing_input = header_b64 + "." + payload_b64;
    auto signature = kimp::utils::Crypto::hmac_sha256_raw(secret_key, signing_input);
    return signing_input + "." + base64url_encode(signature);
}

} // namespace

namespace kimp::exchange::bithumb {

BithumbExchange::~BithumbExchange() {
    stop_orderbook_resync_loop();
}

bool BithumbExchange::connect() {
    if (connected_) {
        Logger::warn("[Bithumb] Already connected");
        return true;
    }

    Logger::info("[Bithumb] Connecting...");

    // Initialize REST connection pool first (pre-establish SSL connections)
    if (!initialize_rest()) {
        Logger::error("[Bithumb] Failed to initialize REST connection pool");
        return false;
    }
    Logger::info("[Bithumb] REST connection pool initialized (4 persistent connections)");

    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "Bithumb-WS");

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType /*type*/) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Bithumb] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& /*reason*/) {
        on_ws_disconnected();
    });

    ws_client_->connect(credentials_.ws_endpoint);

    const std::string private_ws_endpoint = resolve_private_ws_endpoint();
    if (!private_ws_endpoint.empty() &&
        !credentials_.api_key.empty() &&
        !credentials_.secret_key.empty()) {
        private_ws_ = std::make_shared<network::WebSocketClient>(io_context_, "Bithumb-Private-WS");
        private_ws_->set_handshake_headers_callback([this]() {
            return build_v1_auth_headers();
        });

        private_ws_->set_message_callback([this](std::string_view msg, network::MessageType) {
            on_private_ws_message(msg);
        });

        private_ws_->set_connect_callback([this](bool success, const std::string& error) {
            if (success) {
                private_ws_authenticated_ = true;
                Logger::info("[Bithumb-PrivateWS] Connected, subscribing to myOrder");
                subscribe_private_myorder();
            } else {
                Logger::error("[Bithumb-PrivateWS] Connection failed: {}", error);
            }
        });

        private_ws_->set_disconnect_callback([this](const std::string& reason) {
            private_ws_authenticated_ = false;
            Logger::warn("[Bithumb-PrivateWS] Disconnected: {}", reason);
        });

        private_ws_->connect(private_ws_endpoint);
    } else {
        Logger::info("[Bithumb-PrivateWS] Disabled (missing private endpoint or API credentials)");
    }

    return true;
}

void BithumbExchange::disconnect() {
    stop_orderbook_resync_loop();

    if (private_ws_) {
        private_ws_->disconnect();
        private_ws_authenticated_ = false;
    }
    {
        std::lock_guard lock(fill_cache_mutex_);
        fill_cache_.clear();
    }

    // Shutdown REST connection pool
    shutdown_rest();

    if (ws_client_) {
        ws_client_->disconnect();
    }
    connected_ = false;
    Logger::info("[Bithumb] Disconnected");
}

void BithumbExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    // Store symbols for reconnection
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_tickers_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[Bithumb] Cannot subscribe, not connected");
        return;
    }

    constexpr size_t BATCH_SIZE = 30;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"type":"ticker","symbols":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << "\"" << symbols[i].to_bithumb_format() << "\"";
            first = false;
        }

        ss << R"(],"tickTypes":["MID"]})";
        ws_client_->send(ss.str());

        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[Bithumb] Subscribed to {} tickers in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);
}

void BithumbExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    // Store for reconnection
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_orderbooks_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[Bithumb] Cannot subscribe orderbook, not connected");
        return;
    }

    constexpr size_t BATCH_SIZE = 30;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"type":"orderbookdepth","symbols":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << "\"" << symbols[i].to_bithumb_format() << "\"";
            first = false;
        }

        ss << R"(]})";
        ws_client_->send(ss.str());

        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[Bithumb] Subscribed to {} orderbook depth streams in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);

    start_orderbook_resync_loop();
}

void BithumbExchange::start_orderbook_resync_loop() {
    if (orderbook_resync_running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    Logger::info("[Bithumb] Starting authoritative orderbook BBO resync loop ({} ms)",
                 ORDERBOOK_RESYNC_INTERVAL.count());
    orderbook_resync_thread_ = std::thread([this]() {
        constexpr auto sleep_slice = std::chrono::milliseconds(50);
        while (orderbook_resync_running_.load(std::memory_order_acquire)) {
            for (auto slept = std::chrono::milliseconds(0);
                 slept < ORDERBOOK_RESYNC_INTERVAL &&
                 orderbook_resync_running_.load(std::memory_order_acquire);
                 slept += sleep_slice) {
                std::this_thread::sleep_for(sleep_slice);
            }
            if (!orderbook_resync_running_.load(std::memory_order_acquire)) break;
            if (!connected_.load(std::memory_order_acquire)) {
                continue;
            }

            std::vector<SymbolId> symbols;
            {
                std::lock_guard lock(subscription_mutex_);
                symbols = subscribed_orderbooks_;
            }
            if (symbols.empty()) {
                continue;
            }

            fetch_all_orderbook_snapshots(symbols, ORDERBOOK_BBO_DEPTH);
        }
    });
}

void BithumbExchange::stop_orderbook_resync_loop() {
    if (!orderbook_resync_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (orderbook_resync_thread_.joinable()) {
        orderbook_resync_thread_.join();
    }
    Logger::info("[Bithumb] Stopped authoritative orderbook BBO resync loop");
}

std::vector<SymbolId> BithumbExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/public/ticker/ALL_KRW");
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch markets: {}", response.error);
        return symbols;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto data = doc["data"].get_object();

        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key != "date") {  // Skip timestamp field
                symbols.emplace_back(std::string(key), "KRW");
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse markets: {}", e.what());
    }

    Logger::info("[Bithumb] Found {} KRW markets", symbols.size());
    return symbols;
}

std::vector<Ticker> BithumbExchange::fetch_all_tickers() {
    std::vector<Ticker> tickers;

    auto response = rest_client_->get("/public/ticker/ALL_KRW");
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch all tickers: {}", response.error);
        return tickers;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto data = doc["data"].get_object();

        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key == "date") continue;  // Skip timestamp field

            auto item = field.value().get_object();

            Ticker ticker;
            ticker.exchange = Exchange::Bithumb;
            ticker.symbol = SymbolId(std::string(key), "KRW");

            // Parse prices
            auto closing_result = item["closing_price"];
            if (!closing_result.error()) {
                std::string_view price_str = closing_result.get_string().value();
                ticker.last = opt::fast_stod(price_str);
                ticker.bid = 0.0;
                ticker.ask = 0.0;

                // Overlay real bid/ask from orderbook BBO
                if (orderbook_ready_.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard lock(orderbook_mutex_);
                        auto bbo_it = orderbook_bbo_.find(ticker.symbol);
                        if (bbo_it != orderbook_bbo_.end()) {
                            double real_bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
                            double real_ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
                            double real_bid_qty = bbo_it->second.best_bid_qty.load(std::memory_order_acquire);
                            double real_ask_qty = bbo_it->second.best_ask_qty.load(std::memory_order_acquire);
                            if (real_bid > 0.0) ticker.bid = real_bid;
                            if (real_ask > 0.0) ticker.ask = real_ask;
                            if (real_bid_qty > 0.0) ticker.bid_qty = real_bid_qty;
                            if (real_ask_qty > 0.0) ticker.ask_qty = real_ask_qty;
                        }
                    }
                }
            }

            ticker.timestamp = std::chrono::steady_clock::now();

            if (ticker.last > 0) {
                tickers.push_back(ticker);

                // Also cache USDT price
                if (key == "USDT") {
                    usdt_krw_price_.store(ticker.last);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse all tickers: {}", e.what());
    }

    Logger::info("[Bithumb] Fetched {} tickers via REST", tickers.size());
    return tickers;
}

void BithumbExchange::fetch_all_orderbook_snapshots(const std::vector<SymbolId>& symbols,
                                                    std::size_t depth_count) {
    const std::size_t requested_depth =
        depth_count == 0 ? ORDERBOOK_BBO_DEPTH : depth_count;
    auto response = rest_client_->get(
        "/public/orderbook/ALL_KRW?count=" + std::to_string(requested_depth));
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch orderbook snapshots: {}", response.error);
        return;
    }

    try {
        // Use a local parser to avoid contention with WebSocket parser
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        auto status = doc["status"].get_string().value();
        if (status != "0000") {
            Logger::error("[Bithumb] Orderbook API returned status: {}", std::string(status));
            return;
        }

        std::unordered_set<SymbolId> requested_symbols;
        requested_symbols.reserve(symbols.size());
        for (const auto& symbol : symbols) {
            requested_symbols.insert(symbol);
        }

        auto data = doc["data"].get_object();
        int count = 0;
        std::vector<SymbolId> seeded_symbols;
        seeded_symbols.reserve(symbols.size());

        {
            std::lock_guard lock(orderbook_mutex_);

            for (auto field : data) {
                std::string_view key = field.unescaped_key().value();
                if (key == "timestamp" || key == "payment_currency") continue;

                SymbolId sym(key, "KRW");
                if (!requested_symbols.empty() &&
                    requested_symbols.find(sym) == requested_symbols.end()) {
                    continue;
                }

                auto& state = orderbook_state_[sym];
                state.bids.clear();
                state.asks.clear();

                auto item = field.value().get_object();

                auto bids_arr = item["bids"];
                if (!bids_arr.error()) {
                    for (auto bid : bids_arr.get_array()) {
                        std::string_view price_str = bid["price"].get_string().value();
                        std::string_view qty_str = bid["quantity"].get_string().value();
                        double price = opt::fast_stod(price_str);
                        double qty = opt::fast_stod(qty_str);
                        if (price > 0 && qty > 0) {
                            state.bids[price] = qty;
                        }
                    }
                }

                auto asks_arr = item["asks"];
                if (!asks_arr.error()) {
                    for (auto ask : asks_arr.get_array()) {
                        std::string_view price_str = ask["price"].get_string().value();
                        std::string_view qty_str = ask["quantity"].get_string().value();
                        double price = opt::fast_stod(price_str);
                        double qty = opt::fast_stod(qty_str);
                        if (price > 0 && qty > 0) {
                            state.asks[price] = qty;
                        }
                    }
                }

                state.initialized = true;
                update_bbo(sym);
                seeded_symbols.push_back(sym);
                ++count;
            }
        }

        orderbook_ready_.store(true, std::memory_order_release);
        Logger::info("[Bithumb] Loaded orderbook snapshots for {} symbols (depth={})",
                     count, requested_depth);

        int dispatched = 0;
        for (const auto& sym : seeded_symbols) {
            auto ticker = make_bbo_ticker(sym);
            if (!ticker) continue;
            if (ticker->symbol.get_base() == "USDT") {
                usdt_krw_price_.store(ticker->last, std::memory_order_release);
            }
            dispatch_ticker(*ticker);
            ++dispatched;
        }
        Logger::info("[Bithumb] Seeded {} synthetic BBO tickers from snapshots", dispatched);
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse orderbook snapshots: {}", e.what());
    }
}

double BithumbExchange::get_usdt_krw_price() {
    double cached = usdt_krw_price_.load();
    if (cached > 0) return cached;

    auto response = rest_client_->get("/public/ticker/USDT_KRW");
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch USDT price: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        std::string_view price_str = doc["data"]["closing_price"].get_string().value();
        double price = opt::fast_stod(price_str);
        usdt_krw_price_.store(price);
        return price;
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse USDT price: {}", e.what());
    }

    return 0.0;
}

Order BithumbExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bithumb;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    const char* endpoint = (side == Side::Buy) ? "/trade/market_buy" : "/trade/market_sell";

    char params_buf[256];
    int params_len;
    if (side == Side::Sell) {
        params_len = std::snprintf(params_buf, sizeof(params_buf),
            "order_currency=%s&payment_currency=KRW&units=%.8f",
            std::string(symbol.get_base()).c_str(), quantity);
    } else {
        params_len = std::snprintf(params_buf, sizeof(params_buf),
            "order_currency=%s&payment_currency=KRW&units=%.0f",
            std::string(symbol.get_base()).c_str(), quantity);
    }
    std::string params_str(params_buf, static_cast<size_t>(params_len));

    auto headers = build_auth_headers(endpoint, params_str);
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto response = rest_client_->post(endpoint, params_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bithumb] Order failed: {}", response.body);
        return order;
    }

    Logger::debug("[Bithumb] Order raw response: {}", response.body);

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        std::string_view status = doc["status"].get_string().value();
        if (status == "0000") {
            order.status = OrderStatus::Filled;
            auto oid = doc["order_id"];
            if (!oid.error()) {
                order.order_id_str = std::string(oid.get_string().value());
            }
        } else {
            order.status = OrderStatus::Rejected;
        }
    } catch (const simdjson::simdjson_error& e) {
        order.status = OrderStatus::Rejected;
    }

    return order;
}

Order BithumbExchange::place_market_buy_cost(const SymbolId& symbol, Price cost) {
    if (cost < MIN_ORDER_KRW) {
        Order order;
        order.status = OrderStatus::Rejected;
        Logger::error("[Bithumb] Order cost {} below minimum {}", cost, MIN_ORDER_KRW);
        return order;
    }

    // Bithumb market buy uses KRW amount
    return place_market_order(symbol, Side::Buy, cost);
}

Order BithumbExchange::place_market_buy_quantity(const SymbolId& symbol, Quantity quantity) {
    if (quantity <= 0) {
        Order order;
        order.status = OrderStatus::Rejected;
        Logger::error("[Bithumb] Buy quantity {} invalid", quantity);
        return order;
    }

    Order order;
    order.exchange = Exchange::Bithumb;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    const char* endpoint = "/trade/market_buy";

    char params_buf[256];
    int params_len = std::snprintf(params_buf, sizeof(params_buf),
        "order_currency=%s&payment_currency=KRW&units=%.8f",
        std::string(symbol.get_base()).c_str(), quantity);
    std::string params_str(params_buf, static_cast<size_t>(params_len));

    auto headers = build_auth_headers(endpoint, params_str);
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto response = rest_client_->post(endpoint, params_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bithumb] Order failed: {}", response.body);
        return order;
    }

    Logger::debug("[Bithumb] BuyQty raw response: {}", response.body);

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        std::string_view status = doc["status"].get_string().value();
        if (status == "0000") {
            order.status = OrderStatus::Filled;
            auto oid = doc["order_id"];
            if (!oid.error()) {
                order.order_id_str = std::string(oid.get_string().value());
            }
        } else {
            order.status = OrderStatus::Rejected;
        }
    } catch (const simdjson::simdjson_error&) {
        order.status = OrderStatus::Rejected;
    }

    return order;
}

bool BithumbExchange::cancel_order(uint64_t /*order_id*/) {
    // Bithumb cancel implementation
    return false;  // TODO: Implement if needed
}

double BithumbExchange::get_balance(const std::string& currency) {
    std::string endpoint = "/info/balance";
    std::string params = "currency=" + currency;

    auto headers = build_auth_headers(endpoint, params);
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto response = rest_client_->post(endpoint, params, headers);
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch balance: {}", response.error);
        return -1.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        std::string field = "available_" + currency;

        // Convert to lowercase for the field name
        for (auto& c : field) c = std::tolower(c);

        auto balance = doc["data"][field];
        if (!balance.error()) {
            std::string_view bal_str = balance.get_string().value();
            return opt::fast_stod(bal_str);
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse balance: {}", e.what());
    }

    return -1.0;
}

void BithumbExchange::on_ws_message(std::string_view message) {
    // Fast message type routing via string search (avoids double JSON parse)
    if (message.find("orderbookdepth") != std::string_view::npos) {
        auto updated_symbols = parse_orderbookdepth_message(message);

        // Dispatch synthetic tickers for BBO-changed symbols (0ms propagation)
        if (orderbook_ready_.load(std::memory_order_acquire)) {
            for (const auto& sym : updated_symbols) {
                auto ticker = make_bbo_ticker(sym);
                if (!ticker) continue;
                if (ticker->symbol.get_base() == "USDT") {
                    usdt_krw_price_.store(ticker->last, std::memory_order_release);
                }
                dispatch_ticker(*ticker);
            }
        }
        return;
    }

    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        if (ticker.symbol.get_base() == "USDT") {
            usdt_krw_price_.store(ticker.last);
        }

        // Overlay real bid/ask from orderbook BBO
        if (orderbook_ready_.load(std::memory_order_acquire)) {
            std::lock_guard lock(orderbook_mutex_);
            auto bbo_it = orderbook_bbo_.find(ticker.symbol);
            if (bbo_it != orderbook_bbo_.end()) {
                double real_bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
                double real_ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
                double real_bid_qty = bbo_it->second.best_bid_qty.load(std::memory_order_acquire);
                double real_ask_qty = bbo_it->second.best_ask_qty.load(std::memory_order_acquire);
                if (real_bid > 0.0) ticker.bid = real_bid;
                if (real_ask > 0.0) ticker.ask = real_ask;
                if (real_bid_qty > 0.0) ticker.bid_qty = real_bid_qty;
                if (real_ask_qty > 0.0) ticker.ask_qty = real_ask_qty;
            }
        }

        dispatch_ticker(ticker);
    }
}

void BithumbExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[Bithumb] WebSocket connected");

    // Invalidate orderbook state on reconnect
    {
        std::lock_guard lock(orderbook_mutex_);
        for (auto& [key, state] : orderbook_state_) {
            state.bids.clear();
            state.asks.clear();
            state.initialized = false;
        }
    }
    orderbook_ready_.store(false, std::memory_order_release);

    // Resubscribe to tickers after reconnection
    std::vector<SymbolId> symbols_to_subscribe;
    std::vector<SymbolId> orderbooks_to_subscribe;
    {
        std::lock_guard lock(subscription_mutex_);
        symbols_to_subscribe = subscribed_tickers_;
        orderbooks_to_subscribe = subscribed_orderbooks_;
    }

    if (!symbols_to_subscribe.empty()) {
        Logger::info("[Bithumb] Resubscribing to {} tickers after reconnection", symbols_to_subscribe.size());
        subscribe_ticker(symbols_to_subscribe);
    }

    if (!orderbooks_to_subscribe.empty()) {
        // Re-fetch snapshots then subscribe to deltas
        auto weak_self = weak_from_this();
        std::thread([weak_self, orderbooks_to_subscribe]() {
            auto base_self = weak_self.lock();
            if (!base_self) return;
            auto self = std::dynamic_pointer_cast<BithumbExchange>(base_self);
            if (!self) return;

            self->fetch_all_orderbook_snapshots(orderbooks_to_subscribe);
            if (!self->connected_.load(std::memory_order_acquire)) return;
            self->subscribe_orderbook(orderbooks_to_subscribe);
            Logger::info("[Bithumb] Orderbook re-initialized after reconnection");
        }).detach();
    }
}

void BithumbExchange::on_ws_disconnected() {
    connected_ = false;
    orderbook_ready_.store(false, std::memory_order_release);
    Logger::warn("[Bithumb] WebSocket disconnected");
}

void BithumbExchange::on_private_ws_message(std::string_view message) {
    if (message.find("myOrder") == std::string_view::npos) {
        return;
    }

    try {
        simdjson::padded_string padded(message);
        auto doc = json_parser_.iterate(padded);
        simdjson::ondemand::object obj = doc.get_object();

        auto parse_number = [](simdjson::ondemand::value value) -> double {
            auto string_value = value.get_string();
            if (!string_value.error()) {
                return opt::fast_stod(string_value.value());
            }
            auto double_value = value.get_double();
            if (!double_value.error()) {
                return double_value.value();
            }
            return 0.0;
        };

        std::string_view type;
        std::string_view order_id_sv;
        std::string_view state;
        double executed_volume = 0.0;
        double executed_funds = 0.0;

        for (auto field : obj) {
            std::string_view key = field.unescaped_key().value();
            simdjson::ondemand::value value = field.value();

            if (key == "ty" || key == "type") {
                auto sv = value.get_string();
                if (!sv.error()) type = sv.value();
            } else if (key == "uid" || key == "uuid") {
                auto sv = value.get_string();
                if (!sv.error()) order_id_sv = sv.value();
            } else if (key == "s" || key == "state") {
                auto sv = value.get_string();
                if (!sv.error()) state = sv.value();
            } else if (key == "ev" || key == "executed_volume") {
                executed_volume = parse_number(value);
            } else if (key == "ef" || key == "executed_funds") {
                executed_funds = parse_number(value);
            }
        }

        if (type != "myOrder") {
            return;
        }

        if (order_id_sv.empty() || state.empty()) {
            return;
        }

        if (executed_volume <= 0.0 || executed_funds <= 0.0) {
            return;
        }

        if (state != "trade" && state != "done" && state != "cancel") {
            return;
        }

        FillInfo fill;
        fill.filled_qty = executed_volume;
        fill.avg_price = executed_funds / executed_volume;

        {
            std::lock_guard lock(fill_cache_mutex_);
            fill_cache_[std::string(order_id_sv)] = fill;
        }
        fill_cache_cv_.notify_all();
    } catch (const simdjson::simdjson_error&) {
        // Ignore non-fill/private control messages
    }
}

std::string BithumbExchange::generate_signature(const std::string& endpoint,
                                                  const std::string& params,
                                                  int64_t timestamp) const {
    std::string message = endpoint + '\0' + params + '\0' + std::to_string(timestamp);
    return utils::Crypto::hmac_sha512(credentials_.secret_key, message);
}

std::unordered_map<std::string, std::string> BithumbExchange::build_auth_headers(
    const std::string& endpoint, const std::string& params) const {

    int64_t timestamp = utils::Crypto::timestamp_ms();
    std::string signature = generate_signature(endpoint, params, timestamp);
    std::string signature_b64 = utils::Crypto::base64_encode(
        reinterpret_cast<const uint8_t*>(signature.data()), signature.size());

    return {
        {"Api-Key", credentials_.api_key},
        {"Api-Sign", signature_b64},
        {"Api-Nonce", std::to_string(timestamp)}
    };
}

std::string BithumbExchange::generate_v1_jwt_token() const {
    std::ostringstream payload;
    payload << R"({"access_key":")" << credentials_.api_key
            << R"(","nonce":")" << utils::Crypto::generate_uuid()
            << R"(","timestamp":)" << utils::Crypto::timestamp_ms()
            << "}";
    return sign_bithumb_v1_jwt(credentials_.secret_key, payload.str());
}

std::string BithumbExchange::generate_v1_jwt_token_with_query(const std::string& query_string) const {
    std::ostringstream payload;
    payload << R"({"access_key":")" << credentials_.api_key
            << R"(","nonce":")" << utils::Crypto::generate_uuid()
            << R"(","timestamp":)" << utils::Crypto::timestamp_ms()
            << R"(,"query_hash":")" << utils::Crypto::sha512(query_string)
            << R"(","query_hash_alg":"SHA512"})";
    return sign_bithumb_v1_jwt(credentials_.secret_key, payload.str());
}

std::unordered_map<std::string, std::string> BithumbExchange::build_v1_auth_headers() const {
    return {
        {"Authorization", "Bearer " + generate_v1_jwt_token()},
        {"accept", "application/json"}
    };
}

std::unordered_map<std::string, std::string> BithumbExchange::build_v1_auth_headers(
    const std::string& query_string) const {
    return {
        {"Authorization", "Bearer " + generate_v1_jwt_token_with_query(query_string)},
        {"accept", "application/json"}
    };
}

std::string BithumbExchange::resolve_private_ws_endpoint() const {
    if (!credentials_.ws_private_endpoint.empty()) {
        return credentials_.ws_private_endpoint;
    }
    return "wss://ws-api.bithumb.com/websocket/v1/private";
}

void BithumbExchange::subscribe_private_myorder() {
    if (!private_ws_ || !private_ws_->is_connected()) {
        Logger::warn("[Bithumb-PrivateWS] Cannot subscribe to myOrder, not connected");
        return;
    }

    private_ws_->send(std::string(
        R"([{"ticket":"kimp-bithumb-private"},{"type":"myOrder","codes":[]},{"format":"SIMPLE"}])"));
}

bool BithumbExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    if (parse_ticker_fast(message, ticker)) {
        std::lock_guard lock(orderbook_mutex_);
        last_price_cache_[ticker.symbol] = ticker.last;
        return true;
    }

    try {
        // Use padded_string to satisfy simdjson's padding requirement
        // This fixes potential parsing issues with decimal prices
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        std::string_view type = doc["type"].get_string().value();
        if (type != "ticker") return false;

        auto content = doc["content"];

        ticker.exchange = Exchange::Bithumb;
        ticker.timestamp = std::chrono::steady_clock::now();

        std::string_view symbol_str = content["symbol"].get_string().value();
        auto pos = symbol_str.find('_');
        if (pos != std::string_view::npos) {
            ticker.symbol.set_base(symbol_str.substr(0, pos));
            ticker.symbol.set_quote(symbol_str.substr(pos + 1));
        }

        std::string_view close_str = content["closePrice"].get_string().value();
        ticker.last = opt::fast_stod(close_str);
        ticker.bid = 0.0;
        ticker.ask = 0.0;

        // Cache last price for orderbookdepth-driven dispatch
        {
            std::lock_guard lock(orderbook_mutex_);
            last_price_cache_[ticker.symbol] = ticker.last;
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse ticker: {}", e.what());
        return false;
    }
}

std::optional<Ticker> BithumbExchange::make_bbo_ticker(const SymbolId& symbol) {
    std::lock_guard lock(orderbook_mutex_);

    auto bbo_it = orderbook_bbo_.find(symbol);
    if (bbo_it == orderbook_bbo_.end()) {
        return std::nullopt;
    }

    const double bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
    const double ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
    const double bid_qty = bbo_it->second.best_bid_qty.load(std::memory_order_acquire);
    const double ask_qty = bbo_it->second.best_ask_qty.load(std::memory_order_acquire);
    if (bid <= 0.0 || ask <= 0.0) {
        return std::nullopt;
    }

    double last = 0.0;
    auto last_it = last_price_cache_.find(symbol);
    if (last_it != last_price_cache_.end() && last_it->second > 0.0) {
        last = last_it->second;
    } else {
        last = (bid + ask) * 0.5;
    }

    Ticker ticker;
    ticker.exchange = Exchange::Bithumb;
    ticker.timestamp = std::chrono::steady_clock::now();
    ticker.symbol = symbol;
    ticker.last = last;
    ticker.bid = bid;
    ticker.ask = ask;
    ticker.bid_qty = bid_qty;
    ticker.ask_qty = ask_qty;
    return ticker;
}

std::vector<SymbolId> BithumbExchange::parse_orderbookdepth_message(std::string_view message) {
    std::vector<SymbolId> updated_symbols;
    std::vector<FastBithumbDepthUpdate> fast_updates;
    if (parse_orderbookdepth_fast(message, fast_updates)) {
        std::lock_guard lock(orderbook_mutex_);

        SymbolId last_updated_symbol;
        bool has_last = false;
        updated_symbols.reserve(fast_updates.size());

        for (const auto& item : fast_updates) {
            auto it = orderbook_state_.find(item.symbol);
            if (it == orderbook_state_.end() || !it->second.initialized) {
                Logger::debug("[Bithumb] Orderbook update for uninitialized symbol {}, skipping", item.symbol.to_string());
                continue;
            }

            auto& state = it->second;
            if (item.is_bid) {
                if (item.quantity <= 0.0) {
                    state.bids.erase(item.price);
                } else {
                    state.bids[item.price] = item.quantity;
                }
            } else {
                if (item.quantity <= 0.0) {
                    state.asks.erase(item.price);
                } else {
                    state.asks[item.price] = item.quantity;
                }
            }

            if (!has_last || item.symbol != last_updated_symbol) {
                if (has_last) {
                    update_bbo(last_updated_symbol);
                    updated_symbols.push_back(last_updated_symbol);
                }
                last_updated_symbol = item.symbol;
                has_last = true;
            }
        }

        if (has_last) {
            update_bbo(last_updated_symbol);
            updated_symbols.push_back(last_updated_symbol);
        }

        return updated_symbols;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        std::string_view type = doc["type"].get_string().value();
        if (type != "orderbookdepth") return updated_symbols;

        auto content = doc["content"];
        auto list = content["list"].get_array();

        std::lock_guard lock(orderbook_mutex_);

        SymbolId last_updated_symbol;
        bool has_last = false;

        for (auto item : list) {
            std::string_view symbol_sv = item["symbol"].get_string().value();
            SymbolId sym = SymbolId::from_bithumb_format(symbol_sv);

            auto it = orderbook_state_.find(sym);
            if (it == orderbook_state_.end() || !it->second.initialized) continue;

            auto& state = it->second;

            std::string_view order_type = item["orderType"].get_string().value();
            std::string_view price_str = item["price"].get_string().value();
            std::string_view qty_str = item["quantity"].get_string().value();

            double price = opt::fast_stod(price_str);
            double quantity = opt::fast_stod(qty_str);

            if (order_type == "bid") {
                if (quantity <= 0.0) {
                    state.bids.erase(price);
                } else {
                    state.bids[price] = quantity;
                }
            } else if (order_type == "ask") {
                if (quantity <= 0.0) {
                    state.asks.erase(price);
                } else {
                    state.asks[price] = quantity;
                }
            }

            if (!has_last || sym != last_updated_symbol) {
                if (has_last) {
                    update_bbo(last_updated_symbol);
                    updated_symbols.push_back(last_updated_symbol);
                }
                last_updated_symbol = sym;
                has_last = true;
            }
        }

        if (has_last) {
            update_bbo(last_updated_symbol);
            updated_symbols.push_back(last_updated_symbol);
        }

    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse orderbookdepth: {}", e.what());
    }
    return updated_symbols;
}

void BithumbExchange::update_bbo(const SymbolId& symbol) {
    // Called with orderbook_mutex_ held
    auto state_it = orderbook_state_.find(symbol);
    if (state_it == orderbook_state_.end() || !state_it->second.initialized) return;

    const auto& state = state_it->second;
    auto& bbo = orderbook_bbo_[symbol];

    if (!state.bids.empty()) {
        bbo.best_bid.store(state.bids.begin()->first, std::memory_order_release);
        bbo.best_bid_qty.store(state.bids.begin()->second, std::memory_order_release);
    } else {
        bbo.best_bid.store(0.0, std::memory_order_release);
        bbo.best_bid_qty.store(0.0, std::memory_order_release);
    }
    if (!state.asks.empty()) {
        bbo.best_ask.store(state.asks.begin()->first, std::memory_order_release);
        bbo.best_ask_qty.store(state.asks.begin()->second, std::memory_order_release);
    } else {
        bbo.best_ask.store(0.0, std::memory_order_release);
        bbo.best_ask_qty.store(0.0, std::memory_order_release);
    }
}

bool BithumbExchange::query_order_detail_ws(const std::string& order_id, Order& order) {
    std::unique_lock lock(fill_cache_mutex_);

    auto apply_fill = [&](auto it) {
        order.average_price = it->second.avg_price;
        order.filled_quantity = it->second.filled_qty;
        fill_cache_.erase(it);
    };

    auto it = fill_cache_.find(order_id);
    if (it != fill_cache_.end()) {
        apply_fill(it);
        Logger::info("[Bithumb-WS] Fill: orderId={}, avgPrice={:.2f}, filledQty={:.8f}",
                     order_id, order.average_price, order.filled_quantity);
        return true;
    }

    if (!private_ws_authenticated_.load(std::memory_order_acquire)) {
        return false;
    }

    bool found = fill_cache_cv_.wait_for(lock, std::chrono::milliseconds(150), [&]() {
        return fill_cache_.find(order_id) != fill_cache_.end();
    });
    if (!found) {
        return false;
    }

    it = fill_cache_.find(order_id);
    if (it == fill_cache_.end()) {
        return false;
    }

    apply_fill(it);
    Logger::info("[Bithumb-WS] Fill (waited): orderId={}, avgPrice={:.2f}, filledQty={:.8f}",
                 order_id, order.average_price, order.filled_quantity);
    return true;
}

bool BithumbExchange::query_order_detail_v1(const std::string& order_id, Order& order) {
    const std::string query = "uuid=" + utils::Crypto::url_encode(order_id);
    auto headers = build_v1_auth_headers(query);
    auto response = rest_client_->get("/v1/order?" + query, headers);
    if (!response.success) {
        Logger::debug("[Bithumb] v1 order query failed for {}: {}", order_id, response.error);
        return false;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        double executed_volume = 0.0;
        auto executed = doc["executed_volume"];
        if (!executed.error()) {
            executed_volume = opt::fast_stod(executed.get_string().value());
        }
        if (executed_volume <= 0.0) {
            return false;
        }

        double total_funds = 0.0;
        double total_units = 0.0;
        auto trades = doc["trades"];
        if (!trades.error()) {
            for (auto trade : trades.get_array()) {
                double volume = 0.0;
                double funds = 0.0;

                auto volume_field = trade["volume"];
                if (!volume_field.error()) {
                    volume = opt::fast_stod(volume_field.get_string().value());
                }

                auto funds_field = trade["funds"];
                if (!funds_field.error()) {
                    funds = opt::fast_stod(funds_field.get_string().value());
                } else {
                    auto price_field = trade["price"];
                    if (!price_field.error()) {
                        funds = opt::fast_stod(price_field.get_string().value()) * volume;
                    }
                }

                if (volume > 0.0 && funds > 0.0) {
                    total_units += volume;
                    total_funds += funds;
                }
            }
        }

        order.filled_quantity = total_units > 0.0 ? total_units : executed_volume;
        if (total_units > 0.0 && total_funds > 0.0) {
            order.average_price = total_funds / total_units;
        } else {
            order.average_price = 0.0;
        }

        Logger::info("[Bithumb] Fill(v1): orderId={}, avgPrice={:.2f}, filledQty={:.8f}",
                     order_id, order.average_price, order.filled_quantity);
        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse v1 order detail for {}: {}", order_id, e.what());
        return false;
    }
}

bool BithumbExchange::query_order_detail_legacy(const std::string& order_id,
                                                const SymbolId& symbol,
                                                Order& order) {
    static constexpr std::array<int, 5> retry_delays_ms{0, 80, 160, 320, 640};

    std::string endpoint = "/info/order_detail";
    std::ostringstream params;
    params << "order_id=" << order_id;
    params << "&order_currency=" << symbol.get_base();
    params << "&payment_currency=" << symbol.get_quote();
    const std::string params_str = params.str();

    for (std::size_t attempt = 0; attempt < retry_delays_ms.size(); ++attempt) {
        if (retry_delays_ms[attempt] > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delays_ms[attempt]));
        }

        auto headers = build_auth_headers(endpoint, params_str);
        headers["Content-Type"] = "application/x-www-form-urlencoded";

        auto response = rest_client_->post(endpoint, params_str, headers);
        if (!response.success) {
            Logger::debug("[Bithumb] Legacy order detail query failed for {}: {}", order_id, response.error);
            continue;
        }

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);

            std::string_view status = doc["status"].get_string().value();
            if (status != "0000") {
                continue;
            }

            auto data = doc["data"];
            double total_cost = 0.0;
            double total_units = 0.0;

            auto contracts = data["contract"];
            if (!contracts.error()) {
                for (auto c : contracts.get_array()) {
                    auto price_field = c["price"];
                    auto units_field = c["units"];
                    if (price_field.error() || units_field.error()) continue;

                    double price = opt::fast_stod(price_field.get_string().value());
                    double units = opt::fast_stod(units_field.get_string().value());
                    total_cost += price * units;
                    total_units += units;
                }
            }

            if (total_units > 0.0) {
                order.filled_quantity = total_units;
                order.average_price = total_cost / total_units;
                Logger::info("[Bithumb] Fill(legacy): orderId={}, avgPrice={:.2f}, filledQty={:.8f}",
                             order_id, order.average_price, order.filled_quantity);
                return true;
            }
        } catch (const simdjson::simdjson_error& e) {
            Logger::debug("[Bithumb] Failed to parse legacy order detail for {}: {}", order_id, e.what());
        }
    }

    return false;
}

bool BithumbExchange::query_order_detail(const std::string& order_id, const SymbolId& symbol, Order& order) {
    if (query_order_detail_ws(order_id, order)) {
        return true;
    }

    static constexpr std::array<int, 7> v1_fast_delays_ms{0, 15, 25, 40, 60, 90, 140};

    for (std::size_t attempt = 0; attempt < v1_fast_delays_ms.size(); ++attempt) {
        if (v1_fast_delays_ms[attempt] > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(v1_fast_delays_ms[attempt]));
        }
        if (query_order_detail_v1(order_id, order)) {
            return true;
        }
    }

    Logger::debug("[Bithumb] v1 fill lookup missed for {}, falling back to legacy order_detail", order_id);
    if (query_order_detail_legacy(order_id, symbol, order)) {
        return true;
    }

    Logger::error("[Bithumb] Failed to get fill price via v1+legacy paths for order {}", order_id);
    return false;
}

} // namespace kimp::exchange::bithumb
