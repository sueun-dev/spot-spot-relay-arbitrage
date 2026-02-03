#include "kimp/exchange/gateio/gateio.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>
#include <cmath>

namespace kimp::exchange::gateio {

bool GateIOExchange::connect() {
    if (connected_) {
        Logger::warn("[GateIO] Already connected");
        return true;
    }

    Logger::info("[GateIO] Connecting...");

    // Initialize REST connection pool first (pre-establish SSL connections)
    if (!initialize_rest()) {
        Logger::error("[GateIO] Failed to initialize REST connection pool");
        return false;
    }
    Logger::info("[GateIO] REST connection pool initialized (4 persistent connections)");

    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "GateIO-WS");

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType type) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[GateIO] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& reason) {
        on_ws_disconnected();
    });

    ws_client_->connect(credentials_.ws_endpoint);
    return true;
}

void GateIOExchange::disconnect() {
    // Shutdown REST connection pool
    shutdown_rest();

    if (ws_client_) {
        ws_client_->disconnect();
    }
    connected_ = false;
    Logger::info("[GateIO] Disconnected");
}

void GateIOExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[GateIO] Cannot subscribe, not connected");
        return;
    }

    int64_t timestamp = utils::Crypto::timestamp_sec();

    // Format: {"time":123456,"channel":"futures.tickers","event":"subscribe","payload":["BTC_USDT"]}
    std::ostringstream ss;
    ss << R"({"time":)" << timestamp << R"(,)";
    ss << R"("channel":"futures.tickers",)";
    ss << R"("event":"subscribe",)";
    ss << R"("payload":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << symbol_to_gateio(sym) << "\"";
        first = false;
    }

    ss << "]}";

    Logger::debug("[GateIO] Subscribing to {} symbols", symbols.size());
    ws_client_->send(ss.str());
}

void GateIOExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    if (!ws_client_ || !ws_client_->is_connected()) return;

    int64_t timestamp = utils::Crypto::timestamp_sec();

    std::ostringstream ss;
    ss << R"({"time":)" << timestamp << R"(,)";
    ss << R"("channel":"futures.order_book",)";
    ss << R"("event":"subscribe",)";
    ss << R"("payload":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << symbol_to_gateio(sym) << "\"";
        first = false;
    }

    ss << R"(,"5","0"]})";  // depth=5, interval=0
    ws_client_->send(ss.str());
}

std::vector<SymbolId> GateIOExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/api/v4/futures/usdt/contracts");
    if (!response.success) {
        Logger::error("[GateIO] Failed to fetch markets: {}", response.error);
        return symbols;
    }

    try {
        auto doc = json_parser_.iterate(response.body);

        for (auto contract : doc.get_array()) {
            std::string_view name = contract["name"].get_string().value();

            // Parse "BTC_USDT" format
            auto pos = name.find('_');
            if (pos != std::string_view::npos) {
                std::string base(name.substr(0, pos));
                std::string quote(name.substr(pos + 1));

                if (quote == "USDT") {
                    symbols.emplace_back(base, quote);

                    // Cache contract size
                    auto quanto_multiplier = contract["quanto_multiplier"];
                    if (!quanto_multiplier.error()) {
                        std::string_view mult_str = quanto_multiplier.get_string().value();
                        double size = opt::fast_stod(mult_str);
                        std::lock_guard lock(contract_mutex_);
                        contract_sizes_[std::string(name)] = size;
                    }
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[GateIO] Failed to parse markets: {}", e.what());
    }

    Logger::info("[GateIO] Found {} USDT perpetual markets", symbols.size());
    return symbols;
}

double GateIOExchange::get_funding_rate(const SymbolId& symbol) {
    std::string gateio_symbol = symbol_to_gateio(symbol);
    std::string endpoint = "/api/v4/futures/usdt/contracts/" + gateio_symbol;

    auto response = rest_client_->get(endpoint);
    if (!response.success) {
        Logger::error("[GateIO] Failed to fetch funding rate: {}", response.error);
        return 0.0;
    }

    try {
        auto doc = json_parser_.iterate(response.body);
        std::string_view rate_str = doc["funding_rate"].get_string().value();
        return opt::fast_stod(rate_str);
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[GateIO] Failed to parse funding rate: {}", e.what());
    }

    return 0.0;
}

double GateIOExchange::get_contract_size(const SymbolId& symbol) {
    std::string gateio_symbol = symbol_to_gateio(symbol);

    {
        std::lock_guard lock(contract_mutex_);
        auto it = contract_sizes_.find(gateio_symbol);
        if (it != contract_sizes_.end()) {
            return it->second;
        }
    }

    // Fetch if not cached
    std::string endpoint = "/api/v4/futures/usdt/contracts/" + gateio_symbol;
    auto response = rest_client_->get(endpoint);

    if (response.success) {
        try {
            auto doc = json_parser_.iterate(response.body);
            std::string_view mult_str = doc["quanto_multiplier"].get_string().value();
            double size = opt::fast_stod(mult_str);

            std::lock_guard lock(contract_mutex_);
            contract_sizes_[gateio_symbol] = size;
            return size;
        } catch (const simdjson::simdjson_error& e) {
            Logger::error("[GateIO] Failed to parse contract size: {}", e.what());
        }
    }

    return 1.0;  // Default
}

int64_t GateIOExchange::coins_to_contracts(const SymbolId& symbol, double coins) {
    double contract_size = get_contract_size(symbol);
    return static_cast<int64_t>(std::floor(coins / contract_size));
}

Order GateIOExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    Order order;
    order.exchange = Exchange::GateIO;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    int64_t contracts = coins_to_contracts(symbol, quantity);
    if (contracts <= 0) {
        order.status = OrderStatus::Rejected;
        Logger::error("[GateIO] Invalid contract quantity for {}", quantity);
        return order;
    }

    std::ostringstream body;
    body << R"({"contract":")" << symbol_to_gateio(symbol) << R"(",)";
    body << R"("size":)" << (side == Side::Sell ? -contracts : contracts) << R"(,)";
    body << R"("tif":"ioc"})";  // Immediate-or-cancel for market orders

    std::string body_str = body.str();
    auto headers = build_auth_headers("POST", "/api/v4/futures/usdt/orders", "", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v4/futures/usdt/orders", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[GateIO] Order failed: {}", response.body);
        return order;
    }

    try {
        auto doc = json_parser_.iterate(response.body);
        auto id = doc["id"];
        if (!id.error()) {
            order.exchange_order_id = id.get_uint64().value();
            order.status = OrderStatus::Filled;
        } else {
            order.status = OrderStatus::Rejected;
        }
    } catch (const simdjson::simdjson_error& e) {
        order.status = OrderStatus::Rejected;
    }

    return order;
}

