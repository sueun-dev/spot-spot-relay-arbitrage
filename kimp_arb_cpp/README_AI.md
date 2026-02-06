# KIMP Arbitrage Bot - AI Reference Document

이 문서는 AI가 코드베이스를 빠르게 이해할 수 있도록 구조화된 기술 문서입니다.

## 1. 시스템 개요

### 1.1 목적
한국 거래소(빗썸)와 해외 선물 거래소(Bybit) 간의 가격 차이(김치 프리미엄)를 이용한 무위험 차익거래 봇.

### 1.2 핵심 전략
```
진입 (역프리미엄 -0.75% 이하):
  빗썸: BTC 현물 매수 (Long)
  Bybit: BTCUSDT 선물 숏 (Short, 1x leverage)

청산 (정프리미엄 +0.34% 이상):
  빗썸: BTC 현물 매도
  Bybit: 숏 포지션 청산 (Cover)
```

### 1.3 수익 공식
```
순수익 = 프리미엄_스윙 - 수수료
       = (|entry_premium| + |exit_premium|) - (bithumb_fee + bybit_fee + slippage)
       = (0.75% + 0.34%) - (0.08% + 0.11% + 0.10%)
       = 1.09% - 0.29%
       = 0.80%
```

---

## 2. 아키텍처

### 2.1 주요 컴포넌트

```
┌─────────────────────────────────────────────────────────────┐
│                        main.cpp                              │
│  - 초기화, 콜백 설정, 메인 루프                               │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐   ┌─────────────────┐   ┌─────────────────┐
│ ArbitrageEngine│   │  OrderManager   │   │   Exchanges     │
│ (전략/시그널)   │   │  (주문 실행)     │   │ (API 통신)      │
└───────────────┘   └─────────────────┘   └─────────────────┘
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              ▼
                    ┌─────────────────┐
                    │ WebSocketClient │
                    │ (실시간 데이터)   │
                    └─────────────────┘
```

### 2.2 데이터 흐름

```
1. WebSocket → Ticker 수신
2. ArbitrageEngine::on_ticker_update() → 프리미엄 계산
3. check_entry_opportunities() 또는 check_exit_conditions()
4. Entry/Exit 콜백 → OrderManager 호출
5. OrderManager → Exchange API 호출
6. 결과 → PositionTracker, CapitalTracker 업데이트
```

---

## 3. 핵심 파일 및 역할

### 3.1 설정 (types.hpp)
```cpp
// 파일: include/kimp/core/types.hpp
struct TradingConfig {
    static constexpr int MAX_POSITIONS = 4;                    // 최대 동시 포지션
    static constexpr double POSITION_SIZE_USD = 250.0;         // 포지션당 한쪽 금액
    static constexpr double ORDER_SIZE_USD = 25.0;             // 스플릿 주문 크기
    static constexpr int SPLIT_ORDERS = 10;                    // 스플릿 횟수
    static constexpr double ENTRY_PREMIUM_THRESHOLD = -0.75;   // 진입 조건
    static constexpr double EXIT_PREMIUM_THRESHOLD = 0.34;     // 청산 조건
    static constexpr int MIN_FUNDING_INTERVAL_HOURS = 8;       // 8시간 펀딩만
    static constexpr bool REQUIRE_POSITIVE_FUNDING = true;     // 양수 펀딩만
};
```

### 3.2 프리미엄 계산 (arbitrage_engine.hpp)
```cpp
// 파일: include/kimp/strategy/arbitrage_engine.hpp
class PremiumCalculator {
    // Entry: 매수가(ASK) 기준
    static double calculate_entry_premium(korean_ask, foreign_bid, usdt_rate) {
        foreign_krw = foreign_bid * usdt_rate;
        return ((korean_ask - foreign_krw) / foreign_krw) * 100.0;
    }

    // Exit: 매도가(BID) 기준
    static double calculate_exit_premium(korean_bid, foreign_ask, usdt_rate) {
        foreign_krw = foreign_ask * usdt_rate;
        return ((korean_bid - foreign_krw) / foreign_krw) * 100.0;
    }
};
```

### 3.3 진입 조건 체크 (arbitrage_engine.cpp)
```cpp
// 파일: src/strategy/arbitrage_engine.cpp
void ArbitrageEngine::check_entry_opportunities() {
    for (symbol : monitored_symbols) {
        if (position_tracker.has_position(symbol)) continue;    // 중복 방지
        if (funding_interval < 8h) continue;                    // 8시간만
        if (funding_rate < 0) continue;                         // 양수만 (WS 캐시 + 5분 백그라운드 갱신)
        if (premium > -0.75%) continue;                         // 역프만

        // 모든 조건 충족 → Entry 시그널
        on_entry_signal_(signal);
    }
}
```

