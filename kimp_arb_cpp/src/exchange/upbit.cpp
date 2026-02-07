#include "kimp/exchange/upbit/upbit.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>

namespace kimp::exchange::upbit {

bool UpbitExchange::connect() {
    if (connected_) {
        Logger::warn("[Upbit] Already connected");
        return true;
    }

    Logger::info("[Upbit] Connecting...");

    // Initialize REST connection pool first (pre-establish SSL connections)
    if (!initialize_rest()) {
        Logger::error("[Upbit] Failed to initialize REST connection pool");
        return false;
    }
    Logger::info("[Upbit] REST connection pool initialized (4 persistent connections)");

    // Create WebSocket client
    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "Upbit-WS");

    // Set callbacks
    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType type) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Upbit] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& reason) {
        on_ws_disconnected();
    });

    ws_client_->set_error_callback([](const std::string& error) {
        Logger::error("[Upbit] WebSocket error: {}", error);
    });

    // Connect
    ws_client_->connect(credentials_.ws_endpoint);

    return true;
}

void UpbitExchange::disconnect() {
    // Shutdown REST connection pool
    shutdown_rest();

    if (ws_client_) {
        ws_client_->disconnect();
    }
    connected_ = false;
    Logger::info("[Upbit] Disconnected");
}

void UpbitExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[Upbit] Cannot subscribe, not connected");
        return;
    }

    // Build subscription message
    // Format: [{"ticket":"unique-id"},{"type":"ticker","codes":["KRW-BTC","KRW-ETH"]},{"format":"SIMPLE"}]
    std::ostringstream ss;
    ss << R"([{"ticket":")" << utils::Crypto::generate_uuid() << R"("},{"type":"ticker","codes":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << sym.to_upbit_format() << "\"";
        first = false;
    }

    ss << R"(]},{"format":"SIMPLE"}])";

    std::string msg = ss.str();
    Logger::debug("[Upbit] Subscribing to {} symbols", symbols.size());
    ws_client_->send(msg);
}

void UpbitExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[Upbit] Cannot subscribe, not connected");
        return;
    }

    std::ostringstream ss;
    ss << R"([{"ticket":")" << utils::Crypto::generate_uuid() << R"("},{"type":"orderbook","codes":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << sym.to_upbit_format() << "\"";
        first = false;
    }

    ss << R"(]},{"format":"SIMPLE"}])";

    ws_client_->send(ss.str());
}

