#include "kimp/exchange/bybit/bybit.hpp"
#include "kimp/exchange/bybit/bybit_trade_ws.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <limits>
#include <cmath>

namespace kimp::exchange::bybit {

namespace {

bool parse_quoted_double_pair(std::string_view message,
                              std::string_view marker,
                              double& first,
                              double& second) {
    const size_t start = message.find(marker);
    if (start == std::string_view::npos) {
        return false;
    }

    const size_t first_start = start + marker.size();
    const size_t first_end = message.find('"', first_start);
    if (first_end == std::string_view::npos) {
        return false;
    }

    const size_t second_marker = message.find("\",\"", first_end);
    if (second_marker == std::string_view::npos) {
        return false;
    }
    const size_t second_start = second_marker + 3;
    const size_t second_end = message.find('"', second_start);
    if (second_end == std::string_view::npos) {
        return false;
    }

    first = opt::fast_stod(message.substr(first_start, first_end - first_start));
    second = opt::fast_stod(message.substr(second_start, second_end - second_start));
    return true;
}

bool parse_orderbook_fast(std::string_view message, Ticker& ticker) {
    static constexpr std::string_view topic_marker = R"("topic":"orderbook.1.)";
    static constexpr std::string_view symbol_marker = R"("s":")";
    static constexpr std::string_view bids_marker = R"("b":[[")";
    static constexpr std::string_view asks_marker = R"("a":[[")";

    const size_t topic_start = message.find(topic_marker);
    if (topic_start == std::string_view::npos) {
        return false;
    }

    const size_t symbol_start = topic_start + topic_marker.size();
    const size_t symbol_end = message.find('"', symbol_start);
    if (symbol_end == std::string_view::npos) {
        return false;
    }

    std::string_view topic_symbol = message.substr(symbol_start, symbol_end - symbol_start);
    if (topic_symbol.size() <= 4 || !topic_symbol.ends_with("USDT")) {
        return false;
    }

    size_t data_symbol_start = message.find(symbol_marker, symbol_end);
    if (data_symbol_start != std::string_view::npos) {
        data_symbol_start += symbol_marker.size();
        size_t data_symbol_end = message.find('"', data_symbol_start);
        if (data_symbol_end == std::string_view::npos) {
            return false;
        }
        const std::string_view data_symbol = message.substr(data_symbol_start, data_symbol_end - data_symbol_start);
        if (data_symbol != topic_symbol) {
            return false;
        }
    }

    double bid = 0.0;
    double bid_qty = 0.0;
    double ask = 0.0;
    double ask_qty = 0.0;
    if (!parse_quoted_double_pair(message, bids_marker, bid, bid_qty) ||
        !parse_quoted_double_pair(message, asks_marker, ask, ask_qty)) {
        return false;
    }

    ticker.exchange = Exchange::Bybit;
    ticker.timestamp = std::chrono::steady_clock::now();
    ticker.symbol = SymbolId(std::string(topic_symbol.substr(0, topic_symbol.size() - 4)), "USDT");
    ticker.bid = bid;
    ticker.bid_qty = bid_qty;
    ticker.ask = ask;
    ticker.ask_qty = ask_qty;
    return bid > 0.0 && ask > 0.0;
}

}  // namespace

std::string BybitExchange::resolve_public_ws_endpoint() const {
    if (credentials_.ws_endpoint.empty()) {
        return "wss://stream.bybit.com/v5/public/spot";
    }
    return credentials_.ws_endpoint;
}

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

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType /*type*/) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[Bybit] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& /*reason*/) {
        on_ws_disconnected();
    });

    ws_client_->connect(resolve_public_ws_endpoint());

    // Initialize WebSocket Trade API for low-latency order placement
    if (!credentials_.ws_trade_endpoint.empty()) {
        trade_ws_ = std::make_unique<BybitTradeWS>(io_context_, credentials_);
        trade_ws_->connect();
    }

    // Connect Private WS for real-time fill data (replaces REST fill query)
    if (!credentials_.ws_private_endpoint.empty() && !credentials_.api_key.empty()) {
        private_ws_ = std::make_shared<network::WebSocketClient>(io_context_, "Bybit-Private-WS");

        private_ws_->set_message_callback([this](std::string_view msg, network::MessageType) {
            on_private_ws_message(msg);
        });

        private_ws_->set_connect_callback([this](bool success, const std::string& error) {
            if (success) {
                Logger::info("[Bybit-PrivateWS] Connected, authenticating...");
                authenticate_private_ws();
            } else {
                Logger::error("[Bybit-PrivateWS] Connection failed: {}", error);
            }
        });

        private_ws_->set_disconnect_callback([this](const std::string& reason) {
            private_ws_authenticated_ = false;
            Logger::warn("[Bybit-PrivateWS] Disconnected: {}", reason);
        });

        private_ws_->connect(credentials_.ws_private_endpoint);
    }

    return true;
}