※ `MAX_POSITIONS=1`일 때는 전체 심볼을 훑어 **가장 낮은 프리미엄(최대 역프)** 1개만 선택하도록 동작합니다.

### 3.4 주문 실행 (order_manager.cpp)
```cpp
// 파일: src/execution/order_manager.cpp

// Split Entry (순차 실행 - 안전)
ExecutionResult execute_entry_futures_first(signal) {
    for (split in splits) {
        // 프리미엄 재확인
        if (premium > -0.75%) wait;

        // Step 1: Bybit SHORT 먼저 (레버리지 1x는 시작 시 일괄 설정)
        foreign_order = execute_foreign_short(symbol, quantity);
        if (failed) continue;

        // Step 2: Bithumb BUY (정확히 같은 수량)
        korean_order = execute_korean_buy(symbol, krw_amount);
        if (failed) {
            execute_foreign_cover(symbol, quantity);  // 롤백
        }
    }
}

// Split Exit (순차 실행 - 안전)
ExecutionResult execute_exit_futures_first(signal, position) {
    for (split in splits) {
        // 프리미엄 재확인
        if (premium < +0.34%) wait;

        // Step 1: Bybit COVER 먼저 (레버리지 1x는 시작 시 일괄 설정)
        foreign_order = execute_foreign_cover(symbol, quantity);
        if (failed) continue;

        // Step 2: Bithumb SELL
        korean_order = execute_korean_sell(symbol, quantity);
        if (failed) {
            LOG_CRITICAL("수동 청산 필요!");  // 언헤지 상태
        }
    }
}
```

### 3.5 Bybit API (bybit.cpp)
```cpp
// 파일: src/exchange/bybit.cpp

Order open_short(symbol, quantity) {
    body = {
        "category": "linear",
        "symbol": "BTCUSDT",
        "side": "Sell",
        "orderType": "Market",
        "qty": quantity,
        "positionIdx": 2,        // SHORT_POSITION_IDX (헤지모드)
        "reduceOnly": false      // 새 포지션 열기
    };
    POST /v5/order/create
}

Order close_short(symbol, quantity) {
    body = {
        "category": "linear",
        "symbol": "BTCUSDT",
        "side": "Buy",
        "orderType": "Market",
        "qty": quantity,
        "positionIdx": 2,
        "reduceOnly": true       // 청산만 (실수 방지)
    };
    POST /v5/order/create
}

bool set_leverage(symbol, leverage) {
    body = {
        "category": "linear",
        "symbol": "BTCUSDT",
        "buyLeverage": "1",      // 1x
        "sellLeverage": "1"      // 1x
    };
    POST /v5/position/set-leverage
}
```

### 3.6 P&L 계산 (order_manager.cpp)
```cpp
double calculate_pnl(position, exit_korean_price, exit_foreign_price, usdt_rate) {
    // 한국 현물 (Long): 팔 때 - 살 때
    korean_pnl_krw = (exit_korean_price - entry_korean_price) * korean_amount;

    // 해외 선물 (Short): 살 때 - 팔 때 (반대)
    foreign_pnl_usd = (entry_foreign_price - exit_foreign_price) * foreign_amount;
    foreign_pnl_krw = foreign_pnl_usd * usdt_rate;

    return korean_pnl_krw + foreign_pnl_krw;
}
```

---

## 4. 안전 장치

### 4.1 블랙리스트 (order_manager.cpp)
```cpp
void refresh_external_positions() {
    // Bithumb 현물 잔고 체크
    for (coin : common_coins) {
        if (bithumb.get_balance(coin) > 0.0001) {
            blacklist.insert(symbol.hash());
        }
    }

    // Bybit 선물 포지션 체크
    for (pos : bybit.get_positions()) {
        if (pos.amount > 0.0001) {
            blacklist.insert(symbol.hash());
        }
    }
}

// O(1) 체크
bool is_safe_to_trade(symbol) {
    return blacklist.find(symbol.hash()) == blacklist.end();
}
```

