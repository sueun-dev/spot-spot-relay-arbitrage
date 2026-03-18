#include "kimp/exchange/okx/okx.hpp"
#include "kimp/exchange/okx/okx_trade_ws.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <ctime>
#include <unordered_set>

namespace kimp::exchange::okx {

namespace {

// Fast OKX BBO message parser (bbo-tbt channel)
// Format: {"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"asks":[["price","size","","1"]],"bids":[["price","size","","1"]],"ts":"1597026383085"}]}
bool parse_bbo_fast(std::string_view message, Ticker& ticker) {
    static constexpr std::string_view channel_marker = R"("channel":"bbo-tbt")";
    static constexpr std::string_view inst_marker = R"("instId":")";
    static constexpr std::string_view bids_marker = R"("bids":[[")";
    static constexpr std::string_view asks_marker = R"("asks":[[")";

    if (message.find(channel_marker) == std::string_view::npos) {
        return false;
    }

    // Extract instId from arg section
    const size_t inst_start = message.find(inst_marker);
    if (inst_start == std::string_view::npos) {
        return false;
    }
    const size_t id_start = inst_start + inst_marker.size();
    const size_t id_end = message.find('"', id_start);
    if (id_end == std::string_view::npos) {
        return false;
    }

    std::string_view inst_id = message.substr(id_start, id_end - id_start);
    // Parse "BTC-USDT" -> base="BTC", quote="USDT"
    auto dash = inst_id.find('-');
    if (dash == std::string_view::npos) {
        return false;
    }
    std::string_view base = inst_id.substr(0, dash);
    std::string_view quote = inst_id.substr(dash + 1);
    if (quote != "USDT") {
        return false;
    }

    // Parse bid: "bids":[["price","size","","1"]]
    const size_t bids_start = message.find(bids_marker);
    if (bids_start == std::string_view::npos) {
        return false;
    }
    const size_t bid_price_start = bids_start + bids_marker.size();
    const size_t bid_price_end = message.find('"', bid_price_start);
    if (bid_price_end == std::string_view::npos) {
        return false;
    }

    // Next quoted value after bid price is bid size
    const size_t bid_size_marker = message.find("\",\"", bid_price_end);
    if (bid_size_marker == std::string_view::npos) {
        return false;
    }
    const size_t bid_size_start = bid_size_marker + 3;
    const size_t bid_size_end = message.find('"', bid_size_start);
    if (bid_size_end == std::string_view::npos) {
        return false;
    }

    // Parse ask: "asks":[["price","size","","1"]]
    const size_t asks_start = message.find(asks_marker);
    if (asks_start == std::string_view::npos) {
        return false;
    }
    const size_t ask_price_start = asks_start + asks_marker.size();
    const size_t ask_price_end = message.find('"', ask_price_start);
    if (ask_price_end == std::string_view::npos) {
        return false;
    }

    const size_t ask_size_marker = message.find("\",\"", ask_price_end);
    if (ask_size_marker == std::string_view::npos) {
        return false;
    }
    const size_t ask_size_start = ask_size_marker + 3;
    const size_t ask_size_end = message.find('"', ask_size_start);
    if (ask_size_end == std::string_view::npos) {
        return false;
    }

    double bid = opt::fast_stod(message.substr(bid_price_start, bid_price_end - bid_price_start));
    double bid_qty = opt::fast_stod(message.substr(bid_size_start, bid_size_end - bid_size_start));
    double ask = opt::fast_stod(message.substr(ask_price_start, ask_price_end - ask_price_start));
    double ask_qty = opt::fast_stod(message.substr(ask_size_start, ask_size_end - ask_size_start));

    ticker.exchange = Exchange::OKX;
    ticker.timestamp = std::chrono::steady_clock::now();
    ticker.symbol = SymbolId(base, quote);
    ticker.bid = bid;
    ticker.bid_qty = bid_qty;
    ticker.ask = ask;
    ticker.ask_qty = ask_qty;
    return bid > 0.0 && ask > 0.0;
}

}  // namespace

std::string OkxExchange::generate_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    struct tm tm_buf;
    gmtime_r(&time_t_now, &tm_buf);

    char buf[32];
    int len = std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms.count()));
    return std::string(buf, static_cast<size_t>(len));
}

std::string OkxExchange::resolve_public_ws_endpoint() const {
    if (credentials_.ws_endpoint.empty()) {
        return "wss://ws.okx.com:8443/ws/v5/public";
    }
    return credentials_.ws_endpoint;
}

bool OkxExchange::connect() {
    if (connected_) {
        Logger::warn("[OKX] Already connected");
        return true;
    }

    Logger::info("[OKX] Connecting...");

    // Initialize REST connection pool first (pre-establish SSL connections)
    if (!initialize_rest()) {
        Logger::error("[OKX] Failed to initialize REST connection pool");
        return false;
    }
    Logger::info("[OKX] REST connection pool initialized (4 persistent connections)");

    ws_client_ = std::make_shared<network::WebSocketClient>(io_context_, "OKX-WS");

    ws_client_->set_message_callback([this](std::string_view msg, network::MessageType /*type*/) {
        on_ws_message(msg);
    });

    ws_client_->set_connect_callback([this](bool success, const std::string& error) {
        if (success) {
            on_ws_connected();
        } else {
            Logger::error("[OKX] Connection failed: {}", error);
        }
    });

    ws_client_->set_disconnect_callback([this](const std::string& /*reason*/) {
        on_ws_disconnected();
    });

    ws_client_->connect(resolve_public_ws_endpoint());

    // Initialize WebSocket Trade API for low-latency order placement
    if (!credentials_.ws_private_endpoint.empty() && !credentials_.api_key.empty()) {
        trade_ws_ = std::make_unique<OkxTradeWS>(io_context_, credentials_);
        trade_ws_->connect();
    }

    // Connect Private WS for real-time fill data
    if (!credentials_.ws_private_endpoint.empty() && !credentials_.api_key.empty()) {
        private_ws_ = std::make_shared<network::WebSocketClient>(io_context_, "OKX-Private-WS");

        private_ws_->set_message_callback([this](std::string_view msg, network::MessageType) {
            on_private_ws_message(msg);
        });

        private_ws_->set_connect_callback([this](bool success, const std::string& error) {
            if (success) {
                Logger::info("[OKX-PrivateWS] Connected, authenticating...");
                authenticate_private_ws();
            } else {
                Logger::error("[OKX-PrivateWS] Connection failed: {}", error);
            }
        });

        private_ws_->set_disconnect_callback([this](const std::string& reason) {
            private_ws_authenticated_ = false;
            Logger::warn("[OKX-PrivateWS] Disconnected: {}", reason);
        });

        private_ws_->connect(credentials_.ws_private_endpoint);
    }

    return true;
}

