#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <limits>
#include <cmath>

namespace {
bool use_hedge_mode() {
    static int cached = -1;
    if (cached != -1) {
        return cached == 1;
    }
    const char* env = std::getenv("BYBIT_HEDGE_MODE");
    if (!env || env[0] == '\0') {
        cached = 0;
        return false;
    }
    char c = env[0];
    cached = (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' || c == 'o' || c == 'O') ? 1 : 0;
    return cached == 1;
}
} // namespace

namespace kimp::exchange::bybit {

bool BybitExchange::connect() {
    if (connected_) {
        Logger::warn("[Bybit] Already connected");
        return true;
    }

    Logger::info("[Bybit] Connecting...");

    // Initialize REST connection pool first (pre-establish SSL connections)
    if (!initialize_rest()) {
        Logger::error("[Bybit] Failed to initialize REST connection pool");
        return false;
    }
    Logger::info("[Bybit] REST connection pool initialized (4 persistent connections)");

    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "Bybit-WS");

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType type) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Bybit] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& reason) {
        on_ws_disconnected();
    });

    ws_client_->connect(credentials_.ws_endpoint);
    return true;
}

void BybitExchange::disconnect() {
    // Shutdown REST connection pool
    shutdown_rest();

    if (ws_client_) {
        ws_client_->disconnect();
    }
    connected_ = false;
    Logger::info("[Bybit] Disconnected");
}

void BybitExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    // Store symbols for reconnection
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_tickers_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[Bybit] Cannot subscribe, not connected");
        return;
    }

    // Format: {"op":"subscribe","args":["tickers.BTCUSDT","tickers.ETHUSDT"]}
    std::ostringstream ss;
    ss << R"({"op":"subscribe","args":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"tickers." << symbol_to_bybit(sym) << "\"";
        first = false;
    }

    ss << "]}";

    Logger::debug("[Bybit] Subscribing to {} symbols", symbols.size());
    ws_client_->send(ss.str());
}

void BybitExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    if (!ws_client_ || !ws_client_->is_connected()) return;

    std::ostringstream ss;
    ss << R"({"op":"subscribe","args":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"orderbook.1." << symbol_to_bybit(sym) << "\"";
        first = false;
    }

    ss << "]}";
    ws_client_->send(ss.str());
}