### 4.2 WebSocket 재연결 (websocket_client.cpp)
```cpp
// 무제한 재연결 (MAX_RECONNECT_ATTEMPTS = 0)
void schedule_reconnect() {
    delay = min(1000 * attempts, 30000);  // 최대 30초
    reconnect_timer.async_wait(on_reconnect_timer);
}

void on_ws_connected() {
    // 구독 복원
    if (!subscribed_tickers_.empty()) {
        subscribe_ticker(subscribed_tickers_);
    }
}
```

### 4.3 Exit 실패 복구 (main.cpp)
```cpp
engine.set_exit_callback([&](signal) {
    Position closed_pos;
    engine.close_position(symbol, closed_pos);  // 트래킹에서 제거

    result = order_manager.execute_exit(signal, closed_pos);

    if (result.success) {
        engine.add_realized_pnl(pnl_usd);
    } else {
        // 실패 시 포지션 다시 추가
        engine.open_position(closed_pos);
    }
});
```

---

## 5. 복리 성장 (CapitalTracker)

```cpp
// 파일: include/kimp/strategy/arbitrage_engine.hpp
class CapitalTracker {
    atomic<double> initial_capital_{2000.0};
    atomic<double> realized_pnl_usd_{0.0};

    double get_current_capital() {
        return initial_capital + realized_pnl_usd;
    }

    // 동적 포지션 사이즈
    double get_position_size_usd() {
        return get_current_capital() / MAX_POSITIONS / 2.0;
        // 예: $2000 / 4 / 2 = $250 per side
    }

    void add_realized_pnl(pnl_usd) {
        // atomic CAS로 업데이트
        realized_pnl_usd += pnl_usd;
        total_trades++;
        if (pnl_usd > 0) winning_trades++;
    }
};
```

---

## 6. 실행 모드

### 6.1 Split Orders 모드 (기본)
```
$25 × 10회 = $250/side
각 스플릿마다:
  1. 프리미엄 재확인
  2. 조건 충족 시 주문
  3. 1초 대기
  4. 다음 스플릿
```

## 7. 주요 클래스 관계

```
ArbitrageEngine
├── PriceCache          # 가격 캐시 (exchange → symbol → price)
├── PositionTracker     # 포지션 관리 (lock-free)
├── CapitalTracker      # 자본/수익 추적
├── on_entry_signal_    # Entry 콜백
└── on_exit_signal_     # Exit 콜백

OrderManager
├── exchanges_[]        # 거래소 참조
├── external_position_blacklist_  # 거래 금지 심볼
└── engine_             # ArbitrageEngine 참조

BithumbExchange : KoreanExchangeBase
├── place_market_buy_cost()  # 금액 기준 매수
├── place_market_order()     # 수량 기준 주문
└── get_balance()            # 잔고 조회

BybitExchange : ForeignFuturesExchangeBase
├── open_short()        # 숏 진입
├── close_short()       # 숏 청산 (reduceOnly=true)
├── set_leverage()      # 레버리지 설정
└── get_positions()     # 포지션 조회
```

---

## 8. 중요 상수

| 상수 | 값 | 위치 |
|------|-----|------|
| `MAX_POSITIONS` | 4 | types.hpp |
| `POSITION_SIZE_USD` | 250.0 | types.hpp |
| `ENTRY_PREMIUM_THRESHOLD` | -0.75 | types.hpp |
| `EXIT_PREMIUM_THRESHOLD` | 0.34 | types.hpp |
| `MIN_FUNDING_INTERVAL_HOURS` | 8 | types.hpp |
| `SHORT_POSITION_IDX` | 2 | bybit.hpp |
| `MAX_RECONNECT_ATTEMPTS` | 0 (무제한) | websocket_client.hpp |
| `RECONNECT_MAX_DELAY_MS` | 30000 | websocket_client.hpp |

---

## 9. 에러 처리

| 상황 | 처리 |
|------|------|
| Entry 선물 실패 | 현물 안함, 위험 없음 |
| Entry 현물 실패 | 선물 롤백 (close_short) |
| Exit 선물 실패 | 청산 중단, 헤지 유지 |
| Exit 현물 실패 | CRITICAL 로그, 수동 청산 필요 |
| WebSocket 끊김 | 자동 재연결, 구독 복원 |
| 기존 포지션 감지 | 블랙리스트, 거래 스킵 |

---

## 10. 테스트 설정

테스트용 자본 설정:
```cpp
// arbitrage_engine.hpp
CapitalTracker capital_tracker_{500.0};  // $500 테스트

// 결과: $500 / 4 / 2 = $62.50 per side
```

또는 런타임에:
```cpp
engine.set_initial_capital(500.0);
```