void OkxExchange::disconnect() {
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
    Logger::info("[OKX] Disconnected");
}

void OkxExchange::subscribe_ticker(const std::vector<SymbolId>& symbols) {
    // Store symbols for reconnection
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_tickers_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) {
        Logger::error("[OKX] Cannot subscribe, not connected");
        return;
    }

    // OKX WS allows max 25 args per subscribe request
    constexpr size_t BATCH_SIZE = 25;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"op":"subscribe","args":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << R"({"channel":"bbo-tbt","instId":")" << symbol_to_okx(symbols[i]) << R"("})";
            first = false;
        }

        ss << "]}";
        ws_client_->send(ss.str());

        // Tiny pacing keeps startup fast without overwhelming the socket.
        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[OKX] Subscribed to {} tickers (bbo-tbt) in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);
}

void OkxExchange::subscribe_orderbook(const std::vector<SymbolId>& symbols) {
    {
        std::lock_guard lock(subscription_mutex_);
        subscribed_orderbooks_ = symbols;
    }

    if (!ws_client_ || !ws_client_->is_connected()) return;

    // OKX WS allows max 25 args per subscribe request
    constexpr size_t BATCH_SIZE = 25;

    for (size_t start = 0; start < symbols.size(); start += BATCH_SIZE) {
        size_t end = std::min(start + BATCH_SIZE, symbols.size());

        std::ostringstream ss;
        ss << R"({"op":"subscribe","args":[)";

        bool first = true;
        for (size_t i = start; i < end; ++i) {
            if (!first) ss << ",";
            ss << R"({"channel":"bbo-tbt","instId":")" << symbol_to_okx(symbols[i]) << R"("})";
            first = false;
        }

        ss << "]}";
        ws_client_->send(ss.str());

        if (end < symbols.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Logger::info("[OKX] Subscribed to {} orderbooks (bbo-tbt) in {} batches",
                 symbols.size(), (symbols.size() + BATCH_SIZE - 1) / BATCH_SIZE);
}

std::vector<SymbolId> OkxExchange::get_available_symbols() {
    std::vector<SymbolId> symbols;
    auto headers = std::unordered_map<std::string, std::string>{};

    // Step 1: Fetch MARGIN instruments to know which bases support margin short
    std::unordered_set<std::string> margin_bases;
    {
        auto margin_resp = rest_client_->get("/api/v5/public/instruments?instType=MARGIN", headers);
        if (!margin_resp.success) {
            Logger::error("[OKX] Failed to fetch MARGIN instruments: {}", margin_resp.error);
            return symbols;
        }
        try {
            simdjson::ondemand::parser margin_parser;
            simdjson::padded_string padded(margin_resp.body);
            auto doc = margin_parser.iterate(padded);
            std::string_view code = doc["code"].get_string().value();
            if (code != "0") {
                Logger::error("[OKX] MARGIN instruments API error, code: {}", code);
                return symbols;
            }
            for (auto item : doc["data"].get_array()) {
                std::string_view state = item["state"].get_string().value();
                std::string_view quote_ccy = item["quoteCcy"].get_string().value();
                if (quote_ccy != "USDT" || state != "live") continue;
                std::string_view base_ccy = item["baseCcy"].get_string().value();
                margin_bases.insert(std::string(base_ccy));
            }
        } catch (const simdjson::simdjson_error& e) {
            Logger::error("[OKX] Failed to parse MARGIN instruments: {}", e.what());
            return symbols;
        }
        Logger::info("[OKX] {} USDT margin-shortable bases", margin_bases.size());
    }

    // Step 2: Fetch SPOT instruments, filter by margin_bases
    auto response = rest_client_->get("/api/v5/public/instruments?instType=SPOT", headers);
    if (!response.success) {
        Logger::error("[OKX] Failed to fetch SPOT instruments: {}", response.error);
        return symbols;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        std::string_view code = doc["code"].get_string().value();
        if (code != "0") {
            Logger::error("[OKX] SPOT instruments API error, code: {}", code);
            return symbols;
        }

        size_t spot_total = 0;
        for (auto item : doc["data"].get_array()) {
            std::string_view inst_id = item["instId"].get_string().value();
            std::string_view state = item["state"].get_string().value();
            std::string_view quote_ccy = item["quoteCcy"].get_string().value();

            if (quote_ccy != "USDT" || state != "live") continue;
            ++spot_total;

            std::string_view base_ccy = item["baseCcy"].get_string().value();

            // Only include if margin short is possible
            if (margin_bases.find(std::string(base_ccy)) == margin_bases.end()) continue;

            symbols.emplace_back(std::string(base_ccy), "USDT");

            // Cache lot size filters
            LotSize info;
            auto lot_sz = item["lotSz"];
            if (!lot_sz.error()) {
                info.qty_step = opt::fast_stod(lot_sz.get_string().value());
            }
            auto min_sz = item["minSz"];
            if (!min_sz.error()) {
                info.min_qty = opt::fast_stod(min_sz.get_string().value());
            }

            {
                std::unique_lock lock(metadata_mutex_);
                lot_size_cache_[std::string(inst_id)] = info;
            }
        }
        Logger::info("[OKX] {} USDT spot total, {} margin-shortable → {} symbols",
                     spot_total, margin_bases.size(), symbols.size());
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[OKX] Failed to parse SPOT instruments: {}", e.what());
    }

    return symbols;
}

std::vector<Ticker> OkxExchange::fetch_all_tickers() {
    std::vector<Ticker> tickers;

    // GET /api/v5/market/tickers?instType=SPOT
    auto headers = std::unordered_map<std::string, std::string>{};
    auto response = rest_client_->get("/api/v5/market/tickers?instType=SPOT", headers);
    if (!response.success) {
        Logger::error("[OKX] Failed to fetch all tickers: {}", response.error);
        return tickers;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        std::string_view code = doc["code"].get_string().value();
        if (code != "0") {
            Logger::error("[OKX] Tickers API error, code: {}", code);
            return tickers;
        }

        auto data = doc["data"].get_array();

        for (auto item : data) {
            std::string_view inst_id = item["instId"].get_string().value();

            // Only USDT pairs: "BTC-USDT"
            auto dash = inst_id.rfind('-');
            if (dash == std::string_view::npos) continue;
            std::string_view quote = inst_id.substr(dash + 1);
            if (quote != "USDT") continue;

            std::string_view base = inst_id.substr(0, dash);

            Ticker ticker;
            ticker.exchange = Exchange::OKX;
            ticker.symbol = SymbolId(base, "USDT");

            auto last_str = item["last"];
            if (!last_str.error()) {
                ticker.last = opt::fast_stod(last_str.get_string().value());
            }

            auto bid_str = item["bidPx"];
            if (!bid_str.error()) {
                ticker.bid = opt::fast_stod(bid_str.get_string().value());
            }
            auto bid_qty = item["bidSz"];
            if (!bid_qty.error()) {
                ticker.bid_qty = opt::fast_stod(bid_qty.get_string().value());
            }

            auto ask_str = item["askPx"];
            if (!ask_str.error()) {
                ticker.ask = opt::fast_stod(ask_str.get_string().value());
            }
            auto ask_qty = item["askSz"];
            if (!ask_qty.error()) {
                ticker.ask_qty = opt::fast_stod(ask_qty.get_string().value());
            }

            ticker.timestamp = std::chrono::steady_clock::now();

            if (ticker.last > 0) {
                tickers.push_back(ticker);
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[OKX] Failed to parse all tickers: {}", e.what());
    }

    Logger::info("[OKX] Fetched {} tickers via REST", tickers.size());
    return tickers;
}

Order OkxExchange::place_market_order(const SymbolId& symbol, Side side, Quantity quantity) {
    Order order;
    order.exchange = Exchange::OKX;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    // POST /api/v5/trade/order
    char body_buf[512];
    int body_len = std::snprintf(body_buf, sizeof(body_buf),
        "{\"instId\":\"%s\",\"tdMode\":\"cross\",\"side\":\"%s\","
        "\"ordType\":\"market\",\"sz\":\"%.8f\"}",
        symbol_to_okx(symbol).c_str(),
        side == Side::Buy ? "buy" : "sell",
        quantity);
    if (body_len <= 0 || static_cast<size_t>(body_len) >= sizeof(body_buf)) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Order body buffer overflow");
        return order;
    }
    std::string body_str(body_buf, static_cast<size_t>(body_len));
    auto headers = build_auth_headers("POST", "/api/v5/trade/order", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v5/trade/order", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Order failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order, &order.order_id_str);
    return order;
}

Order OkxExchange::open_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::OKX;
    order.symbol = symbol;
    order.side = Side::Sell;
    order.type = OrderType::Market;
    double adj_qty = normalize_order_qty(symbol, quantity, true);
    if (adj_qty <= 0.0) {
        order.status = OrderStatus::Rejected;
        Logger::warn("[OKX] Short open qty invalid after lot size check: {} {}", symbol.to_string(), quantity);
        return order;
    }
    order.quantity = adj_qty;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    if (!ensure_margin_mode()) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Margin mode is not ready for {}", symbol.to_string());
        return order;
    }

    // Try WebSocket Trade API first (~5-20ms vs ~150-300ms REST)
    if (trade_ws_ && trade_ws_->is_connected()) {
        Order ws_order = trade_ws_->place_order_sync(
            symbol_to_okx(symbol), Side::Sell, adj_qty, "cross");
        if (ws_order.status != OrderStatus::Rejected) {
            ws_order.symbol = symbol;
            ws_order.quantity = adj_qty;
            ws_order.client_order_id = order.client_order_id;
            Logger::info("[OKX-WS] Opened cross-margin short {} {} - orderId: {}",
                         symbol.to_string(), adj_qty, ws_order.order_id_str);
            return ws_order;
        }
        Logger::warn("[OKX] WS order failed, falling back to REST");
    }

    // REST fallback
    char body_buf[512];
    int body_len = std::snprintf(body_buf, sizeof(body_buf),
        "{\"instId\":\"%s\",\"tdMode\":\"cross\",\"side\":\"sell\","
        "\"ordType\":\"market\",\"sz\":\"%.8f\"}",
        symbol_to_okx(symbol).c_str(), adj_qty);
    if (body_len <= 0 || static_cast<size_t>(body_len) >= sizeof(body_buf)) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Short open body buffer overflow");
        return order;
    }
    std::string body_str(body_buf, static_cast<size_t>(body_len));
    auto headers = build_auth_headers("POST", "/api/v5/trade/order", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v5/trade/order", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Cross-margin short open failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order, &order.order_id_str);
    Logger::info("[OKX-REST] Opened cross-margin short {} {} - Status: {}, orderId: {}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.order_id_str);
    return order;
}

Order OkxExchange::close_short(const SymbolId& symbol, Quantity quantity) {
    Order order;
    order.exchange = Exchange::OKX;
    order.symbol = symbol;
    order.side = Side::Buy;
    order.type = OrderType::Market;
    double adj_qty = normalize_order_qty(symbol, quantity, false);
    if (adj_qty <= 0.0) {
        order.status = OrderStatus::Rejected;
        Logger::warn("[OKX] Short close qty invalid after lot size check: {} {}", symbol.to_string(), quantity);
        return order;
    }
    order.quantity = adj_qty;
    order.client_order_id = generate_order_id();
    order.create_time = std::chrono::system_clock::now();

    if (!ensure_margin_mode()) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Margin mode is not ready for {}", symbol.to_string());
        return order;
    }

    // Try WebSocket Trade API first (~5-20ms vs ~150-300ms REST)
    if (trade_ws_ && trade_ws_->is_connected()) {
        Order ws_order = trade_ws_->place_order_sync(
            symbol_to_okx(symbol), Side::Buy, adj_qty, "cross");
        if (ws_order.status != OrderStatus::Rejected) {
            ws_order.symbol = symbol;
            ws_order.quantity = adj_qty;
            ws_order.client_order_id = order.client_order_id;
            Logger::info("[OKX-WS] Closed cross-margin short {} {} - orderId: {}",
                         symbol.to_string(), adj_qty, ws_order.order_id_str);
            return ws_order;
        }
        Logger::warn("[OKX] WS close failed, falling back to REST");
    }

    // REST fallback
    char body_buf[512];
    int body_len = std::snprintf(body_buf, sizeof(body_buf),
        "{\"instId\":\"%s\",\"tdMode\":\"cross\",\"side\":\"buy\","
        "\"ordType\":\"market\",\"sz\":\"%.8f\"}",
        symbol_to_okx(symbol).c_str(), adj_qty);
    if (body_len <= 0 || static_cast<size_t>(body_len) >= sizeof(body_buf)) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Short close body buffer overflow");
        return order;
    }
    std::string body_str(body_buf, static_cast<size_t>(body_len));
    auto headers = build_auth_headers("POST", "/api/v5/trade/order", body_str);
    headers["Content-Type"] = "application/json";

    auto response = rest_client_->post("/api/v5/trade/order", body_str, headers);
    if (!response.success) {
        order.status = OrderStatus::Rejected;
        Logger::error("[OKX] Cross-margin short close failed: {}", response.body);
        return order;
    }

    parse_order_response(response.body, order, &order.order_id_str);
    Logger::info("[OKX-REST] Closed cross-margin short {} {} - Status: {}, orderId: {}",
                 symbol.to_string(), adj_qty, static_cast<int>(order.status),
                 order.order_id_str);
    return order;
}

double OkxExchange::normalize_order_qty(const SymbolId& symbol, double qty, bool is_open) const {
    if (qty <= 0.0) return 0.0;
    const std::string key = symbol_to_okx(symbol);
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
        // e.g. 0.04/0.01 = 3.9999999996 -> floor=3 -> 0.03 (wrong!)
        // With epsilon: 3.9999999996 + 1e-9 = 4.000000000 -> floor=4 -> 0.04 (correct)
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

bool OkxExchange::cancel_order(uint64_t /*order_id*/) {
    // TODO: Implement if needed
    return false;
}

bool OkxExchange::prepare_shorting(const SymbolId& symbol) {
    (void)symbol;

    if (!ensure_margin_mode()) {
        return false;
    }

    // Force 1x cross margin so spot-short sizing matches the Korean spot leg.
    std::string base(symbol.get_base());
    char body_buf[256];
    int body_len = std::snprintf(body_buf, sizeof(body_buf),
        "{\"lever\":\"1\",\"mgnMode\":\"cross\",\"ccy\":\"%s\"}",
        base.c_str());
    if (body_len > 0 && static_cast<size_t>(body_len) < sizeof(body_buf)) {
        std::string body_str(body_buf, static_cast<size_t>(body_len));
        auto headers = build_auth_headers("POST", "/api/v5/account/set-leverage", body_str);
        headers["Content-Type"] = "application/json";

        auto response = rest_client_->post("/api/v5/account/set-leverage", body_str, headers);
        if (!response.success) {
            Logger::warn("[OKX] Failed to set leverage for {}: {}", base, response.body);
            return false;
        }

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);
            std::string_view code = doc["code"].get_string().value();
            if (code != "0") {
                auto msg = doc["msg"];
                if (!msg.error()) {
                    Logger::warn("[OKX] Failed to set leverage for {} to 1x: {}", base, msg.get_string().value());
                } else {
                    Logger::warn("[OKX] Failed to set leverage for {} to 1x: code={}", base, code);
                }
                return false;
            }
            Logger::info("[OKX] Set leverage for {} to 1x cross", base);
        } catch (const simdjson::simdjson_error& e) {
            Logger::warn("[OKX] Failed to parse leverage response for {}: {}", base, e.what());
            return false;
        }
    }

    return true;
}