std::vector<SymbolId> BybitExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/v5/market/instruments-info?category=linear&limit=1000");
    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch markets: {}", response.error);
        return symbols;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto item : list) {
            std::string_view symbol_str = item["symbol"].get_string().value();
            std::string_view quote = item["quoteCoin"].get_string().value();
            std::string_view settle = item["settleCoin"].get_string().value();

            // Only USDT perpetuals
            if (quote == "USDT" && settle == "USDT") {
                std::string_view base = item["baseCoin"].get_string().value();
                symbols.emplace_back(std::string(base), "USDT");

                // Cache funding interval (in minutes -> hours)
                auto funding_interval = item["fundingInterval"];
                if (!funding_interval.error()) {
                    int interval_minutes = static_cast<int>(funding_interval.get_int64().value());
                    int interval_hours = interval_minutes / 60;
                    if (interval_hours == 0) interval_hours = 8;  // Default for delivery contracts
                    std::unique_lock lock(metadata_mutex_);
                    funding_interval_cache_[std::string(symbol_str)] = interval_hours;
                }

                // Cache lot size filters
                auto lot = item["lotSizeFilter"];
                if (!lot.error()) {
                    LotSize info;
                    auto min_qty = lot["minOrderQty"];
                    if (!min_qty.error()) {
                        info.min_qty = opt::fast_stod(min_qty.get_string().value());
                    }
                    auto step = lot["qtyStep"];
                    if (!step.error()) {
                        info.qty_step = opt::fast_stod(step.get_string().value());
                    }
                    auto min_amt = lot["minOrderAmt"];
                    if (!min_amt.error()) {
                        info.min_notional = opt::fast_stod(min_amt.get_string().value());
                    }
                    auto min_notional = lot["minNotionalValue"];
                    if (!min_notional.error()) {
                        info.min_notional = std::max(info.min_notional,
                                                     opt::fast_stod(min_notional.get_string().value()));
                    }
                    std::unique_lock lock(metadata_mutex_);
                    lot_size_cache_[std::string(symbol_str)] = info;
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse markets: {}", e.what());
    }

    Logger::info("[Bybit] Found {} USDT perpetual markets, cached {} funding intervals",
                 symbols.size(), funding_interval_cache_.size());
    return symbols;
}

std::vector<Ticker> BybitExchange::fetch_all_tickers() {
    std::vector<Ticker> tickers;

    auto response = rest_client_->get("/v5/market/tickers?category=linear");
    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch all tickers: {}", response.error);
        return tickers;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto item : list) {
            std::string_view symbol_str = item["symbol"].get_string().value();

            // Only USDT pairs
            if (symbol_str.size() < 5 || symbol_str.substr(symbol_str.size() - 4) != "USDT") {
                continue;
            }

            // Extract base currency (remove USDT suffix)
            std::string base(symbol_str.substr(0, symbol_str.size() - 4));

            Ticker ticker;
            ticker.exchange = Exchange::Bybit;
            ticker.symbol = SymbolId(base, "USDT");

            // Parse prices
            std::string_view last_str = item["lastPrice"].get_string().value();
            ticker.last = opt::fast_stod(last_str);

            std::string_view bid_str = item["bid1Price"].get_string().value();
            ticker.bid = opt::fast_stod(bid_str);

            std::string_view ask_str = item["ask1Price"].get_string().value();
            ticker.ask = opt::fast_stod(ask_str);

            // Parse funding rate
            auto funding_rate_result = item["fundingRate"];
            if (!funding_rate_result.error()) {
                std::string_view fr_str = funding_rate_result.get_string().value();
                ticker.funding_rate = opt::fast_stod(fr_str);
            }

            auto next_funding_result = item["nextFundingTime"];
            if (!next_funding_result.error()) {
                std::string_view nft_str = next_funding_result.get_string().value();
                ticker.next_funding_time = std::stoull(std::string(nft_str));
            }

            // Use cached funding interval, default to 8h if not found
            {
                std::shared_lock lock(metadata_mutex_);
                auto it = funding_interval_cache_.find(std::string(symbol_str));
                ticker.funding_interval_hours = (it != funding_interval_cache_.end()) ? it->second : 8;
            }
            ticker.timestamp = std::chrono::steady_clock::now();

            if (ticker.last > 0) {
                tickers.push_back(ticker);
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse all tickers: {}", e.what());
    }

    Logger::info("[Bybit] Fetched {} tickers via REST", tickers.size());
    return tickers;
}

double BybitExchange::get_funding_rate(const SymbolId& symbol) {
    std::string bybit_symbol = symbol_to_bybit(symbol);
    std::string endpoint = "/v5/market/funding/history?category=linear&symbol=" + bybit_symbol + "&limit=1";

    auto response = rest_client_->get(endpoint);
    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch funding rate: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto item : list) {
            std::string_view rate_str = item["fundingRate"].get_string().value();
            return opt::fast_stod(rate_str);
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse funding rate: {}", e.what());
    }

    return 0.0;
}

Order BybitExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":")" << (side == Side::Buy ? "Buy" : "Sell") << R"(",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << quantity << R"(",)";
    body << R"("timeInForce":"GTC"})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/order/create", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Order failed: {}", response.body);
        return order;
    }

    std::string order_id_str;
    parse_order_response(response.body, order, &order_id_str);
    if (order.status == OrderStatus::Filled && !order_id_str.empty()) {
        query_order_fill(order_id_str, order);
    }
    return order;
}

Order BybitExchange::open_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.symbol = symbol;
    order.side = Side::Sell;
    order.type = OrderType::Market;
    double adj_qty = normalize_order_qty(symbol, quantity, true);
    if (adj_qty <= 0.0) {
        order.status = OrderStatus::Rejected;
        Logger::warn("[Bybit] Short open qty invalid after lot size check: {} {}", symbol.to_string(), quantity);
        return order;
    }
    order.quantity = adj_qty;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Sell",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << adj_qty << R"(",)";
    if (use_hedge_mode()) {
        body << R"("positionIdx":)" << SHORT_POSITION_IDX << R"(,)";
    }
    body << R"("reduceOnly":false})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/order/create", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Short open failed: {}", response.body);
        return order;
    }

    std::string order_id_str;
    parse_order_response(response.body, order, &order_id_str);
    if (order.status == OrderStatus::Filled && !order_id_str.empty()) {
        query_order_fill(order_id_str, order);
    }
    Logger::info("[Bybit] Opened short {} {} - Status: {}, avgPrice: {:.8f}, filledQty: {:.8f}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.average_price, order.filled_quantity);
    return order;
}

