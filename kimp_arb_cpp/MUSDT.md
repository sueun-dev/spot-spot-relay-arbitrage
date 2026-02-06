# C++ HFT Kimchi Premium Arbitrage Bot - Code Reading Guide

## 추천 읽기 순서

### 1. 진입점 - main.cpp
```
src/main.cpp
```
프로그램 시작점. 전체 흐름을 파악할 수 있음:
- Bithumb/Bybit Exchange 객체 생성
- WebSocket 연결 + 브로드캐스트 서버 (포트 8765)
- ArbitrageEngine + OrderManager 실행
- Crash recovery (포지션 영속성)
- 콘솔 모니터 (10초마다 프리미엄 테이블)

### 2. 핵심 타입 정의
```
include/kimp/core/types.hpp
```
- `Exchange` enum (Bithumb, Bybit, Upbit, GateIO)
- `SymbolId`, `Position`, `Ticker` 구조체
- `TradingConfig` 상수 (ENTRY=-0.75%, DYNAMIC_EXIT_SPREAD=0.79%)

### 3. 전략 엔진 (가장 중요)
```
include/kimp/strategy/arbitrage_engine.hpp  ← 헤더 먼저
src/strategy/arbitrage_engine.cpp           ← 구현체
```
- `PriceCache` - 거래소별 가격 캐시 (shared_mutex + atomic)
- `PremiumCalculator` - 김프 계산 (entry: ask/bid, exit: bid/ask)
- `PositionTracker` - 16-slot 배열, atomic CAS
- `CapitalTracker` - 복리 성장, 동적 포지션 사이징
- `ArbitrageEngine` - event-driven 시그널 생성

### 4. 주문 실행
```
include/kimp/execution/order_manager.hpp
src/execution/order_manager.cpp
```
- Adaptive split execution ($25 단위)
- Futures-first 주문 (Bybit SHORT → Bithumb BUY)
- Hedge accuracy: normalize_order_qty → actual_filled → 동일 수량
- 청산 중 재진입 (adaptive exit/re-entry)

### 5. 거래소 구현체
```
include/kimp/exchange/exchange_base.hpp     ← 인터페이스
include/kimp/exchange/bithumb/bithumb.hpp   ← 빗썸 (주력)
include/kimp/exchange/bybit/bybit.hpp       ← 바이비트 (주력)
src/exchange/bithumb.cpp
src/exchange/bybit.cpp
```

### 6. 인프라
```
include/kimp/memory/ring_buffer.hpp         ← Lock-free 큐
include/kimp/network/websocket_client.hpp   ← Beast/Asio WebSocket
include/kimp/network/ws_broadcast_server.hpp ← 대시보드 브로드캐스트
include/kimp/core/simd_premium.hpp          ← AVX2/NEON SIMD 배치 계산
```

---

## 아키텍처 다이어그램

```
┌─────────────────────────────────────────────────────────────┐
│                    Main Strategy Thread                      │
│   (Event-driven: on_ticker_update → premium → signal)       │
└─────────────────────────────────────────────────────────────┘
                              ▲
                              │ Lock-free queues
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
┌───────┴───────┐    ┌───────┴───────┐    ┌───────┴───────┐
│  I/O Threads  │    │   Background  │    │  Order Exec   │
│  (WebSocket)  │    │   Refresh     │    │    Thread     │
│  Bithumb+Bybit│    │  5min/60sec   │    │  Adaptive $25 │
└───────────────┘    └───────────────┘    └───────────────┘
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
- **Entry**: Premium ≤ -0.75% AND 8h Funding AND Funding Rate > 0
- **Exit**: Dynamic (entry_premium + 0.79% = 0.19% fees + 0.60% profit)

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

### PositionTracker
- 최대 1~4개 동시 포지션 (런타임 설정)
- Atomic CAS 기반 포지션 열기/닫기
- 16-slot 배열, symbol hash collision safety

### CapitalTracker
- 복리 성장 (초기 $2000)
- 동적 포지션 사이징 (최대 $250/side)
- Win rate, total trades 추적

---

## 빠르게 파악하려면

1. **types.hpp** - 5분 훑어보기
2. **arbitrage_engine.hpp** - `PriceCache`, `PremiumCalculator` 집중
3. **main.cpp** - 실행 흐름 따라가기

이 세 파일만 읽으면 전체 구조가 잡힘.

---

## 디렉토리 구조

```
kimp_arb_cpp/
├── CMakeLists.txt              # 빌드 시스템 (C++20, 14 executables)
├── include/kimp/
│   ├── core/                   # types, config, logger, optimization, simd_premium
│   ├── memory/                 # ring_buffer
│   ├── network/                # websocket_client, ws_broadcast_server, connection_pool
│   ├── exchange/               # exchange_base, bithumb/, bybit/, upbit/, gateio/
│   ├── strategy/               # arbitrage_engine
│   ├── execution/              # order_manager
│   └── utils/                  # crypto (HMAC/JWT)
├── src/                        # 구현체 (.cpp)
├── tests/                      # 14개 테스트 (unit + integration + live)
│   └── integration/            # diag_trade
├── config/                     # config.yaml
├── docs/                       # OPTIMIZATION.md
└── third_party/                # jwt-cpp (header-only)
```

---

## 빌드 & 실행

```bash
# 의존성 설치 (macOS)
brew install boost openssl@3 simdjson spdlog fmt yaml-cpp

# 빌드
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 실행
./build/kimp_bot --config config/config.yaml
./build/kimp_bot -m   # 모니터 모드
```

---

## Dashboard 연동

C++ 봇이 `data/premiums.json`에 200ms마다 데이터 export.
WebSocket 브로드캐스트 서버 (포트 8765)로 50ms 간격 실시간 전송.

Next.js 대시보드 (`dashboard/`)가 이 데이터를 읽어서 화면에 표시.