std::vector<SymbolId> UpbitExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/v1/market/all?isDetails=false");
    if (!response.success) {
        Logger::error("[Upbit] Failed to fetch markets: {}", response.error);
        return symbols;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        for (auto market : doc.get_array()) {
            std::string_view market_str = market["market"].get_string().value();

            // Only KRW markets
            if (market_str.substr(0, 4) == "KRW-") {
                std::string base(market_str.substr(4));
                symbols.emplace_back(base, "KRW");
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse markets: {}", e.what());
    }

    Logger::info("[Upbit] Found {} KRW markets", symbols.size());
    return symbols;
}

double UpbitExchange::get_usdt_krw_price() {
    // Check cached value
    double cached = usdt_krw_price_.load();
    if (cached > 0) {
        return cached;
    }

    // Fetch from REST API
    auto response = rest_client_->get("/v1/ticker?markets=KRW-USDT");
    if (!response.success) {
        Logger::error("[Upbit] Failed to fetch USDT price: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto arr = doc.get_array();

        for (auto ticker : arr) {
            double price = ticker["trade_price"].get_double().value();
            usdt_krw_price_.store(price);
            return price;
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse USDT price: {}", e.what());
    }

    return 0.0;
}

Order UpbitExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    Order order;
    order.exchange = Exchange::Upbit;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::string market = symbol.to_upbit_format();
    std::string side_str = (side == Side::Buy) ? "bid" : "ask";

    std::ostringstream body;
    body << R"({"market":")" << market << R"(",)";
    body << R"("side":")" << side_str << R"(",)";
    body << R"("ord_type":"market",)";

    if (side == Side::Sell) {
        body << R"("volume":")" << std::fixed << std::setprecision(8) << quantity << R"("})";
    } else {
        // For market buy, Upbit needs price (cost in KRW)
        // This method should be called with quantity = cost for buys
        body << R"("price":")" << std::fixed << std::setprecision(0) << quantity << R"("})";
    }

    std::string body_str = body.str();
    auto headers = build_auth_headers();
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v1/orders", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Order failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order);
    return order;
}

Order UpbitExchange::place_market_buy_cost(const SymbolId& symbol, Price cost) {
    if (cost < MIN_ORDER_KRW) {
        Order order;
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Order cost {} below minimum {}", cost, MIN_ORDER_KRW);
        return order;
    }

    Order order;
    order.exchange = Exchange::Upbit;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    order.price = cost;  // Store cost in price field
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    std::string market = symbol.to_upbit_format();

    std::ostringstream body;
    body << R"({"market":")" << market << R"(",)";
    body << R"("side":"bid",)";
    body << R"("ord_type":"price",)";  // price = market buy with KRW amount
    body << R"("price":")" << std::fixed << std::setprecision(0) << cost << R"("})";

    std::string body_str = body.str();
    auto headers = build_auth_headers();
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v1/orders", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Upbit] Buy order failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order);
    Logger::info("[Upbit] Market buy {} for {} KRW - Status: {}",
                 symbol.to_string(), cost, static_cast<int>(order.status));
    return order;
}

bool UpbitExchange::cancel_order(uint64_t order_id) {
    std::string query = "uuid=" + std::to_string(order_id);
    auto headers = build_auth_headers(query);

    auto response = rest_client_->del("/v1/order?" + query, headers);
    return response.success;
}

double UpbitExchange::get_balance(const std::string& currency) {
    auto headers = build_auth_headers();

    auto response = rest_client_->get("/v1/accounts", headers);
    if (!response.success) {
        Logger::error("[Upbit] Failed to fetch balance: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        for (auto account : doc.get_array()) {
            std::string_view curr = account["currency"].get_string().value();
            if (curr == currency) {
                std::string_view balance_str = account["balance"].get_string().value();
                return opt::fast_stod(balance_str);
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse balance: {}", e.what());
    }

    return 0.0;
}

void UpbitExchange::on_ws_message(std::string_view message) {
    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        // Update USDT price if this is USDT/KRW
        if (ticker.symbol.get_base() == "USDT") {
            usdt_krw_price_.store(ticker.last);
        }

        dispatch_ticker(ticker);
    }
}

void UpbitExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[Upbit] WebSocket connected");
}

void UpbitExchange::on_ws_disconnected() {
    connected_ = false;
    Logger::warn("[Upbit] WebSocket disconnected");
}

std::string UpbitExchange::generate_jwt_token() const {
    auto token = jwt::create()
        .set_type("JWT")
        .set_payload_claim("access_key", jwt::claim(credentials_.api_key))
        .set_payload_claim("nonce", jwt::claim(utils::Crypto::generate_uuid()))
        .sign(jwt::algorithm::hs256{credentials_.secret_key});

    return "Bearer " + token;
}

std::string UpbitExchange::generate_jwt_token_with_query(const std::string& query_string) const {
    std::string query_hash = utils::Crypto::sha512(query_string);

    auto token = jwt::create()
        .set_type("JWT")
        .set_payload_claim("access_key", jwt::claim(credentials_.api_key))
        .set_payload_claim("nonce", jwt::claim(utils::Crypto::generate_uuid()))
        .set_payload_claim("query_hash", jwt::claim(query_hash))
        .set_payload_claim("query_hash_alg", jwt::claim(std::string("SHA512")))
        .sign(jwt::algorithm::hs256{credentials_.secret_key});

    return "Bearer " + token;
}

std::unordered_map<std::string, std::string> UpbitExchange::build_auth_headers() const {
    return {{"Authorization", generate_jwt_token()}};
}

std::unordered_map<std::string, std::string> UpbitExchange::build_auth_headers(const std::string& query) const {
    return {{"Authorization", generate_jwt_token_with_query(query)}};
}

bool UpbitExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        // Check if this is ticker data
        std::string_view type = doc["ty"].get_string().value();
        if (type != "ticker") {
            return false;
        }

        ticker.exchange = Exchange::Upbit;
        ticker.timestamp = std::chrono::steady_clock::now();

        // Parse market code (e.g., "KRW-BTC")
        std::string_view code = doc["cd"].get_string().value();
        auto pos = code.find('-');
        if (pos != std::string_view::npos) {
            ticker.symbol.set_quote(code.substr(0, pos));
            ticker.symbol.set_base(code.substr(pos + 1));
        }

        // Parse prices
        ticker.last = doc["tp"].get_double().value();      // trade_price
        ticker.bid = doc["hp"].get_double().value();       // high_price as bid approximation
        ticker.ask = doc["lp"].get_double().value();       // low_price as ask approximation

        // Actually for trading, we need proper bid/ask
        // In SIMPLE format: hp=high, lp=low, tp=trade
        // We'll use trade_price for both bid/ask as approximation for ticker
        ticker.bid = ticker.last;
        ticker.ask = ticker.last;

        ticker.high_24h = doc["hp"].get_double().value();
        ticker.low_24h = doc["lp"].get_double().value();
        ticker.volume_24h = doc["tv"].get_double().value();  // trade_volume

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Upbit] Failed to parse ticker: {}", e.what());
        return false;
    }
}

bool UpbitExchange::parse_order_response(const std::string& response, Order& order) {
    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response);
        auto doc = local_parser.iterate(padded);

        std::string_view uuid = doc["uuid"].get_string().value();
        order.exchange_order_id = std::hash<std::string_view>{}(uuid);

        std::string_view state = doc["state"].get_string().value();
        if (state == "done") {
            order.status = OrderStatus::Filled;
        } else if (state == "wait") {
            order.status = OrderStatus::New;
        } else if (state == "cancel") {
            order.status = OrderStatus::Cancelled;
        } else {
            order.status = OrderStatus::PartiallyFilled;
        }

        // Parse filled quantity and average price if available
        auto volume = doc["volume"];
        if (!volume.error()) {
            std::string_view vol_str = volume.get_string().value();
            order.quantity = opt::fast_stod(vol_str);
        }

        auto executed = doc["executed_volume"];
        if (!executed.error()) {
            std::string_view exec_str = executed.get_string().value();
            order.filled_quantity = opt::fast_stod(exec_str);
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Upbit] Failed to parse order response: {}", e.what());
        return false;
    }
}

} // namespace kimp::exchange::upbit
