#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>

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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
                    funding_interval_cache_[std::string(symbol_str)] = interval_hours;
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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
            auto it = funding_interval_cache_.find(std::string(symbol_str));
            ticker.funding_interval_hours = (it != funding_interval_cache_.end()) ? it->second : 8;
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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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

    parse_order_response(response.body, order);
    return order;
}

Order BybitExchange::open_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.symbol = symbol;
    order.side = Side::Sell;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Sell",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << quantity << R"(",)";
    body << R"("positionIdx":)" << SHORT_POSITION_IDX << R"(,)";
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

    parse_order_response(response.body, order);
    Logger::info("[Bybit] Opened short {} {} - Status: {}",
                 symbol.to_string(), quantity, static_cast<int>(order.status));
    return order;
}

Order BybitExchange::close_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Bybit;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::ostringstream body;
    body << R"({"category":"linear",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Buy",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << quantity << R"(",)";
    body << R"("positionIdx":)" << SHORT_POSITION_IDX << R"(,)";
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

    parse_order_response(response.body, order);
    Logger::info("[Bybit] Closed short {} {} - Status: {}",
                 symbol.to_string(), quantity, static_cast<int>(order.status));
    return order;
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

    auto headers = build_auth_headers();
    auto response = rest_client_->get("/v5/position/list?category=linear&settleCoin=USDT", headers);

    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch positions: {}", response.error);
        return positions;
    }

    try {
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
    auto headers = build_auth_headers();
    auto response = rest_client_->get("/v5/account/wallet-balance?accountType=UNIFIED", headers);

    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch balance: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
        simdjson::padded_string padded(message);
        auto doc = json_parser_.iterate(padded);

        auto topic = doc["topic"];
        if (topic.error()) return false;

        std::string_view topic_str = topic.get_string().value();
        if (topic_str.substr(0, 8) != "tickers.") return false;

        auto data = doc["data"];
        if (data.error()) return false;

        ticker.exchange = Exchange::Bybit;
        ticker.timestamp = std::chrono::steady_clock::now();

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
        auto it = funding_interval_cache_.find(bybit_symbol);
        ticker.funding_interval_hours = (it != funding_interval_cache_.end()) ? it->second : 8;

        return true;
    } catch (const simdjson::simdjson_error& e) {
        return false;
    }
}

bool BybitExchange::parse_order_response(const std::string& response, Order& order) {
    try {
        // Use padded_string to satisfy simdjson's padding requirement
        simdjson::padded_string padded_response(response);
        auto doc = json_parser_.iterate(padded_response);

        int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
        if (ret_code == 0) {
            order.status = OrderStatus::Filled;  // Assume filled for market orders

            auto result = doc["result"];
            std::string_view order_id = result["orderId"].get_string().value();
            order.exchange_order_id = std::hash<std::string_view>{}(order_id);
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

} // namespace kimp::exchange::bybit
