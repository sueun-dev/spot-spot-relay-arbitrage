/**
 * Test: Fill Price Parsing (체결가 파싱 검증)
 *
 * 실제 주문 없이 JSON 파싱 로직만 검증:
 * 1. Bybit parse_order_response → orderId 추출
 * 2. Bybit query_order_fill 응답 파싱 → avgPrice, cumExecQty
 * 3. Bithumb 주문 응답 → order_id 추출
 * 4. Bithumb query_order_detail 응답 → contract 배열 → VWAP 계산
 * 5. 엣지 케이스: 멀티 fill, 필드 누락, 에러 응답
 * 6. order_manager 에서 fallback 대신 실제 fill 값 사용 확인
 */

#include "kimp/core/types.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/logger.hpp"

#include <simdjson.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>

using namespace kimp;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; ++tests_passed;

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; ++tests_failed;

#define ASSERT_EQ(a, b, msg) \
    if ((a) != (b)) { FAIL(fmt::format("{}: {} != {}", msg, a, b)); return; }

#define ASSERT_NEAR(a, b, eps, msg) \
    if (std::abs((a) - (b)) > (eps)) { FAIL(fmt::format("{}: {:.8f} != {:.8f}", msg, (double)(a), (double)(b))); return; }

#define ASSERT_TRUE(cond, msg) \
    if (!(cond)) { FAIL(msg); return; }

// ============================================================
// Bybit parse_order_response 로직 재현
// (bybit.cpp:670-698 과 동일)
// ============================================================
struct BybitParseResult {
    bool success{false};
    OrderStatus status{OrderStatus::Rejected};
    std::string order_id;
    uint64_t exchange_order_id{0};
};

BybitParseResult parse_bybit_order_response(const std::string& response) {
    BybitParseResult result;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
        if (ret_code == 0) {
            result.status = OrderStatus::Filled;
            auto res = doc["result"];
            std::string_view order_id = res["orderId"].get_string().value();
            result.exchange_order_id = std::hash<std::string_view>{}(order_id);
            result.order_id = std::string(order_id);
            result.success = true;
        } else {
            result.status = OrderStatus::Rejected;
            result.success = true;
        }
    } catch (...) {
        result.success = false;
    }
    return result;
}

// ============================================================
// Bybit query_order_fill 응답 파싱 로직 재현
// (bybit.cpp:700-747 과 동일)
// ============================================================
struct FillData {
    double average_price{0.0};
    double filled_quantity{0.0};
    bool success{false};
};

FillData parse_bybit_fill_response(const std::string& response) {
    FillData fill;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        int ret_code = static_cast<int>(doc["retCode"].get_int64().value());
        if (ret_code != 0) return fill;

        auto list = doc["result"]["list"].get_array();
        for (auto item : list) {
            auto avg_price = item["avgPrice"];
            if (!avg_price.error()) {
                std::string_view p = avg_price.get_string().value();
                double price = opt::fast_stod(p);
                if (price > 0) fill.average_price = price;
            }
            auto cum_qty = item["cumExecQty"];
            if (!cum_qty.error()) {
                std::string_view q = cum_qty.get_string().value();
                double qty = opt::fast_stod(q);
                if (qty > 0) fill.filled_quantity = qty;
            }
            break;
        }
        fill.success = fill.average_price > 0 && fill.filled_quantity > 0;
    } catch (...) {
        fill.success = false;
    }
    return fill;
}

// ============================================================
// Bithumb 주문 응답에서 order_id 추출 로직 재현
// (bithumb.cpp:338-355 과 동일)
// ============================================================
struct BithumbOrderResult {
    bool success{false};
    OrderStatus status{OrderStatus::Rejected};
    std::string order_id;
};