Order GateIOExchange::open_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::GateIO;
    order.symbol = symbol;
    order.side = Side::Sell;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    int64_t contracts = coins_to_contracts(symbol, quantity);
    if (contracts <= 0) {
        order.status = OrderStatus::Rejected;
        Logger::error("[GateIO] Invalid contract quantity");
        return order;
    }

    std::ostringstream body;
    body << R"({"contract":")" << symbol_to_gateio(symbol) << R"(",)";
    body << R"("size":)" << -contracts << R"(,)";  // Negative for short
    body << R"("tif":"ioc",)";
    body << R"("reduce_only":false})";

    std::string body_str = body.str();
    auto headers = build_auth_headers("POST", "/api/v4/futures/usdt/orders", "", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v4/futures/usdt/orders", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[GateIO] Short open failed: {}", response.body);
        return order;
    }

    try {
        auto doc = json_parser_.iterate(response.body);
        auto id = doc["id"];
        if (!id.error()) {
            order.exchange_order_id = id.get_uint64().value();
            order.status = OrderStatus::Filled;
            Logger::info("[GateIO] Opened short {} {} contracts",
                         symbol.to_string(), contracts);
        } else {
            order.status = OrderStatus::Rejected;
        }
    } catch (const simdjson::simdjson_error& e) {
        order.status = OrderStatus::Rejected;
    }

    return order;
}

Order GateIOExchange::close_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::GateIO;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    int64_t contracts = coins_to_contracts(symbol, quantity);
    if (contracts <= 0) {
        order.status = OrderStatus::Rejected;
        return order;
    }

    std::ostringstream body;
    body << R"({"contract":")" << symbol_to_gateio(symbol) << R"(",)";
    body << R"("size":)" << contracts << R"(,)";  // Positive to close short
    body << R"("tif":"ioc",)";
    body << R"("reduce_only":true})";

    std::string body_str = body.str();
    auto headers = build_auth_headers("POST", "/api/v4/futures/usdt/orders", "", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v4/futures/usdt/orders", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[GateIO] Short close failed: {}", response.body);
        return order;
    }

    try {
        auto doc = json_parser_.iterate(response.body);
        auto id = doc["id"];
        if (!id.error()) {
            order.exchange_order_id = id.get_uint64().value();
            order.status = OrderStatus::Filled;
            Logger::info("[GateIO] Closed short {} {} contracts",
                         symbol.to_string(), contracts);
        } else {
            order.status = OrderStatus::Rejected;
        }
    } catch (const simdjson::simdjson_error& e) {
        order.status = OrderStatus::Rejected;
    }

    return order;
}

bool GateIOExchange::cancel_order(uint64_t order_id) {
    return false;  // TODO: Implement if needed
}

