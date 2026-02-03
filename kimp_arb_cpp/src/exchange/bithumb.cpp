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
    if (!ws_client_ || !ws_client_->is_connected()) return;

    std::ostringstream ss;
    ss << R"({"type":"orderbookdepth","symbols":[)";

    bool first = true;
    for (const auto& sym : symbols) {
        if (!first) ss << ",";
        ss << "\"" << sym.to_bithumb_format() << "\"";
        first = false;
    }

    ss << R"(]})";
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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
                ticker.bid = ticker.last;  // Approximation
                ticker.ask = ticker.last;  // Approximation
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

double BithumbExchange::get_usdt_krw_price() {
    double cached = usdt_krw_price_.load();
    if (cached > 0) return cached;

    auto response = rest_client_->get("/public/ticker/USDT_KRW");
    if (!response.success) {
        Logger::error("[Bithumb] Failed to fetch USDT price: {}", response.error);
        return TradingConfig::DEFAULT_USDT_KRW;
    }

    try {
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
        std::string_view price_str = doc["data"]["closing_price"].get_string().value();
        double price = opt::fast_stod(price_str);
        usdt_krw_price_.store(price);
        return price;
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bithumb] Failed to parse USDT price: {}", e.what());
    }

    return TradingConfig::DEFAULT_USDT_KRW;
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

    try {
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
        std::string_view status = doc["status"].get_string().value();
        if (status == "0000") {
            order.status = OrderStatus::Filled;
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
        simdjson::padded_string padded(response.body);
        auto doc = json_parser_.iterate(padded);
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
    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        if (ticker.symbol.get_base() == "USDT") {
            usdt_krw_price_.store(ticker.last);
        }
        dispatch_ticker(ticker);
    }
}

void BithumbExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[Bithumb] WebSocket connected");
}

void BithumbExchange::on_ws_disconnected() {
    connected_ = false;
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
        simdjson::padded_string padded(message);
        auto doc = json_parser_.iterate(padded);

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
        ticker.bid = ticker.last;
        ticker.ask = ticker.last;

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::debug("[Bithumb] Failed to parse ticker: {}", e.what());
        return false;
    }
}

} // namespace kimp::exchange::bithumb