Order BybitExchange::close_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    double adj_qty = normalize_order_qty(symbol, quantity, false);
    if (adj_qty <= 0.0) {
        order.status = OrderStatus::Rejected;
        Logger::warn("[Bybit] Short close qty invalid after lot size check: {} {}", symbol.to_string(), quantity);
        return order;
    }
    order.quantity = adj_qty;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Buy",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << adj_qty << R"(",)";
    if (use_hedge_mode()) {
        body << R"("positionIdx":)" << SHORT_POSITION_IDX << R"(,)";
    }
    body << R"("reduceOnly":true})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/order/create", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Short close failed: {}", response.body);
        return order;
    }

    std::string order_id_str;
    parse_order_response(response.body, order, &order_id_str);
    if (order.status == OrderStatus::Filled && !order_id_str.empty()) {
        query_order_fill(order_id_str, order);
    }
    Logger::info("[Bybit] Closed short {} {} - Status: {}, avgPrice: {:.8f}, filledQty: {:.8f}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.average_price, order.filled_quantity);
    return order;
}

double BybitExchange::normalize_order_qty(const SymbolId& symbol, double qty, bool is_open) const {
    if (qty <= 0.0) return 0.0;
    const std::string key = symbol_to_bybit(symbol);
    double step = 0.0;
    double min_qty = 0.0;
    double min_notional = 0.0;
    {
        std::shared_lock lock(metadata_mutex_);
        auto it = lot_size_cache_.find(key);
        if (it == lot_size_cache_.end()) {
            return qty;
        }
        step = it->second.qty_step > 0.0 ? it->second.qty_step : 0.0;
        min_qty = it->second.min_qty;
        min_notional = it->second.min_notional;
    }

    if (step > 0.0) {
        qty = std::floor(qty / step) * step;
    }
    if (qty < min_qty) {
        return 0.0;
    }

    double price_hint = 0.0;
    auto cached = get_cached_ticker(symbol);
    if (cached) {
        if (is_open) {
            price_hint = cached->bid > 0 ? cached->bid : cached->last;
        } else {
            price_hint = cached->ask > 0 ? cached->ask : cached->last;
        }
    }
    if (min_notional > 0.0 && price_hint > 0.0 && qty * price_hint < min_notional) {
        return 0.0;
    }

    return qty;
}

bool BybitExchange::cancel_order(uint64_t order_id) {
    // TODO: Implement if needed
    return false;
}

bool BybitExchange::set_leverage(const SymbolId& symbol, int leverage) {
    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("buyLeverage":")" << leverage << R"(",)";
    body << R"("sellLeverage":")" << leverage << R"("})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/position/set-leverage", body_str, headers);
    if (!response.success) {
        // Leverage might already be set, check if it's a "same leverage" error
        if (response.body.find("110043") != std::string::npos) {
            return true;  // Already at this leverage
        }
        Logger::error("[Bybit] Set leverage failed: {}", response.body);
        return false;
    }

    return true;
}