std::vector<Position> OkxExchange::get_short_positions() {
    std::vector<Position> positions;

    // GET /api/v5/account/balance
    auto headers = build_auth_headers("GET", "/api/v5/account/balance", "");
    auto response = rest_client_->get("/api/v5/account/balance", headers);

    if (!response.success) {
        Logger::error("[OKX] Failed to fetch positions: {}", response.error);
        return positions;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        std::string_view code = doc["code"].get_string().value();
        if (code != "0") {
            Logger::error("[OKX] Balance API error, code: {}", code);
            return positions;
        }

        auto data = doc["data"].get_array();
        for (auto account : data) {
            auto details = account["details"].get_array();
            for (auto detail : details) {
                std::string_view ccy = detail["ccy"].get_string().value();
                if (ccy == "USDT") continue;

                // Check for negative available balance or liabilities
                double liab = 0.0;
                auto liab_field = detail["liab"];
                if (!liab_field.error()) {
                    std::string_view liab_str = liab_field.get_string().value();
                    if (!liab_str.empty()) {
                        liab = opt::fast_stod(liab_str);
                    }
                }

                // liab is negative for borrowed amounts; take absolute value
                double borrow_amount = std::abs(liab);
                if (borrow_amount <= 0.0) continue;

                Position pos;
                pos.symbol = SymbolId(std::string(ccy), "USDT");
                pos.foreign_exchange = Exchange::OKX;
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
        Logger::error("[OKX] Failed to parse positions: {}", e.what());
    }

    return positions;
}

bool OkxExchange::close_short_position(const SymbolId& symbol) {
    auto positions = get_short_positions();

    for (const auto& pos : positions) {
        if (pos.symbol == symbol && pos.foreign_amount > 0) {
            auto order = close_short(symbol, pos.foreign_amount);
            return order.status == OrderStatus::Filled;
        }
    }

    return true;  // No position to close
}

double OkxExchange::get_balance(const std::string& currency) {
    // GET /api/v5/account/balance?ccy=<currency>
    std::string path = "/api/v5/account/balance?ccy=" + currency;
    auto headers = build_auth_headers("GET", path, "");
    auto response = rest_client_->get(path, headers);

    if (!response.success) {
        Logger::error("[OKX] Failed to fetch balance: {}", response.error);
        return 0.0;
    }

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded(response.body);
        auto doc = local_parser.iterate(padded);

        std::string_view code = doc["code"].get_string().value();
        if (code != "0") {
            Logger::warn("[OKX] Balance query error, code: {}", code);
            return 0.0;
        }

        auto data = doc["data"].get_array();
        for (auto account : data) {
            auto details = account["details"].get_array();
            for (auto detail : details) {
                std::string_view ccy = detail["ccy"].get_string().value();
                if (ccy == currency) {
                    auto avail_field = detail["availBal"];
                    if (!avail_field.error()) {
                        double balance = opt::fast_stod(avail_field.get_string().value());
                        Logger::info("[OKX] {} balance: {}", currency, balance);
                        return balance;
                    }
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[OKX] Failed to parse balance: {}", e.what());
    }

    Logger::warn("[OKX] {} not found in account", currency);
    return 0.0;
}

void OkxExchange::on_ws_message(std::string_view message) {
    // OKX keepalive: literal "pong" response
    if (message == "pong") {
        return;
    }

    Ticker ticker;
    if (parse_ticker_message(message, ticker)) {
        dispatch_ticker(ticker);
    } else if (!public_ws_parse_warned_.exchange(true, std::memory_order_relaxed) &&
               message.find("bbo-tbt") != std::string_view::npos) {
        Logger::warn("[OKX-WS] Failed to parse bbo-tbt payload: {}",
                     std::string(message.substr(0, 240)));
    }
}

void OkxExchange::on_ws_connected() {
    connected_ = true;
    Logger::info("[OKX] WebSocket connected");

    std::vector<SymbolId> tickers_to_subscribe;
    std::vector<SymbolId> orderbooks_to_subscribe;
    {
        std::lock_guard lock(subscription_mutex_);
        tickers_to_subscribe = subscribed_tickers_;
        orderbooks_to_subscribe = subscribed_orderbooks_;
    }

    if (!tickers_to_subscribe.empty()) {
        Logger::info("[OKX] Resubscribing to {} tickers after reconnection", tickers_to_subscribe.size());
        subscribe_ticker(tickers_to_subscribe);
    }
    if (!orderbooks_to_subscribe.empty()) {
        Logger::info("[OKX] Resubscribing to {} orderbooks after reconnection", orderbooks_to_subscribe.size());
        subscribe_orderbook(orderbooks_to_subscribe);
    }
}

void OkxExchange::on_ws_disconnected() {
    connected_ = false;
    Logger::warn("[OKX] WebSocket disconnected");
}

std::string OkxExchange::generate_signature(const std::string& timestamp,
                                             const std::string& method,
                                             const std::string& request_path,
                                             const std::string& body) const {
    // OKX: Base64(HMAC-SHA256(timestamp + method + requestPath + body, secret))
    std::string prehash = timestamp + method + request_path + body;
    auto hmac_raw = utils::Crypto::hmac_sha256_raw(credentials_.secret_key, prehash);
    return utils::Crypto::base64_encode(hmac_raw);
}

std::unordered_map<std::string, std::string> OkxExchange::build_auth_headers(
    const std::string& method,
    const std::string& request_path,
    const std::string& body) const {

    std::string timestamp = generate_iso_timestamp();
    std::string signature = generate_signature(timestamp, method, request_path, body);

    return {
        {"OK-ACCESS-KEY", credentials_.api_key},
        {"OK-ACCESS-SIGN", signature},
        {"OK-ACCESS-TIMESTAMP", timestamp},
        {"OK-ACCESS-PASSPHRASE", credentials_.passphrase}
    };
}

bool OkxExchange::ensure_margin_mode() {
    if (margin_mode_ready_.load(std::memory_order_acquire)) {
        return true;
    }
    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Logger::error("[OKX] API credentials are required for margin orders");
        return false;
    }

    std::lock_guard lock(margin_mode_mutex_);
    if (margin_mode_ready_.load(std::memory_order_relaxed)) {
        return true;
    }

    // Check account config: GET /api/v5/account/config
    {
        auto headers = build_auth_headers("GET", "/api/v5/account/config", "");
        auto response = rest_client_->get("/api/v5/account/config", headers);
        if (response.success) {
            try {
                simdjson::ondemand::parser local_parser;
                simdjson::padded_string padded(response.body);
                auto doc = local_parser.iterate(padded);

                std::string_view code = doc["code"].get_string().value();
                if (code == "0") {
                    auto data = doc["data"].get_array();
                    for (auto item : data) {
                        auto acct_lv = item["acctLv"];
                        if (!acct_lv.error()) {
                            std::string_view lv = acct_lv.get_string().value();
                            int level = std::atoi(std::string(lv).c_str());
                            if (level >= 2) {
                                // Multi-currency margin or portfolio margin — cross margin available
                                margin_mode_ready_.store(true, std::memory_order_release);
                                Logger::info("[OKX] Account level {} supports cross margin", level);
                                return true;
                            } else {
                                Logger::error("[OKX] Account level {} does not support cross margin (need >= 2)", level);
                                return false;
                            }
                        }
                    }
                }
            } catch (const simdjson::simdjson_error& e) {
                Logger::warn("[OKX] Failed to parse account config: {}", e.what());
            }
        } else {
            Logger::warn("[OKX] Failed to fetch account config: {}", response.error);
        }
    }

    // If we got here without a definitive answer, assume OK and let order placement fail if not
    margin_mode_ready_.store(true, std::memory_order_release);
    Logger::info("[OKX] Margin mode assumed ready (will fail on order if not configured)");
    return true;
}

bool OkxExchange::parse_ticker_message(std::string_view message, Ticker& ticker) {
    // Try fast BBO parser first (bbo-tbt channel)
    if (message.find(R"("channel":"bbo-tbt")") != std::string_view::npos) {
        if (parse_bbo_fast(message, ticker)) {
            auto cached = get_cached_ticker(ticker.symbol);
            if (cached && cached->last > 0.0) {
                ticker.last = cached->last;
            } else {
                ticker.last = (ticker.bid + ticker.ask) * 0.5;
            }
            return true;
        }
    }

    // Fallback: full simdjson parse for tickers channel or other formats
    try {
        simdjson::dom::parser local_parser;
        simdjson::padded_string padded(message);
        auto doc = local_parser.parse(padded);

        // OKX push format: {"arg":{"channel":"...","instId":"..."},"data":[{...}]}
        auto arg = doc["arg"];
        if (arg.error()) return false;

        auto channel = arg["channel"];
        if (channel.error()) return false;
        std::string_view channel_str = std::string_view(channel.get_c_str().value());

        auto inst_id_elem = arg["instId"];
        if (inst_id_elem.error()) return false;
        std::string_view inst_id = std::string_view(inst_id_elem.get_c_str().value());

        // Parse "BTC-USDT" -> SymbolId
        auto dash = inst_id.find('-');
        if (dash == std::string_view::npos) return false;
        std::string_view base = inst_id.substr(0, dash);
        std::string_view quote = inst_id.substr(dash + 1);
        if (quote != "USDT") return false;

        ticker.exchange = Exchange::OKX;
        ticker.timestamp = std::chrono::steady_clock::now();
        ticker.symbol = SymbolId(base, quote);

        auto data_arr = doc["data"];
        if (data_arr.error()) return false;
        auto arr = data_arr.get_array().value();
        if (arr.size() == 0) return false;
        auto data = arr.at(0);

        if (channel_str == "bbo-tbt") {
            // BBO format: bids/asks arrays
            auto bids = data["bids"];
            auto asks = data["asks"];
            if (bids.error() || asks.error()) return false;

            auto bid_rows = bids.get_array().value();
            auto ask_rows = asks.get_array().value();
            if (bid_rows.size() == 0 || ask_rows.size() == 0) return false;

            auto bid_row = bid_rows.at(0).get_array().value();
            auto ask_row = ask_rows.at(0).get_array().value();
            if (bid_row.size() < 2 || ask_row.size() < 2) return false;

            ticker.bid = opt::fast_stod(bid_row.at(0).get_c_str().value());
            ticker.bid_qty = opt::fast_stod(bid_row.at(1).get_c_str().value());
            ticker.ask = opt::fast_stod(ask_row.at(0).get_c_str().value());
            ticker.ask_qty = opt::fast_stod(ask_row.at(1).get_c_str().value());

            auto cached = get_cached_ticker(ticker.symbol);
            if (cached && cached->last > 0.0) {
                ticker.last = cached->last;
            } else {
                ticker.last = (ticker.bid + ticker.ask) * 0.5;
            }

            return ticker.bid > 0.0 && ticker.ask > 0.0;
        }

        if (channel_str == "tickers") {
            auto last_elem = data["last"];
            if (!last_elem.error()) {
                ticker.last = opt::fast_stod(last_elem.get_c_str().value());
            }
            auto bid_elem = data["bidPx"];
            if (!bid_elem.error()) {
                ticker.bid = opt::fast_stod(bid_elem.get_c_str().value());
            }
            auto bid_qty_elem = data["bidSz"];
            if (!bid_qty_elem.error()) {
                ticker.bid_qty = opt::fast_stod(bid_qty_elem.get_c_str().value());
            }
            auto ask_elem = data["askPx"];
            if (!ask_elem.error()) {
                ticker.ask = opt::fast_stod(ask_elem.get_c_str().value());
            }
            auto ask_qty_elem = data["askSz"];
            if (!ask_qty_elem.error()) {
                ticker.ask_qty = opt::fast_stod(ask_qty_elem.get_c_str().value());
            }
            return ticker.last > 0.0 && ticker.bid > 0.0 && ticker.ask > 0.0;
        }

        return false;
    } catch (const simdjson::simdjson_error& e) {
        return false;
    }
}

bool OkxExchange::parse_order_response(const std::string& response, Order& order, std::string* order_id_out) {
    Logger::debug("[OKX] Order raw response: {}", response);

    try {
        simdjson::ondemand::parser local_parser;
        simdjson::padded_string padded_response(response);
        auto doc = local_parser.iterate(padded_response);

        std::string_view code = doc["code"].get_string().value();
        if (code == "0") {
            // Check per-order status in data[0]
            auto data = doc["data"].get_array();
            for (auto item : data) {
                auto s_code = item["sCode"];
                if (!s_code.error()) {
                    std::string_view sc = s_code.get_string().value();
                    if (sc != "0") {
                        order.status = OrderStatus::Rejected;
                        auto s_msg = item["sMsg"];
                        if (!s_msg.error()) {
                            Logger::error("[OKX] Order rejected: {}", s_msg.get_string().value());
                        }
                        return true;
                    }
                }

                order.status = OrderStatus::Filled;  // Assume filled for market orders

                auto ord_id = item["ordId"];
                if (!ord_id.error()) {
                    std::string_view order_id = ord_id.get_string().value();
                    order.exchange_order_id = std::hash<std::string_view>{}(order_id);
                    if (order_id_out) {
                        *order_id_out = std::string(order_id);
                    }
                }
                break;  // Only first element
            }
        } else {
            order.status = OrderStatus::Rejected;
            auto msg = doc["msg"];
            if (!msg.error()) {
                Logger::error("[OKX] Order rejected: {}", msg.get_string().value());
            }
        }

        return true;
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[OKX] Failed to parse order response: {}", e.what());
        order.status = OrderStatus::Rejected;
        return false;
    }
}

bool OkxExchange::query_order_fill(const std::string& order_id, const SymbolId& symbol, Order& order) {
    // Try WS fill cache first (~50-200ms vs ~300ms+ REST)
    if (private_ws_authenticated_.load()) {
        std::unique_lock lock(fill_cache_mutex_);

        // Check immediately
        auto it = fill_cache_.find(order_id);
        if (it != fill_cache_.end()) {
            order.average_price = it->second.avg_price;
            order.filled_quantity = it->second.filled_qty;
            fill_cache_.erase(it);
            Logger::info("[OKX-WS] Fill: orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                         order_id, order.average_price, order.filled_quantity);
            return true;
        }

        // Wait up to 200ms for fill event from Private WS
        bool found = fill_cache_cv_.wait_for(lock, std::chrono::milliseconds(200), [&]() {
            return fill_cache_.find(order_id) != fill_cache_.end();
        });

        if (found) {
            it = fill_cache_.find(order_id);
            order.average_price = it->second.avg_price;
            order.filled_quantity = it->second.filled_qty;
            fill_cache_.erase(it);
            Logger::info("[OKX-WS] Fill (waited): orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                         order_id, order.average_price, order.filled_quantity);
            return true;
        }

        Logger::warn("[OKX] WS fill cache miss for {}, falling back to REST", order_id);
    }

    // REST fallback: GET /api/v5/trade/order?instId=XXX-USDT&ordId=xxx
    constexpr int MAX_RETRIES = 5;
    constexpr int BASE_DELAY_MS = 300;

    std::string inst_id = symbol_to_okx(symbol);
    std::string path = "/api/v5/trade/order?instId=" + inst_id + "&ordId=" + order_id;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        if (attempt > 0) {
            int delay = BASE_DELAY_MS * (1 << (attempt - 1));
            Logger::warn("[OKX] Fill query retry {}/{} for order {} (wait {}ms)",
                         attempt + 1, MAX_RETRIES, order_id, delay);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        auto headers = build_auth_headers("GET", path, "");
        auto response = rest_client_->get(path, headers);

        if (!response.success) {
            Logger::warn("[OKX] Failed to query order fill: {}", response.error);
            continue;
        }

        Logger::debug("[OKX] Fill query raw: {}", response.body);

        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string padded(response.body);
            auto doc = local_parser.iterate(padded);

            std::string_view code = doc["code"].get_string().value();
            if (code != "0") {
                Logger::warn("[OKX] Order fill query error, code: {}", code);
                continue;
            }

            auto data = doc["data"].get_array();
            for (auto item : data) {
                // OKX fields: avgPx, accFillSz, state
                auto avg_px = item["avgPx"];
                if (!avg_px.error()) {
                    std::string_view p = avg_px.get_string().value();
                    double price = opt::fast_stod(p);
                    if (price > 0) order.average_price = price;
                }
                auto acc_fill = item["accFillSz"];
                if (!acc_fill.error()) {
                    std::string_view q = acc_fill.get_string().value();
                    double qty = opt::fast_stod(q);
                    if (qty > 0) order.filled_quantity = qty;
                }

                if (order.average_price > 0 && order.filled_quantity > 0) {
                    Logger::info("[OKX-REST] Fill: orderId={}, avgPrice={:.8f}, filledQty={:.8f}",
                                 order_id, order.average_price, order.filled_quantity);
                    return true;
                }
                break;
            }

            Logger::warn("[OKX] Order {} fill data incomplete, attempt {}/{}",
                         order_id, attempt + 1, MAX_RETRIES);
        } catch (const simdjson::simdjson_error& e) {
            Logger::warn("[OKX] Failed to parse order fill: {}", e.what());
        }
    }

    Logger::error("[OKX] Failed to get fill price after {} retries for order {}",
                  MAX_RETRIES, order_id);
    return false;
}

void OkxExchange::authenticate_private_ws() {
    // OKX WS auth: sign = Base64(HMAC-SHA256(timestamp + "GET" + "/users/self/verify", secret))
    int64_t ts = utils::Crypto::timestamp_sec();
    std::string ts_str = std::to_string(ts);
    std::string sign_payload = ts_str + "GET" + "/users/self/verify";

    auto hmac_raw = utils::Crypto::hmac_sha256_raw(credentials_.secret_key, sign_payload);
    std::string signature = utils::Crypto::base64_encode(hmac_raw);

    char buf[512];
    int len = std::snprintf(buf, sizeof(buf),
        "{\"op\":\"login\",\"args\":[{\"apiKey\":\"%s\",\"passphrase\":\"%s\","
        "\"timestamp\":\"%s\",\"sign\":\"%s\"}]}",
        credentials_.api_key.c_str(),
        credentials_.passphrase.c_str(),
        ts_str.c_str(),
        signature.c_str());
    if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
        private_ws_->send(std::string(buf, static_cast<size_t>(len)));
    }
}

void OkxExchange::on_private_ws_message(std::string_view message) {
    // OKX keepalive: literal "pong" response
    if (message == "pong") {
        return;
    }

    try {
        simdjson::padded_string padded(message);
        auto doc = json_parser_.iterate(padded);

        // Login response: {"event":"login","code":"0"}
        auto event_field = doc["event"];
        if (!event_field.error()) {
            std::string_view event_str = event_field.get_string().value();
            if (event_str == "login") {
                auto code_field = doc["code"];
                if (!code_field.error()) {
                    std::string_view code_str = code_field.get_string().value();
                    if (code_str == "0") {
                        private_ws_authenticated_ = true;
                        // Subscribe to orders channel
                        private_ws_->send(std::string(
                            R"({"op":"subscribe","args":[{"channel":"orders","instType":"SPOT"}]})"));
                        Logger::info("[OKX-PrivateWS] Authenticated, subscribed to orders stream");
                    } else {
                        Logger::error("[OKX-PrivateWS] Authentication failed, code: {}", code_str);
                    }
                }
            }
            return;
        }

        // Order update push: {"arg":{"channel":"orders","instType":"SPOT"},"data":[{...}]}
        auto arg = doc["arg"];
        if (!arg.error()) {
            auto channel = arg["channel"];
            if (!channel.error()) {
                std::string_view ch = channel.get_string().value();
                if (ch == "orders") {
                    auto data = doc["data"].get_array();
                    for (auto item : data) {
                        auto state_field = item["state"];
                        if (state_field.error()) continue;
                        std::string_view state = state_field.get_string().value();

                        if (state == "filled") {
                            auto id_field = item["ordId"];
                            if (id_field.error()) continue;
                            std::string order_id(id_field.get_string().value());

                            FillInfo fill;
                            auto avg = item["avgPx"];
                            if (!avg.error()) fill.avg_price = opt::fast_stod(avg.get_string().value());
                            auto qty = item["accFillSz"];
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
        }
    } catch (const simdjson::simdjson_error&) {
        // Ignore parse errors for non-JSON messages
    }
}

// ── Deposit Network Fetcher ─────────────────────────────────────────────

std::unordered_map<std::string, std::unordered_set<std::string>>
OkxExchange::fetch_deposit_networks() {
    std::unordered_map<std::string, std::unordered_set<std::string>> result;

    if (credentials_.api_key.empty() || credentials_.secret_key.empty()) {
        Logger::warn("[OKX] No API credentials — cannot fetch deposit networks");
        return result;
    }

    auto headers = build_auth_headers("GET", "/api/v5/asset/currencies");
    auto response = rest_client_->get("/api/v5/asset/currencies", headers);
    if (!response.success) {
        Logger::error("[OKX] Failed to fetch currencies: {}", response.error);
        return result;
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

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response.body);
        auto doc = parser.iterate(padded);

        auto code = doc["code"].get_string();
        if (code.error() || code.value() != "0") {
            Logger::error("[OKX] currencies returned non-zero code");
            return result;
        }

        // Response: {"code":"0","data":[{"ccy":"BTC","chain":"BTC-Bitcoin","canDep":true,...}]}
        // Multi-chain coins appear as separate entries with different "chain" values.
        auto data = doc["data"].get_array();
        if (data.error()) return result;

        for (auto item : data.value()) {
            auto ccy = item["ccy"].get_string();
            if (ccy.error()) continue;

            // Check deposit enabled (can be bool or string "true"/"false")
            bool deposit_ok = false;
            auto can_dep_bool = item["canDep"].get_bool();
            if (!can_dep_bool.error()) {
                deposit_ok = can_dep_bool.value();
            } else {
                auto can_dep_str = item["canDep"].get_string();
                if (!can_dep_str.error()) {
                    deposit_ok = (can_dep_str.value() == "true" || can_dep_str.value() == "1");
                }
            }

            if (!deposit_ok) continue;

            // Extract chain name: OKX format is "BTC-Bitcoin", "ETH-ERC20", etc.
            // Normalize by taking the part after '-' if it exists, else the whole thing.
            std::string chain_name;
            auto chain_val = item["chain"].get_string();
            if (!chain_val.error()) {
                std::string_view chain_sv = chain_val.value();
                auto dash = chain_sv.find('-');
                if (dash != std::string_view::npos) {
                    chain_name = normalize(chain_sv.substr(dash + 1));
                } else {
                    chain_name = normalize(chain_sv);
                }
            }

            if (!chain_name.empty()) {
                result[std::string(ccy.value())].insert(std::move(chain_name));
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        Logger::error("[OKX] Failed to parse currencies: {}", e.what());
    }

    Logger::info("[OKX] Loaded deposit networks for {} coins", result.size());
    return result;
}

} // namespace kimp::exchange::okx
