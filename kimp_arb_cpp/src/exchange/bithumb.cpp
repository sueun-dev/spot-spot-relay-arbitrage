#include "kimp/exchange/bithumb/bithumb.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>

namespace kimp::exchange::bithumb {

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

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType type) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Bithumb] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& reason) {
        on_ws_disconnected();
    });

    ws_client_->connect(credentials_.ws_endpoint);
    return true;
}

void BithumbExchange::disconnect() {
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

    // Format: {"type":"ticker","symbols":["BTC_KRW","ETH_KRW"],"tickTypes":["MID"]}
    std::ostringstream ss;
    ss << R"({"type":"ticker","symbols":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << sym.to_bithumb_format() << "\"";
        first = false;
    }

    ss << R"(],"tickTypes":["MID"]})";

    Logger::debug("[Bithumb] Subscribing to {} symbols", symbols.size());
    ws_client_->send(ss.str());
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

    std::ostringstream ss;
    ss << R"({"type":"orderbookdepth","symbols":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << sym.to_bithumb_format() << "\"";
        first = false;
    }

    ss << R"(]})";

    Logger::info("[Bithumb] Subscribing to {} orderbook depth streams", symbols.size());
    ws_client_->send(ss.str());
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
                    std::string symbol_key = std::string(key) + "_KRW";
                    {
                        std::lock_guard lock(orderbook_mutex_);
                        auto bbo_it = orderbook_bbo_.find(symbol_key);
                        if (bbo_it != orderbook_bbo_.end()) {
                            double real_bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
                            double real_ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
                            if (real_bid > 0.0) ticker.bid = real_bid;
                            if (real_ask > 0.0) ticker.ask = real_ask;
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

void BithumbExchange::fetch_all_orderbook_snapshots(const std::vector<SymbolId>& symbols) {
    auto response = rest_client_->get("/public/orderbook/ALL_KRW?count=5");
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

        auto data = doc["data"].get_object();
        int count = 0;

        std::lock_guard lock(orderbook_mutex_);

        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key == "timestamp" || key == "payment_currency") continue;

            std::string symbol_key = std::string(key) + "_KRW";

            auto& state = orderbook_state_[symbol_key];
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
            update_bbo(symbol_key);
            ++count;
        }

        orderbook_ready_.store(true, std::memory_order_release);
        Logger::info("[Bithumb] Loaded orderbook snapshots for {} symbols", count);
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

    std::string endpoint = "/trade/market_" + std::string(side == Side::Buy ? "buy" : "sell");

    std::ostringstream params;
    params << "order_currency=" << symbol.get_base();
    params << "&payment_currency=KRW";

    if (side == Side::Sell) {
        params << "&units=" << std::fixed << std::setprecision(8) << quantity;
    } else {
        // For buy, quantity is KRW amount
        params << "&units=" << std::fixed << std::setprecision(0) << quantity;
    }

    auto headers = build_auth_headers(endpoint, params.str());
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto response = rest_client_->post(endpoint, params.str(), headers);
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

    std::string endpoint = "/trade/market_buy";

    std::ostringstream params;
    params << "order_currency=" << symbol.get_base();
    params << "&payment_currency=KRW";
    params << "&units=" << std::fixed << std::setprecision(8) << quantity;

    auto headers = build_auth_headers(endpoint, params.str());
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto response = rest_client_->post(endpoint, params.str(), headers);
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

bool BithumbExchange::cancel_order(uint64_t order_id) {
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
        return 0.0;
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

    return 0.0;
}

void BithumbExchange::on_ws_message(std::string_view message) {
    // Fast message type routing via string search (avoids double JSON parse)
    if (message.find("orderbookdepth") != std::string_view::npos) {
        auto updated_symbols = parse_orderbookdepth_message(message);

        // Dispatch synthetic tickers for BBO-changed symbols (0ms propagation)
        if (orderbook_ready_.load(std::memory_order_acquire)) {
            for (const auto& symbol_key : updated_symbols) {
                double bid = 0.0;
                double ask = 0.0;
                double last = 0.0;
                {
                    std::lock_guard lock(orderbook_mutex_);
                    auto bbo_it = orderbook_bbo_.find(symbol_key);
                    if (bbo_it == orderbook_bbo_.end()) continue;

                    bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
                    ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
                    if (bid <= 0.0 || ask <= 0.0) continue;

                    auto last_it = last_price_cache_.find(symbol_key);
                    if (last_it == last_price_cache_.end()) continue;
                    last = last_it->second;
                }

                Ticker ticker;
                ticker.exchange = Exchange::Bithumb;
                ticker.timestamp = std::chrono::steady_clock::now();
                auto pos = symbol_key.find('_');
                if (pos != std::string::npos) {
                    ticker.symbol.set_base(symbol_key.substr(0, pos));
                    ticker.symbol.set_quote(symbol_key.substr(pos + 1));
                }
                ticker.last = last;
                ticker.bid = bid;
                ticker.ask = ask;
                dispatch_ticker(ticker);
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
            std::string symbol_key = ticker.symbol.to_bithumb_format();
            std::lock_guard lock(orderbook_mutex_);
            auto bbo_it = orderbook_bbo_.find(symbol_key);
            if (bbo_it != orderbook_bbo_.end()) {
                double real_bid = bbo_it->second.best_bid.load(std::memory_order_acquire);
                double real_ask = bbo_it->second.best_ask.load(std::memory_order_acquire);
                if (real_bid > 0.0) ticker.bid = real_bid;
                if (real_ask > 0.0) ticker.ask = real_ask;
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

bool BithumbExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
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
            last_price_cache_[std::string(symbol_str)] = ticker.last;
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse ticker: {}", e.what());
        return false;
    }
}

std::vector<std::string> BithumbExchange::parse_orderbookdepth_message(std::string_view message) {
    std::vector<std::string> updated_symbols;
    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        std::string_view type = doc["type"].get_string().value();
        if (type != "orderbookdepth") return updated_symbols;

        auto content = doc["content"];
        auto list = content["list"].get_array();

        std::lock_guard lock(orderbook_mutex_);

        std::string last_updated_symbol;

        for (auto item : list) {
            std::string_view symbol_sv = item["symbol"].get_string().value();
            std::string symbol_key(symbol_sv);

            auto it = orderbook_state_.find(symbol_key);
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

            if (symbol_key != last_updated_symbol) {
                if (!last_updated_symbol.empty()) {
                    update_bbo(last_updated_symbol);
                    updated_symbols.push_back(last_updated_symbol);
                }
                last_updated_symbol = symbol_key;
            }
        }

        if (!last_updated_symbol.empty()) {
            update_bbo(last_updated_symbol);
            updated_symbols.push_back(last_updated_symbol);
        }

    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse orderbookdepth: {}", e.what());
    }
    return updated_symbols;
}

void BithumbExchange::update_bbo(const std::string& symbol_key) {
    // Called with orderbook_mutex_ held
    auto state_it = orderbook_state_.find(symbol_key);
    if (state_it == orderbook_state_.end() || !state_it->second.initialized) return;

    const auto& state = state_it->second;
    auto& bbo = orderbook_bbo_[symbol_key];

    if (!state.bids.empty()) {
        bbo.best_bid.store(state.bids.begin()->first, std::memory_order_release);
    }
    if (!state.asks.empty()) {
        bbo.best_ask.store(state.asks.begin()->first, std::memory_order_release);
    }
}

bool BithumbExchange::query_order_detail(const std::string& order_id, const SymbolId& symbol, Order& order) {
    constexpr int MAX_RETRIES = 5;
    constexpr int BASE_DELAY_MS = 300;  // 300, 600, 1200, 2400ms backoff

    std::string endpoint = "/info/order_detail";
    std::ostringstream params;
    params << "order_id=" << order_id;
    params << "&order_currency=" << symbol.get_base();
    params << "&payment_currency=" << symbol.get_quote();
    const std::string params_str = params.str();

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        if (attempt > 0) {
            int delay = BASE_DELAY_MS * (1 << (attempt - 1));  // exponential backoff
            Logger::warn("[Bithumb] Fill query retry {}/{} for order {} (wait {}ms)",
                         attempt + 1, MAX_RETRIES, order_id, delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        auto headers = build_auth_headers(endpoint, params_str);
        headers["Content-Type"] = "application/x-www-form-urlencoded";

        auto response = rest_client_->post(endpoint, params_str, headers);
        if (!response.success) {
            Logger::warn("[Bithumb] Failed to query order detail: {}", response.error);
            continue;
        }

        Logger::debug("[Bithumb] Order detail raw: {}", response.body);

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);

            std::string_view status = doc["status"].get_string().value();
            if (status != "0000") {
                Logger::warn("[Bithumb] Order detail query status: {}", std::string(status));
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

                    std::string_view price_str = price_field.get_string().value();
                    std::string_view units_str = units_field.get_string().value();
                    double price = opt::fast_stod(price_str);
                    double units = opt::fast_stod(units_str);
                    total_cost += price * units;
                    total_units += units;
                }
            }

            if (total_units > 0) {
                order.filled_quantity = total_units;
                order.average_price = total_cost / total_units;
                Logger::info("[Bithumb] Fill: orderId={}, avgPrice={:.2f}, filledQty={:.8f}",
                             order_id, order.average_price, order.filled_quantity);
                return true;
            }

            // Contract array empty — order may not be settled yet
            Logger::warn("[Bithumb] Order {} contract empty, attempt {}/{}",
                         order_id, attempt + 1, MAX_RETRIES);
        } catch (const simdjson::simdjson_error& e) {
            Logger::warn("[Bithumb] Failed to parse order detail: {}", e.what());
        }
    }

    Logger::error("[Bithumb] Failed to get fill price after {} retries for order {}",
                  MAX_RETRIES, order_id);
    return false;
}

} // namespace kimp::exchange::bithumb