void BybitExchange::disconnect() {
    // Shutdown Private WS
    if (private_ws_) {
        private_ws_->disconnect();
        private_ws_authenticated_ = false;
    }

    // Shutdown Trade WS
    if (trade_ws_) {
        trade_ws_->disconnect();
    }

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

    // Bybit WS allows max 10 args per subscribe request — batch to avoid silent drops
    constexpr size_t BATCH_SIZE = 10;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"op":"subscribe","args":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << "\"tickers." << symbol_to_bybit(symbols[i]) << "\"";
            first = false;
        }

        ss << "]}";
        ws_client_->send(ss.str());

        // Tiny pacing keeps startup fast without overwhelming the socket.
        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[Bybit] Subscribed to {} tickers in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);
}

void BybitExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_orderbooks_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) return;

    // Bybit WS allows max 10 args per subscribe request — batch to avoid silent drops
    constexpr size_t BATCH_SIZE = 10;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"op":"subscribe","args":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << "\"orderbook.1." << symbol_to_bybit(symbols[i]) << "\"";
            first = false;
        }

        ss << "]}";
        ws_client_->send(ss.str());

        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[Bybit] Subscribed to {} orderbooks in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);
}

std::vector<SymbolId> BybitExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;

    auto response = rest_client_->get("/v5/market/instruments-info?category=spot&limit=1000");
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
            std::string_view status = item["status"].get_string().value();
            auto margin_trading = item["marginTrading"];
            std::string_view margin_mode = margin_trading.error()
                ? std::string_view{}
                : margin_trading.get_string().value();

            // Only USDT spot pairs that support margin shorting.
            if (quote == "USDT" && status == "Trading" && !margin_mode.empty() && margin_mode != "none") {
                std::string_view base = item["baseCoin"].get_string().value();
                symbols.emplace_back(std::string(base), "USDT");

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

    Logger::info("[Bybit] Found {} margin-enabled USDT spot markets",
                 symbols.size());
    return symbols;
}

