# C++ HFT Kimchi Premium Arbitrage Bot - Code Reading Guide

## 추천 읽기 순서

### 1. 진입점 - main.cpp
```
src/main.cpp
```
프로그램 시작점. 전체 흐름을 파악할 수 있음:
- Exchange 객체 생성
- WebSocket 연결
- ArbitrageEngine 실행
- JSON export 루프

### 2. 핵심 타입 정의
```
include/kimp/core/types.hpp
```
- `Exchange` enum (Upbit, Bithumb, Bybit, GateIO)
- `SymbolId`, `Position`, `Ticker` 구조체
- `TradingConfig` 상수 (진입/청산 threshold 등)

### 3. 전략 엔진 (가장 중요)
```
include/kimp/strategy/arbitrage_engine.hpp  ← 헤더 먼저
src/strategy/arbitrage_engine.cpp           ← 구현체
```
- `PriceCache` - 거래소별 가격 캐시
- `PremiumCalculator` - 김프 계산 로직
- `ArbitrageEngine` - 시그널 생성, 포지션 관리

### 4. 거래소 구현체
```
include/kimp/exchange/exchange_base.hpp  ← 인터페이스
include/kimp/exchange/upbit/            ← 업비트
include/kimp/exchange/bybit/            ← 바이비트
```
- WebSocket 연결
- Ticker 파싱
- 주문 실행 (API 키 필요)

### 5. 메모리/네트워크 인프라
```
include/kimp/memory/ring_buffer.hpp     ← Lock-free 큐
include/kimp/network/websocket_client.hpp
```

---

## 아키텍처 다이어그램

```
┌─────────────────────────────────────────────────────────────┐
│                    Main Strategy Thread                      │
│         (Premium Calculation + Signal Generation)            │
└─────────────────────────────────────────────────────────────┘
                              ▲
                              │ Lock-free queues
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
┌───────┴───────┐    ┌───────┴───────┐    ┌───────┴───────┐
│  I/O Threads  │    │  Market Data  │    │  Order Exec   │
│  (WebSocket)  │    │   Aggregator  │    │    Thread     │
│  4 exchanges  │    │               │    │               │
└───────────────┘    └───────────────┘    └───────────────┘
```

---

## 핵심 로직 흐름

```
┌─────────────────────────────────────────────────────────┐
│  main.cpp                                               │
│  ├── UpbitExchange, BybitExchange 생성                  │
│  ├── ArbitrageEngine에 등록                             │
│  ├── WebSocket 연결 (ticker 구독)                       │
│  └── 매초 export_to_json() → dashboard가 읽음           │
└─────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────┐
│  ArbitrageEngine                                        │
│  ├── on_ticker_update() ← 거래소에서 가격 수신          │
│  ├── PriceCache에 저장                                  │
│  ├── Premium 계산: (한국가 - 해외가*환율) / 해외가*환율 │
│  └── Entry ≤ -1%, Exit ≥ +1% 시그널 생성               │
└─────────────────────────────────────────────────────────┘
```

---

## 김프(Kimchi Premium) 계산 공식

### Entry Premium (진입용)
```cpp
// 한국 ASK (매수가) vs 해외 BID (숏 진입가)
double premium = ((korean_ask - foreign_bid * usdt_krw) / (foreign_bid * usdt_krw)) * 100.0;
```

### Exit Premium (청산용)
```cpp
// 한국 BID (매도가) vs 해외 ASK (숏 청산가)
double premium = ((korean_bid - foreign_ask * usdt_krw) / (foreign_ask * usdt_krw)) * 100.0;
```

### 트레이딩 조건
- **Entry**: Premium ≤ -1.0% AND Funding Rate ≥ 0
- **Exit**: Premium ≥ +1.0%

---

## 주요 클래스 설명

### PriceCache
거래소별/심볼별 가격 정보를 atomic하게 저장
```cpp
struct CachedPrice {
    std::atomic<double> bid;
    std::atomic<double> ask;
    std::atomic<double> last;
    std::atomic<uint64_t> timestamp;
};
```

### PremiumCalculator
정적 메서드로 김프 계산
```cpp
static double calculate_entry_premium(double korean_ask, double foreign_bid, double usdt_krw_rate);
static double calculate_exit_premium(double korean_bid, double foreign_ask, double usdt_krw_rate);
static bool should_enter(double premium, double funding_rate);
static bool should_exit(double premium);
```

### ArbitrageEngine
- 가격 캐시 관리
- 포지션 추적
- 시그널 생성
- JSON export (대시보드용)

### PositionTracker
- 최대 3개 동시 포지션
- 포지션 열기/닫기
- 활성 포지션 조회

---

## 빠르게 파악하려면

1. **types.hpp** - 5분 훑어보기
2. **arbitrage_engine.hpp** - `PremiumCalculator` 클래스 집중
3. **main.cpp** - 실행 흐름 따라가기

이 세 파일만 읽으면 전체 구조가 잡힘.

---

## 디렉토리 구조

```
kimp_arb_cpp/
├── CMakeLists.txt
├── conanfile.txt
├── include/kimp/
│   ├── core/          # types, config, logger
│   ├── memory/        # ring_buffer, object_pool
│   ├── network/       # websocket_client, ssl
│   ├── exchange/      # upbit, bithumb, bybit, gateio
│   ├── data/          # market_data, orderbook, ticker
│   ├── strategy/      # arbitrage_engine, premium_calc
│   └── execution/     # order_manager
├── src/
├── config/
├── data/              # JSON export (dashboard용)
└── tests/
```

---

## 빌드 & 실행

```bash
# 의존성 설치
conan install . --output-folder=build --build=missing

# 빌드
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 실행
./build/kimp_bot --config config/config.yaml
```

---

## Dashboard 연동

C++ 봇이 `data/premiums.json`에 매초 데이터 export:
```json
{
  "status": {
    "connected": true,
    "upbitConnected": true,
    "bybitConnected": true,
    "lastUpdate": 1706842800000
  },
  "premiums": [
    {
      "symbol": "BTC",
      "koreanPrice": 95000000,
      "foreignPrice": 64500.5,
      "usdtRate": 1450.0,
      "premium": 1.52,
      "signal": "EXIT"
    }
  ]
}
```

Next.js 대시보드가 이 파일을 읽어서 화면에 표시.