BithumbOrderResult parse_bithumb_order_response(const std::string& response) {
    BithumbOrderResult result;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        std::string_view status = doc["status"].get_string().value();
        if (status == "0000") {
            result.status = OrderStatus::Filled;
            auto oid = doc["order_id"];
            if (!oid.error()) {
                result.order_id = std::string(oid.get_string().value());
            }
            result.success = true;
        } else {
            result.status = OrderStatus::Rejected;
            result.success = true;
        }
    } catch (...) {
        result.success = false;
    }
    return result;
}

// ============================================================
// Bithumb query_order_detail 응답 파싱 로직 재현
// (bithumb.cpp:664-724 과 동일)
// ============================================================
FillData parse_bithumb_detail_response(const std::string& response) {
    FillData fill;
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(response);
        auto doc = parser.iterate(padded);

        std::string_view status = doc["status"].get_string().value();
        if (status != "0000") return fill;

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
            fill.filled_quantity = total_units;
            fill.average_price = total_cost / total_units;
            fill.success = true;
        }
    } catch (...) {
        fill.success = false;
    }
    return fill;
}

// ============================================================
// Order fallback 로직 재현
// (order_manager.cpp:257-258 과 동일)
// ============================================================
struct FallbackResult {
    double actual_filled;
    double actual_price;
};

FallbackResult apply_fallback(double filled_quantity, double average_price,
                               double fallback_quantity, double fallback_price) {
    FallbackResult r;
    r.actual_filled = filled_quantity > 0 ? filled_quantity : fallback_quantity;
    r.actual_price = average_price > 0 ? average_price : fallback_price;
    return r;
}

// ============================================================
// Tests
// ============================================================