std::vector<Ticker> BybitExchange::fetch_all_tickers() {
    std::vector<Ticker> tickers;

    auto response = rest_client_->get("/v5/market/tickers?category=spot");
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
            auto bid_qty = item["bid1Size"];
            if (!bid_qty.error()) {
                ticker.bid_qty = opt::fast_stod(bid_qty.get_string().value());
            }

            std::string_view ask_str = item["ask1Price"].get_string().value();
            ticker.ask = opt::fast_stod(ask_str);
            auto ask_qty = item["ask1Size"];
            if (!ask_qty.error()) {
                ticker.ask_qty = opt::fast_stod(ask_qty.get_string().value());
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
    body << R"({"category":"spot",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":")" << (side == Side::Buy ? "Buy" : "Sell") << R"(",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << quantity << R"(",)";
    body << R"("orderFilter":"Order",)";
    body << R"("marketUnit":"baseCoin",)";
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

    parse_order_response(response.body, order, &order.order_id_str);
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

    if (!ensure_spot_margin_mode()) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Spot margin mode is not ready for {}", symbol.to_string());
        return order;
    }

    // Try WebSocket Trade API first (~5-20ms vs ~150-300ms REST)
    if (trade_ws_ && trade_ws_->is_connected()) {
        Order ws_order = trade_ws_->place_order_sync(
            symbol_to_bybit(symbol), Side::Sell, adj_qty, true);
        if (ws_order.status != OrderStatus::Rejected) {
            ws_order.symbol = symbol;
            ws_order.quantity = adj_qty;
            ws_order.client_order_id = order.client_order_id;
            Logger::info("[Bybit-WS] Opened spot-margin short {} {} - orderId: {}",
                         symbol.to_string(), adj_qty, ws_order.order_id_str);
            return ws_order;
        }
        Logger::warn("[Bybit] WS order failed, falling back to REST");
    }

    // REST fallback
    std::ostringstream body;
    body << R"({"category":"spot",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Sell",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << adj_qty << R"(",)";
    body << R"("isLeverage":1,)";
    body << R"("orderFilter":"Order",)";
    body << R"("marketUnit":"baseCoin"})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/order/create", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Spot-margin short open failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order, &order.order_id_str);
    Logger::info("[Bybit-REST] Opened spot-margin short {} {} - Status: {}, orderId: {}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.order_id_str);
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

    if (!ensure_spot_margin_mode()) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Spot margin mode is not ready for {}", symbol.to_string());
        return order;
    }

    // Try WebSocket Trade API first (~5-20ms vs ~150-300ms REST)
    if (trade_ws_ && trade_ws_->is_connected()) {
        Order ws_order = trade_ws_->place_order_sync(
            symbol_to_bybit(symbol), Side::Buy, adj_qty, true);
        if (ws_order.status != OrderStatus::Rejected) {
            ws_order.symbol = symbol;
            ws_order.quantity = adj_qty;
            ws_order.client_order_id = order.client_order_id;
            Logger::info("[Bybit-WS] Closed spot-margin short {} {} - orderId: {}",
                         symbol.to_string(), adj_qty, ws_order.order_id_str);
            return ws_order;
        }
        Logger::warn("[Bybit] WS close failed, falling back to REST");
    }

    // REST fallback
    std::ostringstream body;
    body << R"({"category":"spot",)";
    body << R"("symbol":")" << symbol_to_bybit(symbol) << R"(",)";
    body << R"("side":"Buy",)";
    body << R"("orderType":"Market",)";
    body << R"("qty":")" << std::fixed << std::setprecision(8) << adj_qty << R"(",)";
    body << R"("isLeverage":1,)";
    body << R"("orderFilter":"Order",)";
    body << R"("marketUnit":"baseCoin"})";

    std::string body_str = body.str();
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/order/create", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[Bybit] Spot-margin short close failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order, &order.order_id_str);
    Logger::info("[Bybit-REST] Closed spot-margin short {} {} - Status: {}, orderId: {}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.order_id_str);
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
        // Add small epsilon before floor to prevent floating-point precision loss
        // e.g. 0.04/0.01 = 3.9999999996 → floor=3 → 0.03 (wrong!)
        // With epsilon: 3.9999999996 + 1e-9 = 4.000000000 → floor=4 → 0.04 (correct)
        qty = std::floor(qty / step + 1e-9) * step;
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

bool BybitExchange::cancel_order(uint64_t /*order_id*/) {
    // TODO: Implement if needed
    return false;
}

bool BybitExchange::prepare_shorting(const SymbolId& symbol) {
    (void)symbol;
    return ensure_spot_margin_mode();
}

std::vector<Position> BybitExchange::get_short_positions() {
    std::vector<Position> positions;

    std::string query = "accountType=UNIFIED";
    auto headers = build_auth_headers(query);
    auto response = rest_client_->get("/v5/account/wallet-balance?" + query, headers);

    if (!response.success) {
        Logger::error("[Bybit] Failed to fetch positions: {}", response.error);
        return positions;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);
        auto accounts = doc["result"]["list"].get_array();
        for (auto account : accounts) {
            auto coins = account["coin"].get_array();
            for (auto coin : coins) {
                auto borrow_amount_field = coin["borrowAmount"];
                if (borrow_amount_field.error()) continue;

                double borrow_amount = opt::fast_stod(borrow_amount_field.get_string().value());
                if (borrow_amount <= 0.0) continue;

                std::string_view coin_name = coin["coin"].get_string().value();
                if (coin_name == "USDT") continue;

                Position pos;
                pos.symbol = SymbolId(std::string(coin_name), "USDT");
                pos.foreign_exchange = Exchange::Bybit;
                pos.foreign_amount = borrow_amount;
                pos.is_active = true;

                auto mark = get_cached_ticker(pos.symbol);
                if (mark) {
                    pos.foreign_entry_price = mark->bid > 0.0 ? mark->bid : mark->last;
                }
                positions.push_back(pos);
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[Bybit] Failed to parse positions: {}", e.what());
    }

    return positions;
}

bool BybitExchange::close_short_position(const SymbolId& symbol) {
    auto positions = get_short_positions();

    for (const auto& pos : positions) {
        if (pos.symbol == symbol && pos.foreign_amount > 0) {
            auto order = close_short(symbol, pos.foreign_amount);
            return order.status == OrderStatus::Filled;
        }
    }

    return true;  // No position to close
}

double BybitExchange::get_balance(const std::string& currency) {
    // Try UNIFIED first (Unified Trading Account), fall back to CONTRACT (legacy)
    for (const char* acct_type : {"UNIFIED", "CONTRACT"}) {
        std::string query = std::string("accountType=") + acct_type;
        auto headers = build_auth_headers(query);
        auto response = rest_client_->get("/v5/account/wallet-balance?" + query, headers);

        if (!response.success) {
            Logger::error("[Bybit] Failed to fetch balance ({}): {}", acct_type, response.error);
            continue;
        }

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);

            int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
            if (ret_code != 0) {
                std::string_view ret_msg = doc["retMsg"].get_string().value();
                Logger::warn("[Bybit] Balance query retCode={} ({}): {}", ret_code, acct_type, ret_msg);
                continue;
            }

            auto list = doc["result"]["list"].get_array();
            for (auto account : list) {
                auto coins = account["coin"].get_array();
                for (auto coin : coins) {
                    std::string_view coin_name = coin["coin"].get_string().value();
                    if (coin_name == currency) {
                        std::string_view balance_str = coin["walletBalance"].get_string().value();
                        double balance = opt::fast_stod(balance_str);
                        Logger::info("[Bybit] {} balance: {} ({})", currency, balance, acct_type);
                        return balance;
                    }
                }
            }
        } catch (const simdjson::simdjson_error& e) {
            Logger::error("[Bybit] Failed to parse balance ({}): {}", acct_type, e.what());
        }
    }

    Logger::warn("[Bybit] {} not found in any account type", currency);
    return 0.0;
}

void BybitExchange::on_ws_message(std::string_view message) {
    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        dispatch_ticker(ticker);
    } else if (!public_ws_parse_warned_.exchange(true, std::memory_order_relaxed) &&
               message.find("orderbook.1.") != std::string_view::npos) {
        Logger::warn("[Bybit-WS] Failed to parse orderbook payload: {}",
                     std::string(message.substr(0, 240)));
    }
}

void BybitExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[Bybit] WebSocket connected");

    std::vector<SymbolId> tickers_to_subscribe;
    std::vector<SymbolId> orderbooks_to_subscribe;
    {
        std::lock_guard lock(subscription_mutex_);
        tickers_to_subscribe = subscribed_tickers_;
        orderbooks_to_subscribe = subscribed_orderbooks_;
    }

    if (!tickers_to_subscribe.empty()) {
        Logger::info("[Bybit] Resubscribing to {} tickers after reconnection", tickers_to_subscribe.size());
        subscribe_ticker(tickers_to_subscribe);
    }
    if (!orderbooks_to_subscribe.empty()) {
        Logger::info("[Bybit] Resubscribing to {} orderbooks after reconnection", orderbooks_to_subscribe.size());
        subscribe_orderbook(orderbooks_to_subscribe);
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

bool BybitExchange::ensure_spot_margin_mode() {
    if (spot_margin_mode_ready_.load(std::memory_order_acquire)) {
        return true;
    }
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Logger::error("[Bybit] API credentials are required for spot margin orders");
        return false;
    }

    std::lock_guard lock(spot_margin_mutex_);
    if (spot_margin_mode_ready_.load(std::memory_order_relaxed)) {
        return true;
    }

    {
        auto headers = build_auth_headers();
        auto response = rest_client_->get("/v5/spot-margin-trade/state", headers);
        if (response.success) {
            try {
                simdjson::ondemand::parser local_parser;
                simdjson::padded_string padded(response.body);
                auto doc = local_parser.iterate(padded);
                auto mode = doc["result"]["spotMarginMode"];
                if (!mode.error() && mode.get_string().value() == std::string_view("1")) {
                    spot_margin_mode_ready_.store(true, std::memory_order_release);
                    return true;
                }
            } catch (const simdjson::simdjson_error&) {
            }
        }
    }

    const std::string body_str = R"({"spotMarginMode":"1"})";
    auto headers = build_auth_headers(body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/v5/spot-margin-trade/switch-mode", body_str, headers);
    if (!response.success) {
        Logger::error("[Bybit] Spot margin mode switch failed: {}", response.body);
        return false;
    }

    spot_margin_mode_ready_.store(true, std::memory_order_release);
    Logger::info("[Bybit] Spot margin mode enabled");
    return true;
}

bool BybitExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    if (message.find(R"("topic":"orderbook.1.)") != std::string_view::npos) {
        if (parse_orderbook_fast(message, ticker)) {
            auto cached = get_cached_ticker(ticker.symbol);
            if (cached && cached->last > 0.0) {
                ticker.last = cached->last;
            } else {
                ticker.last = (ticker.bid + ticker.ask) * 0.5;
            }
            return true;
        }
    }

    try {
        simdjson::dom::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.parse(padded);

        auto topic_elem = doc["topic"];
        if (topic_elem.error()) return false;
        std::string_view topic_str = std::string_view(topic_elem.get_c_str().value());

        auto data = doc["data"];
        if (data.error()) return false;

        ticker.exchange = Exchange::Bybit;
        ticker.timestamp = std::chrono::steady_clock::now();
        if (topic_str.substr(0, 8) == "tickers.") {
            auto symbol_elem = data["symbol"];
            if (symbol_elem.error()) return false;
            std::string_view symbol_str = std::string_view(symbol_elem.get_c_str().value());
            if (symbol_str.size() > 4) {
                std::string base(symbol_str.substr(0, symbol_str.size() - 4));
                ticker.symbol = SymbolId(base, "USDT");
            }

            auto last_elem = data["lastPrice"];
            if (last_elem.error()) return false;
            ticker.last = opt::fast_stod(last_elem.get_c_str().value());

            auto bid_elem = data["bid1Price"];
            if (bid_elem.error()) return false;
            ticker.bid = opt::fast_stod(bid_elem.get_c_str().value());
            auto bid_qty = data["bid1Size"];
            if (!bid_qty.error()) {
                ticker.bid_qty = opt::fast_stod(bid_qty.get_c_str().value());
            }

            auto ask_elem = data["ask1Price"];
            if (ask_elem.error()) return false;
            ticker.ask = opt::fast_stod(ask_elem.get_c_str().value());
            auto ask_qty = data["ask1Size"];
            if (!ask_qty.error()) {
                ticker.ask_qty = opt::fast_stod(ask_qty.get_c_str().value());
            }
            return ticker.last > 0.0 && ticker.bid > 0.0 && ticker.ask > 0.0;
        }

        if (topic_str.substr(0, 12) != "orderbook.1.") return false;

        auto symbol_elem = data["s"];
        if (symbol_elem.error()) return false;
        std::string_view symbol_str = std::string_view(symbol_elem.get_c_str().value());
        if (symbol_str.size() <= 4) return false;
        ticker.symbol = SymbolId(std::string(symbol_str.substr(0, symbol_str.size() - 4)), "USDT");

        auto bids = data["b"];
        auto asks = data["a"];
        if (bids.error() || asks.error()) return false;

        auto bid_rows = bids.get_array().value();
        auto ask_rows = asks.get_array().value();
        if (bid_rows.size() == 0 || ask_rows.size() == 0) return false;

        auto bid_row = bid_rows.at(0);
        auto ask_row = ask_rows.at(0);
        auto bid_vals = bid_row.get_array().value();
        auto ask_vals = ask_row.get_array().value();
        if (bid_vals.size() < 2 || ask_vals.size() < 2) return false;

        ticker.bid = opt::fast_stod(bid_vals.at(0).get_c_str().value());
        ticker.bid_qty = opt::fast_stod(bid_vals.at(1).get_c_str().value());
        ticker.ask = opt::fast_stod(ask_vals.at(0).get_c_str().value());
        ticker.ask_qty = opt::fast_stod(ask_vals.at(1).get_c_str().value());

        auto cached = get_cached_ticker(ticker.symbol);
        if (cached && cached->last > 0.0) {
            ticker.last = cached->last;
        } else {
            ticker.last = (ticker.bid + ticker.ask) * 0.5;
        }

        return ticker.bid > 0.0 && ticker.ask > 0.0;
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
    // Try WS fill cache first (~50-200ms vs ~300ms+ REST)
    if (private_ws_authenticated_.load()) {
        std::unique_lock lock(fill_cache_mutex_);

        // Check immediately
        auto it = fill_cache_.find(order_id);
        if (it != fill_cache_.end()) {
            order.average_price = it->second.avg_price;
            order.filled_quantity = it->second.filled_qty;
            fill_cache_.erase(it);
            Logger::info("[Bybit-WS] Fill: orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                         order_id, order.average_price, order.filled_quantity);
            return true;
        }

        // Wait up to 500ms for fill event from Private WS
        bool found = fill_cache_cv_.wait_for(lock, std::chrono::milliseconds(500), [&]() {
            return fill_cache_.find(order_id) != fill_cache_.end();
        });

        if (found) {
            it = fill_cache_.find(order_id);
            order.average_price = it->second.avg_price;
            order.filled_quantity = it->second.filled_qty;
            fill_cache_.erase(it);
            Logger::info("[Bybit-WS] Fill (waited): orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                         order_id, order.average_price, order.filled_quantity);
            return true;
        }

        Logger::warn("[Bybit] WS fill cache miss for {}, falling back to REST", order_id);
    }

    // REST fallback
    constexpr int MAX_RETRIES = 5;
    constexpr int BASE_DELAY_MS = 300;

    std::string query = "category=spot&orderId=" + order_id;
    std::string endpoint = "/v5/order/realtime?" + query;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        if (attempt > 0) {
            int delay = BASE_DELAY_MS * (1 << (attempt - 1));
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
                    Logger::info("[Bybit-REST] Fill: orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                                 order_id, order.average_price, order.filled_quantity);
                    return true;
                }
                break;
            }

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

void BybitExchange::authenticate_private_ws() {
    int64_t expires = utils::Crypto::timestamp_ms() + 10000;
    std::string val = "GET/realtime" + std::to_string(expires);
    std::string signature = utils::Crypto::hmac_sha256(credentials_.secret_key, val);

    std::ostringstream ss;
    ss << R"({"op":"auth","args":[")" << credentials_.api_key << R"(",)";
    ss << expires << R"(,")" << signature << R"("]})";

    private_ws_->send(ss.str());
}

void BybitExchange::on_private_ws_message(std::string_view message) {
    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.iterate(padded);

        // Auth response
        auto op = doc["op"];
        if (!op.error()) {
            std::string_view op_str = op.get_string().value();
            if (op_str == "auth") {
                auto success = doc["success"];
                if (!success.error() && success.get_bool().value()) {
                    private_ws_authenticated_ = true;
                    private_ws_->send(std::string(R"({"op":"subscribe","args":["order"]})"));
                    Logger::info("[Bybit-PrivateWS] Authenticated, subscribed to order stream");
                } else {
                    Logger::error("[Bybit-PrivateWS] Authentication failed");
                }
            }
            return;
        }

        // Order update
        auto topic = doc["topic"];
        if (!topic.error()) {
            std::string_view topic_str = topic.get_string().value();
            if (topic_str == "order") {
                auto data = doc["data"].get_array();
                for (auto item : data) {
                    auto status_field = item["orderStatus"];
                    if (status_field.error()) continue;
                    std::string_view status = status_field.get_string().value();

                    if (status == "Filled") {
                        auto id_field = item["orderId"];
                        if (id_field.error()) continue;
                        std::string order_id(id_field.get_string().value());

                        FillInfo fill;
                        auto avg = item["avgPrice"];
                        if (!avg.error()) fill.avg_price = opt::fast_stod(avg.get_string().value());
                        auto qty = item["cumExecQty"];
                        if (!qty.error()) fill.filled_qty = opt::fast_stod(qty.get_string().value());

                        if (fill.avg_price > 0 && fill.filled_qty > 0) {
                            {
                                std::lock_guard lock(fill_cache_mutex_);
                                fill_cache_[order_id] = fill;
                            }
                            fill_cache_cv_.notify_all();
                        }
                    }
                }
            }
        }
    } catch (const simdjson::simdjson_error&) {
        // Ignore parse errors for non-JSON messages
    }
}

} // namespace kimp::exchange::bybit