bool GateIOExchange::set_leverage(const SymbolId& symbol, int leverage) {
    std::string gateio_symbol = symbol_to_gateio(symbol);
    std::string endpoint = "/api/v4/futures/usdt/positions/" + gateio_symbol + "/leverage";

    std::ostringstream body;
    body << R"({"leverage":)" << leverage << R"(})";

    std::string body_str = body.str();
    auto headers = build_auth_headers("POST", endpoint, "", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post(endpoint, body_str, headers);
    return response.success;
}

std::vector<Position> GateIOExchange::get_positions() {
    std::vector<Position> positions;

    auto headers = build_auth_headers("GET", "/api/v4/futures/usdt/positions");
    auto response = rest_client_->get("/api/v4/futures/usdt/positions", headers);

    if (!response.success) {
        Logger::error("[GateIO] Failed to fetch positions: {}", response.error);
        return positions;
    }

    try {
        auto doc = json_parser_.iterate(response.body);

        for (auto item : doc.get_array()) {
            auto size = item["size"].get_int64().value();

            if (size != 0) {
                Position pos;

                std::string_view contract = item["contract"].get_string().value();
                auto pos_underscore = contract.find('_');
                if (pos_underscore != std::string_view::npos) {
                    std::string base(contract.substr(0, pos_underscore));
                    pos.symbol = SymbolId(base, "USDT");
                }

                pos.foreign_exchange = Exchange::GateIO;
                pos.foreign_amount = std::abs(size) * get_contract_size(pos.symbol);
                pos.is_active = true;

                if (size < 0) {  // Short position
                    positions.push_back(pos);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[GateIO] Failed to parse positions: {}", e.what());
    }

    return positions;
}

bool GateIOExchange::close_position(const SymbolId& symbol) {
    auto positions = get_positions();

    for (const auto& pos : positions) {
        if (pos.symbol == symbol && pos.foreign_amount > 0) {
            auto order = close_short(symbol, pos.foreign_amount);
            return order.status == OrderStatus::Filled;
        }
    }

    return true;
}

double GateIOExchange::get_balance(const std::string& currency) {
    auto headers = build_auth_headers("GET", "/api/v4/futures/usdt/accounts");
    auto response = rest_client_->get("/api/v4/futures/usdt/accounts", headers);

    if (!response.success) {
        Logger::error("[GateIO] Failed to fetch balance: {}", response.error);
        return 0.0;
    }

    try {
        auto doc = json_parser_.iterate(response.body);
        std::string_view available_str = doc["available"].get_string().value();
        return opt::fast_stod(available_str);
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[GateIO] Failed to parse balance: {}", e.what());
    }

    return 0.0;
}

void GateIOExchange::on_ws_message(std::string_view message) {
    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        dispatch_ticker(ticker);
    }
}

void GateIOExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[GateIO] WebSocket connected");
}

void GateIOExchange::on_ws_disconnected() {
    connected_ = false;
    Logger::warn("[GateIO] WebSocket disconnected");
}

std::string GateIOExchange::generate_signature(const std::string& method,
                                                 const std::string& url,
                                                 const std::string& query_string,
                                                 const std::string& body,
                                                 int64_t timestamp) const {
    std::string body_hash = utils::Crypto::sha512(body);
    std::string sign_string = method + "\n" + url + "\n" + query_string +
                              "\n" + body_hash + "\n" + std::to_string(timestamp);
    return utils::Crypto::hmac_sha512(credentials_.secret_key, sign_string);
}

std::unordered_map<std::string, std::string> GateIOExchange::build_auth_headers(
    const std::string& method,
    const std::string& url,
    const std::string& query,
    const std::string& body) const {

    int64_t timestamp = utils::Crypto::timestamp_sec();
    std::string signature = generate_signature(method, url, query, body, timestamp);

    return {
        {"KEY", credentials_.api_key},
        {"SIGN", signature},
        {"Timestamp", std::to_string(timestamp)}
    };
}

bool GateIOExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    try {
        std::memcpy(json_buffer_.data(), message.data(), message.size());
        auto doc = json_parser_.iterate(json_buffer_.data(), message.size(), 8192);

        auto channel = doc["channel"];
        if (channel.error()) return false;

        std::string_view channel_str = channel.get_string().value();
        if (channel_str != "futures.tickers") return false;

        auto result = doc["result"];
        if (result.error()) return false;

        ticker.exchange = Exchange::GateIO;
        ticker.timestamp = std::chrono::steady_clock::now();

        std::string_view contract = result["contract"].get_string().value();
        auto pos = contract.find('_');
        if (pos != std::string_view::npos) {
            std::string base(contract.substr(0, pos));
            ticker.symbol = SymbolId(base, "USDT");
        }

        std::string_view last_str = result["last"].get_string().value();
        ticker.last = opt::fast_stod(last_str);

        // Gate.io might have bid1/ask1 or just use last
        ticker.bid = ticker.last;
        ticker.ask = ticker.last;

        return true;
    } catch (const simdjson::simdjson_error& e) {
        return false;
    }
}

} // namespace kimp::exchange::gateio