void test_bybit_parse_order_success() {
    TEST("Bybit parse_order_response - 정상 응답") {
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "orderId": "1712345678901234567",
                "orderLinkId": ""
            }
        })";

        auto result = parse_bybit_order_response(json);
        ASSERT_TRUE(result.success, "parse should succeed");
        ASSERT_EQ(static_cast<int>(result.status), static_cast<int>(OrderStatus::Filled), "should be Filled");
        ASSERT_EQ(result.order_id, "1712345678901234567", "orderId should match");
        ASSERT_TRUE(result.exchange_order_id != 0, "hash should be non-zero");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_parse_order_rejected() {
    TEST("Bybit parse_order_response - 거부 응답") {
        std::string json = R"({
            "retCode": 10001,
            "retMsg": "Insufficient balance",
            "result": {}
        })";

        auto result = parse_bybit_order_response(json);
        ASSERT_TRUE(result.success, "parse should succeed");
        ASSERT_EQ(static_cast<int>(result.status), static_cast<int>(OrderStatus::Rejected), "should be Rejected");
        ASSERT_TRUE(result.order_id.empty(), "orderId should be empty");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_parse_order_invalid_json() {
    TEST("Bybit parse_order_response - 잘못된 JSON") {
        std::string json = "not valid json at all";

        auto result = parse_bybit_order_response(json);
        ASSERT_TRUE(!result.success, "parse should fail");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_fill_normal() {
    TEST("Bybit query_order_fill - 정상 fill (단일 체결)") {
        // Realistic Bybit /v5/order/realtime response for a filled market order
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "list": [{
                    "orderId": "1712345678901234567",
                    "symbol": "ETHUSDT",
                    "side": "Sell",
                    "orderType": "Market",
                    "qty": "0.01",
                    "price": "",
                    "avgPrice": "2718.35",
                    "cumExecQty": "0.01",
                    "cumExecValue": "27.1835",
                    "orderStatus": "Filled",
                    "timeInForce": "GTC",
                    "reduceOnly": false
                }]
            }
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(fill.success, "fill parse should succeed");
        ASSERT_NEAR(fill.average_price, 2718.35, 0.01, "avgPrice");
        ASSERT_NEAR(fill.filled_quantity, 0.01, 0.0001, "filledQty");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_fill_btc() {
    TEST("Bybit query_order_fill - BTC 소수점 8자리") {
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "list": [{
                    "orderId": "9999999999",
                    "symbol": "BTCUSDT",
                    "side": "Sell",
                    "orderType": "Market",
                    "qty": "0.00025800",
                    "avgPrice": "96842.10",
                    "cumExecQty": "0.00025800",
                    "cumExecValue": "24.985",
                    "orderStatus": "Filled"
                }]
            }
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(fill.success, "fill parse should succeed");
        ASSERT_NEAR(fill.average_price, 96842.10, 0.01, "avgPrice");
        ASSERT_NEAR(fill.filled_quantity, 0.000258, 0.00000001, "filledQty");

        // Verify actual value: 0.000258 * 96842.10 = $24.985
        double value = fill.filled_quantity * fill.average_price;
        ASSERT_NEAR(value, 24.985, 0.01, "fill value ~$25");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_fill_error_response() {
    TEST("Bybit query_order_fill - 에러 응답 (retCode != 0)") {
        std::string json = R"({
            "retCode": 10001,
            "retMsg": "Order not found",
            "result": {"list": []}
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(!fill.success, "should fail on error retCode");
        ASSERT_NEAR(fill.average_price, 0.0, 0.001, "price should be 0");
        ASSERT_NEAR(fill.filled_quantity, 0.0, 0.001, "qty should be 0");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_fill_empty_list() {
    TEST("Bybit query_order_fill - 빈 list (주문 아직 미체결)") {
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "list": []
            }
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(!fill.success, "should fail on empty list");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_fill_partial() {
    TEST("Bybit query_order_fill - 부분 체결") {
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "list": [{
                    "orderId": "111222333",
                    "symbol": "SOLUSDT",
                    "avgPrice": "198.45",
                    "cumExecQty": "0.08",
                    "orderStatus": "PartiallyFilled"
                }]
            }
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(fill.success, "partial fill should still parse");
        ASSERT_NEAR(fill.average_price, 198.45, 0.01, "avgPrice");
        ASSERT_NEAR(fill.filled_quantity, 0.08, 0.0001, "partial qty");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_parse_order_success() {
    TEST("Bithumb 주문 응답 - 정상 (order_id 추출)") {
        std::string json = R"({
            "status": "0000",
            "order_id": "1234567890",
            "message": "success"
        })";

        auto result = parse_bithumb_order_response(json);
        ASSERT_TRUE(result.success, "parse should succeed");
        ASSERT_EQ(static_cast<int>(result.status), static_cast<int>(OrderStatus::Filled), "should be Filled");
        ASSERT_EQ(result.order_id, "1234567890", "order_id should match");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_parse_order_rejected() {
    TEST("Bithumb 주문 응답 - 거부 (잔고 부족 등)") {
        std::string json = R"({
            "status": "5300",
            "message": "Invalid Parameter"
        })";

        auto result = parse_bithumb_order_response(json);
        ASSERT_TRUE(result.success, "parse should succeed");
        ASSERT_EQ(static_cast<int>(result.status), static_cast<int>(OrderStatus::Rejected), "should be Rejected");
        ASSERT_TRUE(result.order_id.empty(), "no order_id");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_parse_order_no_order_id() {
    TEST("Bithumb 주문 응답 - order_id 필드 없음") {
        std::string json = R"({
            "status": "0000",
            "message": "success"
        })";

        auto result = parse_bithumb_order_response(json);
        ASSERT_TRUE(result.success, "parse should succeed");
        ASSERT_EQ(static_cast<int>(result.status), static_cast<int>(OrderStatus::Filled), "should be Filled");
        ASSERT_TRUE(result.order_id.empty(), "order_id should be empty (field missing)");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_detail_single_fill() {
    TEST("Bithumb order_detail - 단일 체결 (ETH 매수)") {
        // /info/order_detail response: 1 contract at 3,920,000 KRW for 0.00638 ETH
        std::string json = R"({
            "status": "0000",
            "data": {
                "order_id": "1234567890",
                "type": "bid",
                "order_currency": "ETH",
                "payment_currency": "KRW",
                "order_date": "1706745600000",
                "units": "0.00638000",
                "units_remaining": "0",
                "contract": [{
                    "transaction_date": "1706745600123",
                    "price": "3920000",
                    "units": "0.00638000",
                    "fee_currency": "KRW",
                    "fee": "1",
                    "total": "25009"
                }]
            }
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(fill.success, "detail parse should succeed");
        ASSERT_NEAR(fill.average_price, 3920000.0, 1.0, "avgPrice KRW");
        ASSERT_NEAR(fill.filled_quantity, 0.00638, 0.00001, "filledQty");

        // Verify value: 0.00638 * 3,920,000 = 25,009.6 KRW ≈ $25
        double value_krw = fill.filled_quantity * fill.average_price;
        ASSERT_NEAR(value_krw, 25009.6, 10.0, "fill value ~25,000 KRW");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_detail_multi_fill() {
    TEST("Bithumb order_detail - 다중 체결 (XRP 매도, 2개 호가에서 체결)") {
        // Market sell split across 2 price levels
        // 5.0 XRP @ 3,610 KRW + 3.0 XRP @ 3,605 KRW
        // VWAP = (5*3610 + 3*3605) / 8 = (18050 + 10815) / 8 = 28865 / 8 = 3608.125
        std::string json = R"({
            "status": "0000",
            "data": {
                "order_id": "9876543210",
                "type": "ask",
                "order_currency": "XRP",
                "payment_currency": "KRW",
                "units": "8.00000000",
                "units_remaining": "0",
                "contract": [
                    {
                        "transaction_date": "1706745600100",
                        "price": "3610",
                        "units": "5.00000000",
                        "fee_currency": "KRW",
                        "fee": "7",
                        "total": "18050"
                    },
                    {
                        "transaction_date": "1706745600101",
                        "price": "3605",
                        "units": "3.00000000",
                        "fee_currency": "KRW",
                        "fee": "4",
                        "total": "10815"
                    }
                ]
            }
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(fill.success, "multi-fill parse should succeed");
        ASSERT_NEAR(fill.filled_quantity, 8.0, 0.0001, "total qty = 5 + 3");
        ASSERT_NEAR(fill.average_price, 3608.125, 0.01, "VWAP = (5*3610 + 3*3605) / 8");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_detail_three_fills() {
    TEST("Bithumb order_detail - 3단 체결 (BTC 매수)") {
        // 0.00010 BTC @ 140,050,000 + 0.00008 BTC @ 140,060,000 + 0.00007 BTC @ 140,070,000
        // Total: 0.00025 BTC
        // VWAP = (10*140050000 + 8*140060000 + 7*140070000) / 25
        //      = (1400500000 + 1120480000 + 980490000) / 25
        //      = 3501470000 / 25 = 140058800
        std::string json = R"({
            "status": "0000",
            "data": {
                "order_id": "5555555555",
                "type": "bid",
                "order_currency": "BTC",
                "payment_currency": "KRW",
                "units": "0.00025000",
                "units_remaining": "0",
                "contract": [
                    {"price": "140050000", "units": "0.00010000", "fee": "56", "fee_currency": "KRW", "total": "14005"},
                    {"price": "140060000", "units": "0.00008000", "fee": "45", "fee_currency": "KRW", "total": "11205"},
                    {"price": "140070000", "units": "0.00007000", "fee": "39", "fee_currency": "KRW", "total": "9805"}
                ]
            }
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(fill.success, "3-fill parse should succeed");
        ASSERT_NEAR(fill.filled_quantity, 0.00025, 0.0000001, "total qty");

        double expected_vwap = (0.00010 * 140050000.0 + 0.00008 * 140060000.0 + 0.00007 * 140070000.0) / 0.00025;
        ASSERT_NEAR(fill.average_price, expected_vwap, 1.0, "VWAP calculation");

        // Value should be ~$25 at USDT=1380
        double value_krw = fill.filled_quantity * fill.average_price;
        ASSERT_NEAR(value_krw, 35015.0, 100.0, "fill value KRW");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_detail_error_status() {
    TEST("Bithumb order_detail - 에러 응답") {
        std::string json = R"({
            "status": "5000",
            "message": "Bad Request"
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(!fill.success, "should fail on error status");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_detail_empty_contract() {
    TEST("Bithumb order_detail - contract 배열 비어있음") {
        std::string json = R"({
            "status": "0000",
            "data": {
                "order_id": "111",
                "type": "bid",
                "contract": []
            }
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(!fill.success, "should fail on empty contract");
        ASSERT_NEAR(fill.filled_quantity, 0.0, 0.001, "qty 0");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_fallback_with_real_fill() {
    TEST("Fallback 로직 - fill 데이터 있을 때 실제값 사용") {
        // Simulating: order.filled_quantity = 0.01, order.average_price = 2718.35
        double filled_qty = 0.01;
        double avg_price = 2718.35;
        double fallback_qty = 0.0092;     // cached coin_amount
        double fallback_price = 2720.00;  // cached foreign_bid

        auto result = apply_fallback(filled_qty, avg_price, fallback_qty, fallback_price);
        ASSERT_NEAR(result.actual_filled, 0.01, 0.0001, "should use real filled qty");
        ASSERT_NEAR(result.actual_price, 2718.35, 0.01, "should use real avg price");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_fallback_without_fill() {
    TEST("Fallback 로직 - fill 데이터 없을 때 캐시 가격 사용") {
        // Simulating: order.filled_quantity = 0, order.average_price = 0 (fill query failed)
        double filled_qty = 0.0;
        double avg_price = 0.0;
        double fallback_qty = 0.0092;
        double fallback_price = 2720.00;

        auto result = apply_fallback(filled_qty, avg_price, fallback_qty, fallback_price);
        ASSERT_NEAR(result.actual_filled, 0.0092, 0.0001, "should use fallback qty");
        ASSERT_NEAR(result.actual_price, 2720.00, 0.01, "should use fallback price");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_fill_price_difference_impact() {
    TEST("체결가 차이 → P&L 영향 시뮬레이션") {
        // Scenario: Entry via 10 splits of $25
        // Cached price: 2720.00 USDT (what fallback would use)
        // Actual fill prices vary slightly per split
        double cached_price = 2720.00;
        double usdt_krw = 1380.0;

        // Simulated actual fills (slightly different from cached)
        struct SplitFill {
            double price;
            double qty;
        };
        std::vector<SplitFill> fills = {
            {2718.50, 0.00920},
            {2719.10, 0.00919},
            {2718.80, 0.00920},
            {2719.50, 0.00919},
            {2720.20, 0.00918},
            {2718.90, 0.00920},
            {2719.30, 0.00919},
            {2720.00, 0.00918},
            {2719.70, 0.00919},
            {2718.60, 0.00920},
        };

        // Calculate VWAP across all splits
        double total_value = 0.0;
        double total_qty = 0.0;
        for (const auto& f : fills) {
            total_value += f.price * f.qty;
            total_qty += f.qty;
        }
        double actual_vwap = total_value / total_qty;

        // Price difference
        double price_diff = actual_vwap - cached_price;
        double price_diff_pct = (price_diff / cached_price) * 100.0;

        // P&L impact for total position (~$250 worth)
        double position_usd = total_qty * actual_vwap;
        double pnl_impact_usd = total_qty * price_diff;
        double pnl_impact_krw = pnl_impact_usd * usdt_krw;

        std::cout << "\n";
        std::cout << fmt::format("    Cached price:  {:.2f} USDT\n", cached_price);
        std::cout << fmt::format("    Actual VWAP:   {:.2f} USDT\n", actual_vwap);
        std::cout << fmt::format("    Diff:          {:.2f} USDT ({:.4f}%)\n", price_diff, price_diff_pct);
        std::cout << fmt::format("    Position:      ${:.2f}\n", position_usd);
        std::cout << fmt::format("    P&L impact:    ${:.4f} ({:.0f} KRW)\n", pnl_impact_usd, pnl_impact_krw);
        std::cout << "    ";

        // Verify the VWAP is close but not identical to cached
        ASSERT_TRUE(std::abs(price_diff) < 2.0, "fill price should be close to cached (~$2 max)");
        ASSERT_TRUE(actual_vwap != cached_price, "fill price should differ from cached");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_dynamic_exit_with_real_fills() {
    TEST("Dynamic exit threshold - 실제 fill 가격 기준 계산") {
        // Entry: 10 splits, each recorded with actual fill prices
        double total_korean_cost = 0.0;  // KRW
        double total_foreign_value = 0.0; // USD
        double held_amount = 0.0;
        double usdt_rate = 1380.0;

        // Simulate 10 entry splits with real fill data
        // 역프 상태: Korean price < Foreign * USDT rate
        // Foreign ~2719 * 1380 = ~3,752,220 KRW → Korean ~3,723,000 (약 -0.78%)
        struct EntryFill {
            double korean_price;   // KRW (bithumb fill)
            double foreign_price;  // USD (bybit fill)
            double qty;
        };
        std::vector<EntryFill> entries = {
            {3722000, 2718.50, 0.00920},
            {3723500, 2719.10, 0.00919},
            {3721000, 2718.80, 0.00920},
            {3724000, 2719.50, 0.00919},
            {3725000, 2720.20, 0.00918},
            {3722500, 2718.90, 0.00920},
            {3723000, 2719.30, 0.00919},
            {3724500, 2720.00, 0.00918},
            {3723800, 2719.70, 0.00919},
            {3721500, 2718.60, 0.00920},
        };

        for (const auto& e : entries) {
            held_amount += e.qty;
            total_korean_cost += e.korean_price * e.qty;
            total_foreign_value += e.foreign_price * e.qty;
        }

        // Calculate effective entry premium from real fill data
        double avg_foreign_entry = total_foreign_value / held_amount;
        double avg_korean_entry = total_korean_cost / held_amount;
        double effective_entry_pm = ((avg_korean_entry - avg_foreign_entry * usdt_rate)
                                     / (avg_foreign_entry * usdt_rate)) * 100.0;

        // Dynamic exit threshold
        double dynamic_exit = effective_entry_pm + TradingConfig::DYNAMIC_EXIT_SPREAD;

        std::cout << "\n";
        std::cout << fmt::format("    Avg Korean entry:    {:.2f} KRW\n", avg_korean_entry);
        std::cout << fmt::format("    Avg Foreign entry:   {:.6f} USDT\n", avg_foreign_entry);
        std::cout << fmt::format("    Foreign * USDT:      {:.2f} KRW\n", avg_foreign_entry * usdt_rate);
        std::cout << fmt::format("    Effective entry PM:  {:.4f}%\n", effective_entry_pm);
        std::cout << fmt::format("    Round-trip fees:     {:.2f}%\n", TradingConfig::ROUND_TRIP_FEE_PCT);
        std::cout << fmt::format("    Min net profit:      {:.2f}%\n", TradingConfig::MIN_NET_PROFIT_PCT);
        std::cout << fmt::format("    Dynamic exit spread: {:.2f}%\n", TradingConfig::DYNAMIC_EXIT_SPREAD);
        std::cout << fmt::format("    Dynamic exit target: {:.4f}%\n", dynamic_exit);
        std::cout << "    ";

        // Verify dynamic exit is reasonable
        // Entry premium should be negative (kimchi reverse premium)
        ASSERT_TRUE(effective_entry_pm < 0.0, "entry premium should be negative (역프)");
        // Dynamic exit = entry + 0.79%, should be higher than entry
        ASSERT_TRUE(dynamic_exit > effective_entry_pm, "exit threshold > entry premium");
        // With ~-0.5% entry, exit should be around +0.29%
        ASSERT_TRUE(dynamic_exit < 1.0, "exit threshold should be reasonable (< 1%)");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bybit_cover_fill() {
    TEST("Bybit close_short fill - 커버(매수) 체결 파싱") {
        std::string json = R"({
            "retCode": 0,
            "retMsg": "OK",
            "result": {
                "list": [{
                    "orderId": "cover_order_123",
                    "symbol": "ETHUSDT",
                    "side": "Buy",
                    "orderType": "Market",
                    "qty": "0.01",
                    "avgPrice": "2725.80",
                    "cumExecQty": "0.01",
                    "orderStatus": "Filled",
                    "reduceOnly": true
                }]
            }
        })";

        auto fill = parse_bybit_fill_response(json);
        ASSERT_TRUE(fill.success, "cover fill should parse");
        ASSERT_NEAR(fill.average_price, 2725.80, 0.01, "cover avgPrice");
        ASSERT_NEAR(fill.filled_quantity, 0.01, 0.0001, "cover qty");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_bithumb_sell_detail() {
    TEST("Bithumb 매도 체결 상세 (ask 체결)") {
        // Selling 0.00920 ETH at bid price ~3,925,000 KRW
        std::string json = R"({
            "status": "0000",
            "data": {
                "order_id": "sell_order_456",
                "type": "ask",
                "order_currency": "ETH",
                "payment_currency": "KRW",
                "units": "0.00920000",
                "units_remaining": "0",
                "contract": [{
                    "transaction_date": "1706745700000",
                    "price": "3925000",
                    "units": "0.00920000",
                    "fee_currency": "KRW",
                    "fee": "14",
                    "total": "36110"
                }]
            }
        })";

        auto fill = parse_bithumb_detail_response(json);
        ASSERT_TRUE(fill.success, "sell detail should parse");
        ASSERT_NEAR(fill.average_price, 3925000.0, 1.0, "sell price KRW");
        ASSERT_NEAR(fill.filled_quantity, 0.0092, 0.00001, "sell qty");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

void test_full_round_trip_pnl() {
    TEST("Full round-trip P&L - 실제 fill 기반 손익 계산 (역프 진입 → 프리미엄 회복 후 청산)") {
        // 역프 진입: Buy ETH on Bithumb @ 3,723,000 KRW, Short on Bybit @ 2718.50 USDT
        // 프리미엄 회복 후 청산: Sell ETH @ 3,758,000 KRW, Cover @ 2720.10 USDT
        // Foreign * USDT: entry=2718.50*1380=3751530, exit=2720.10*1380=3753738
        double usdt_rate = 1380.0;
        double qty = 0.00920;

        // Entry fills (역프: korean < foreign*usdt)
        double korean_entry = 3723000.0;   // Below 3,751,530
        double foreign_entry = 2718.50;

        // Exit fills (프리미엄 회복: korean > foreign*usdt)
        double korean_exit = 3758000.0;    // Above 3,753,738
        double foreign_exit = 2720.10;

        // Korean P&L (long: sell - buy)
        double korean_pnl_krw = (korean_exit - korean_entry) * qty;
        // Foreign P&L (short: entry - exit)
        double foreign_pnl_usd = (foreign_entry - foreign_exit) * qty;
        double foreign_pnl_krw = foreign_pnl_usd * usdt_rate;
        double total_pnl = korean_pnl_krw + foreign_pnl_krw;

        // Entry premium: (korean_ask - foreign_bid * usdt) / (foreign_bid * usdt)
        double entry_pm = ((korean_entry - foreign_entry * usdt_rate) / (foreign_entry * usdt_rate)) * 100.0;
        // Exit premium: (korean_bid - foreign_ask * usdt) / (foreign_ask * usdt)
        double exit_pm = ((korean_exit - foreign_exit * usdt_rate) / (foreign_exit * usdt_rate)) * 100.0;
        double spread = exit_pm - entry_pm;

        std::cout << "\n";
        std::cout << fmt::format("    Entry: KR {:.0f} / BY {:.2f} → PM {:.4f}%\n",
                                  korean_entry, foreign_entry, entry_pm);
        std::cout << fmt::format("    Exit:  KR {:.0f} / BY {:.2f} → PM {:.4f}%\n",
                                  korean_exit, foreign_exit, exit_pm);
        std::cout << fmt::format("    Spread (exit - entry): {:.4f}%\n", spread);
        std::cout << fmt::format("    Korean P&L:  {:.0f} KRW\n", korean_pnl_krw);
        std::cout << fmt::format("    Foreign P&L: {:.4f} USD ({:.0f} KRW)\n",
                                  foreign_pnl_usd, foreign_pnl_krw);
        std::cout << fmt::format("    Total P&L:   {:.0f} KRW\n", total_pnl);
        std::cout << "    ";

        // Entry premium should be negative (역프)
        ASSERT_TRUE(entry_pm < 0, "entry PM should be negative (역프)");
        // Exit premium should be positive (프리미엄 회복)
        ASSERT_TRUE(exit_pm > 0, "exit PM should be positive");
        // Korean side should profit big (price went up from 역프 to 정프)
        ASSERT_TRUE(korean_pnl_krw > 0, "korean side profit");
        // Foreign side small loss (covered slightly higher)
        ASSERT_TRUE(foreign_pnl_usd < 0, "foreign side small loss");
        // Total should be positive — this is how the arb makes money
        ASSERT_TRUE(total_pnl > 0, "total P&L should be positive (arb profit)");
        // Spread should exceed round-trip fees
        ASSERT_TRUE(spread > TradingConfig::ROUND_TRIP_FEE_PCT, "spread > fees");
        PASS();
    } catch (...) { FAIL("unexpected exception"); }
}

// ============================================================
// Main
// ============================================================
int main() {
    Logger::init("test_fill", "warn");

    std::cout << "\n=== Fill Price Parsing 테스트 ===\n\n";

    std::cout << "[Bybit parse_order_response]\n";
    test_bybit_parse_order_success();
    test_bybit_parse_order_rejected();
    test_bybit_parse_order_invalid_json();

    std::cout << "\n[Bybit query_order_fill]\n";
    test_bybit_fill_normal();
    test_bybit_fill_btc();
    test_bybit_fill_error_response();
    test_bybit_fill_empty_list();
    test_bybit_fill_partial();
    test_bybit_cover_fill();

    std::cout << "\n[Bithumb 주문 응답 파싱]\n";
    test_bithumb_parse_order_success();
    test_bithumb_parse_order_rejected();
    test_bithumb_parse_order_no_order_id();

    std::cout << "\n[Bithumb order_detail 체결 파싱]\n";
    test_bithumb_detail_single_fill();
    test_bithumb_detail_multi_fill();
    test_bithumb_detail_three_fills();
    test_bithumb_detail_error_status();
    test_bithumb_detail_empty_contract();
    test_bithumb_sell_detail();

    std::cout << "\n[Fallback 로직]\n";
    test_fallback_with_real_fill();
    test_fallback_without_fill();

    std::cout << "\n[P&L 시뮬레이션]\n";
    test_fill_price_difference_impact();
    test_dynamic_exit_with_real_fills();
    test_full_round_trip_pnl();

    std::cout << fmt::format("\n=== 결과: {} passed, {} failed ===\n\n",
                              tests_passed, tests_failed);

    Logger::shutdown();
    return tests_failed > 0 ? 1 : 0;
}