std::vector<Position> BybitExchange::get_positions() {
    std::vector<Position> positions;

    std::string query = "category=linear&settleCoin=USDT";
    auto headers = build_auth_headers(query);
    auto response = rest_client_->get("/v5/position/list?" + query, headers);

    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch positions: {}", response.error);
        return positions;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto item : list) {
            std::string_view size_str = item["size"].get_string().value();
            double size = opt::fast_stod(size_str);

            if (size > 0) {
                Position pos;

                std::string_view symbol_str = item["symbol"].get_string().value();
                // Parse symbol (e.g., "BTCUSDT" -> base="BTC", quote="USDT")
                if (symbol_str.size() > 4) {
                    std::string base(symbol_str.substr(0, symbol_str.size() - 4));
                    pos.symbol = SymbolId(base, "USDT");
                }

                pos.foreign_exchange = Exchange::Bybit;
                pos.foreign_amount = size;
                pos.is_active = true;

                auto avg_price_field = item["avgPrice"];
                if (!avg_price_field.error()) {
                    std::string_view avg_price_str = avg_price_field.get_string().value();
                    pos.foreign_entry_price = opt::fast_stod(avg_price_str);
                }

                std::string_view side = item["side"].get_string().value();
                if (side == "Sell") {
                    positions.push_back(pos);  // Only short positions for our strategy
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse positions: {}", e.what());
    }

    return positions;
}

bool BybitExchange::close_position(const SymbolId& symbol) {
    auto positions = get_positions();

    for (const auto& pos : positions) {
        if (pos.symbol == symbol && pos.foreign_amount > 0) {
            auto order = close_short(symbol, pos.foreign_amount);
            return order.status == OrderStatus::Filled;
        }
    }

    return true;  // No position to close
}

double BybitExchange::get_balance(const std::string& currency) {
    std::string query = "accountType=UNIFIED";
    auto headers = build_auth_headers(query);
    auto response = rest_client_->get("/v5/account/wallet-balance?" + query, headers);

    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch balance: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto list = doc["result"]["list"].get_array();

        for (auto account : list) {
            auto coins = account["coin"].get_array();
            for (auto coin : coins) {
                std::string_view coin_name = coin["coin"].get_string().value();
                if (coin_name == currency) {
                    std::string_view balance_str = coin["availableToWithdraw"].get_string().value();
                    return opt::fast_stod(balance_str);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse balance: {}", e.what());
    }

    return 0.0;
}

void BybitExchange::on_ws_message(std::string_view message) {
    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        dispatch_ticker(ticker);
    }
}

void BybitExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[Bybit] WebSocket connected");

    // Resubscribe to tickers after reconnection
    std::vector<SymbolId> symbols_to_subscribe;
    {
        std::lock_guard lock(subscription_mutex_);
        symbols_to_subscribe = subscribed_tickers_;
    }

    if (!symbols_to_subscribe.empty()) {
        Logger::info("[Bybit] Resubscribing to {} tickers after reconnection", symbols_to_subscribe.size());
        subscribe_ticker(symbols_to_subscribe);
    }
}

void BybitExchange::on_ws_disconnected() {
    connected_ = false;
    Logger::warn("[Bybit] WebSocket disconnected");
}

std::string BybitExchange::generate_signature(int64_t timestamp, const std::string& params) const {
    std::string recv_window = "5000";
    std::string message = std::to_string(timestamp) + credentials_.api_key + recv_window + params;
    return utils::Crypto::hmac_sha256(credentials_.secret_key, message);
}

std::unordered_map<std::string, std::string> BybitExchange::build_auth_headers(
    const std::string& params) const {

    int64_t timestamp = utils::Crypto::timestamp_ms();
    std::string signature = generate_signature(timestamp, params);

    return {
        {"X-BAPI-API-KEY", credentials_.api_key},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", std::to_string(timestamp)},
        {"X-BAPI-RECV-WINDOW", "5000"}
    };
}

bool BybitExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    try {
        // Use padded_string to satisfy simdjson's padding requirement
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        auto topic = doc["topic"];
        if (topic.error()) return false;

        std::string_view topic_str = topic.get_string().value();
        if (topic_str.substr(0, 8) != "tickers.") return false;

        auto data = doc["data"];
        if (data.error()) return false;

        ticker.exchange = Exchange::Bybit;
        ticker.timestamp = std::chrono::steady_clock::now();
        ticker.funding_rate = std::numeric_limits<double>::quiet_NaN();
        ticker.next_funding_time = 0;

        std::string_view symbol_str = data["symbol"].get_string().value();
        if (symbol_str.size() > 4) {
            std::string base(symbol_str.substr(0, symbol_str.size() - 4));
            ticker.symbol = SymbolId(base, "USDT");
        }

        std::string_view last_str = data["lastPrice"].get_string().value();
        ticker.last = opt::fast_stod(last_str);

        std::string_view bid_str = data["bid1Price"].get_string().value();
        ticker.bid = opt::fast_stod(bid_str);

        std::string_view ask_str = data["ask1Price"].get_string().value();
        ticker.ask = opt::fast_stod(ask_str);

        // Parse funding rate info
        auto funding_rate_result = data["fundingRate"];
        if (!funding_rate_result.error()) {
            std::string_view fr_str = funding_rate_result.get_string().value();
            ticker.funding_rate = opt::fast_stod(fr_str);
        }

        auto next_funding_result = data["nextFundingTime"];
        if (!next_funding_result.error()) {
            std::string_view nft_str = next_funding_result.get_string().value();
            ticker.next_funding_time = std::stoull(std::string(nft_str));
        }

        // Use cached funding interval, default to 8h if not found
        std::string bybit_symbol = std::string(ticker.symbol.get_base()) + "USDT";
        {
            std::shared_lock lock(metadata_mutex_);
            auto it = funding_interval_cache_.find(bybit_symbol);
            ticker.funding_interval_hours = (it != funding_interval_cache_.end()) ? it->second : 8;
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        return false;
    }
}

bool BybitExchange::parse_order_response(const std::string& response, Order& order, std::string* order_id_out) {
    Logger::debug("[Bybit] Order raw response: {}", response);

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded_response(response);
        auto doc = local_parser.iterate(padded_response);

        int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
        if (ret_code == 0) {
            order.status = OrderStatus::Filled;  // Assume filled for market orders

            auto result = doc["result"];
            std::string_view order_id = result["orderId"].get_string().value();
            order.exchange_order_id = std::hash<std::string_view>{}(order_id);

            if (order_id_out) {
                *order_id_out = std::string(order_id);
            }
        } else {
            order.status = OrderStatus::Rejected;
            std::string_view msg = doc["retMsg"].get_string().value();
            Logger::error("[Bybit] Order rejected: {}", msg);
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse order response: {}", e.what());
        order.status = OrderStatus::Rejected;
        return false;
    }
}

bool BybitExchange::query_order_fill(const std::string& order_id, Order& order) {
    constexpr int MAX_RETRIES = 5;
    constexpr int BASE_DELAY_MS = 300;  // 300, 600, 1200, 2400ms backoff

    std::string query = "category=linear&orderId=" + order_id;
    std::string endpoint = "/v5/order/realtime?" + query;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        if (attempt > 0) {
            int delay = BASE_DELAY_MS * (1 << (attempt - 1));  // exponential backoff
            Logger::warn("[Bybit] Fill query retry {}/{} for order {} (wait {}ms)",
                         attempt + 1, MAX_RETRIES, order_id, delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        auto headers = build_auth_headers(query);
        auto response = rest_client_->get(endpoint, headers);

        if (!response.success) {
            Logger::warn("[Bybit] Failed to query order fill: {}", response.error);
            continue;
        }

        Logger::debug("[Bybit] Fill query raw: {}", response.body);

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);

            int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
            if (ret_code != 0) {
                Logger::warn("[Bybit] Order fill query error, retCode: {}", ret_code);
                continue;
            }

            auto list = doc["result"]["list"].get_array();
            for (auto item : list) {
                auto avg_price = item["avgPrice"];
                if (!avg_price.error()) {
                    std::string_view p = avg_price.get_string().value();
                    double price = opt::fast_stod(p);
                    if (price > 0) order.average_price = price;
                }
                auto cum_qty = item["cumExecQty"];
                if (!cum_qty.error()) {
                    std::string_view q = cum_qty.get_string().value();
                    double qty = opt::fast_stod(q);
                    if (qty > 0) order.filled_quantity = qty;
                }

                if (order.average_price > 0 && order.filled_quantity > 0) {
                    Logger::info("[Bybit] Fill: orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                                 order_id, order.average_price, order.filled_quantity);
                    return true;
                }
                break;  // Only check first result
            }

            // List empty or avgPrice=0 — order may not be settled yet
            Logger::warn("[Bybit] Order {} fill data incomplete, attempt {}/{}",
                         order_id, attempt + 1, MAX_RETRIES);
        } catch (const simdjson::simdjson_error& e) {
            Logger::warn("[Bybit] Failed to parse order fill: {}", e.what());
        }
    }

    Logger::error("[Bybit] Failed to get fill price after {} retries for order {}",
                  MAX_RETRIES, order_id);
    return false;
}

} // namespace kimp::exchange::bybit
