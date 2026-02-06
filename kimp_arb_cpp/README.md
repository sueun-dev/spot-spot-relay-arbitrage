# KIMP Arbitrage Bot - C++ HFT Version

김치 프리미엄(Kimchi Premium) 차익거래 봇. 한국 거래소(빗썸)와 해외 선물 거래소(Bybit) 간의 가격 차이를 이용한 무위험 차익거래.

## 전략 개요

### 현선 아비트라지 (Spot-Futures Arbitrage)

```
역프리미엄 (한국 < 해외) 진입:
├── 빗썸: BTC 현물 매수 (Long)
└── Bybit: BTCUSDT 무기한 선물 숏 (Short, 1x 레버리지)

정프리미엄 (한국 > 해외) 청산:
├── 빗썸: BTC 현물 매도
└── Bybit: BTCUSDT 숏 포지션 청산
```

### 진입/청산 조건

| 조건 | 값 | 설명 |
|------|-----|------|
| Entry Premium | ≤ -0.75% | 역프리미엄 진입 |
| Exit Premium | ≥ +0.34% | 순수익 0.8% 확보 |
| Funding Interval | 8시간 | 8h 펀딩 코인만 |
| Funding Rate | > 0 | 양수 펀딩비만 |
| Max Positions | 4개 | 동시 포지션 제한 |

### 수익 구조

```
프리미엄 스윙: -0.75% → +0.34% = 1.09%

수수료:
├── Bithumb: 0.04% × 2 = 0.08%
├── Bybit:   0.055% × 2 = 0.11%
└── Slippage: ~0.10%
    Total: 0.29%

순수익: 1.09% - 0.29% = 0.80% per trade
```

## 주요 기능

### 1. 병렬 포지션 관리
- 조건 만족하는 모든 코인에 동시 진입
- 각 포지션 개별 청산 (0.8% 수익 달성 시)
- 최대 4개 동시 포지션

### 2. 복리 성장 (Compound Growth)
- 수익금 자동 재투자
- 동적 포지션 사이즈: `현재자본 / MAX_POSITIONS / 2`

### 3. 안전 장치
- **레버리지 1x**: 청산 리스크 제거
- **reduceOnly**: 포지션 청산 시 실수 방지
- **블랙리스트**: 기존 포지션 있는 코인 거래 방지
- **롤백 로직**: 한쪽 실패 시 자동 반대 거래

### 4. WebSocket 자동 재연결
- 무제한 재연결 시도
- Exponential backoff (최대 30초)
- 구독 자동 복원

## 실행 순서

### Entry (진입)
```
1. Bybit: SHORT 선물 (정확한 계약 수량 획득)
2. Bithumb: BUY 현물 (동일 수량)

※ 선물 실패 시 → 현물 안함 (위험 없음)
※ 현물 실패 시 → 선물 롤백 (위험 없음)
```

### Exit (청산)
```
1. Bybit: COVER 선물 (숏 포지션 청산)
2. Bithumb: SELL 현물 (동일 수량)

※ 선물 실패 시 → 청산 중단 (헤지 유지)
※ 현물 실패 시 → 수동 청산 필요 (경고 로그)
```

## 설치 및 빌드

### 의존성
```bash
# macOS
brew install cmake boost openssl yaml-cpp spdlog fmt

# Ubuntu
sudo apt install cmake libboost-all-dev libssl-dev libyaml-cpp-dev libspdlog-dev libfmt-dev
```

### 빌드
```bash
cd kimp_arb_cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 설정

### 환경 변수 (.env)
```bash
# Bithumb
BITHUMB_API_KEY=your_api_key
BITHUMB_SECRET_KEY=your_secret_key

# Bybit
BYBIT_API_KEY=your_api_key
BYBIT_SECRET_KEY=your_secret_key
```

### 설정 파일 (config/config.yaml)
```yaml
trading:
  max_positions: 4
  position_size_usd: 250.0
  entry_premium_threshold: -0.75
  exit_premium_threshold: 0.34

exchanges:
  bithumb:
    enabled: true
    api_key: "${BITHUMB_API_KEY}"
    secret_key: "${BITHUMB_SECRET_KEY}"
  bybit:
    enabled: true
    api_key: "${BYBIT_API_KEY}"
    secret_key: "${BYBIT_SECRET_KEY}"
```

## 실행

```bash
# 기본 실행 (Split Orders 모드)
./build/kimp_bot

# 모니터 모드 (실시간 프리미엄 테이블)
./build/kimp_bot -m
```

## 프로젝트 구조

```
kimp_arb_cpp/
├── include/kimp/
│   ├── core/
│   │   ├── types.hpp         # 핵심 타입 정의, TradingConfig
│   │   └── logger.hpp        # 비동기 로깅
│   ├── exchange/
│   │   ├── exchange_base.hpp # 거래소 인터페이스
│   │   ├── bithumb/          # 빗썸 구현
│   │   └── bybit/            # Bybit 구현
│   ├── strategy/
│   │   └── arbitrage_engine.hpp  # 전략 엔진, 프리미엄 계산
│   ├── execution/
│   │   └── order_manager.hpp     # 주문 실행, 롤백
│   └── network/
│       └── websocket_client.hpp  # WebSocket 클라이언트
├── src/
│   ├── main.cpp              # 메인 진입점
│   ├── exchange/             # 거래소 구현
│   ├── strategy/             # 전략 구현
│   └── network/              # 네트워크 구현
├── config/
│   └── config.yaml           # 설정 파일
└── build/
    └── kimp_bot              # 실행 파일
```

## 핵심 파라미터 (types.hpp)

```cpp
struct TradingConfig {
    static constexpr int MAX_POSITIONS = 4;
    static constexpr double POSITION_SIZE_USD = 250.0;      // per side
    static constexpr double ORDER_SIZE_USD = 25.0;          // per split
    static constexpr int SPLIT_ORDERS = 10;
    static constexpr double ENTRY_PREMIUM_THRESHOLD = -0.75;
    static constexpr double EXIT_PREMIUM_THRESHOLD = 0.34;
    static constexpr int MIN_FUNDING_INTERVAL_HOURS = 8;
    static constexpr bool REQUIRE_POSITIVE_FUNDING = true;
};
```

## 로그 출력 예시

```
[INFO] === KIMP Arbitrage Bot Starting ===
[INFO] Capital: $2000.00 | Position size: $250.00/side
[INFO] Entry: premium <= -0.75% (8h funding, rate > 0)
[INFO] Exit: premium >= +0.34% (순수익 0.8%)

[INFO] Entry signal: BTC/KRW premium=-0.82% funding=8h rate=0.0100%
[INFO] [ULTRAFAST] Step 1: SHORT 0.00285714 BTC on Bybit ($250.00)
[INFO] [ULTRAFAST] Futures filled in 45ms: 0.00285714 coins @ $87500.00
[INFO] [ULTRAFAST] Step 2: BUY 0.00285714 BTC on Bithumb (345000 KRW)
[INFO] [ULTRAFAST] Entry SUCCESS in 112ms

[INFO] Exit signal: BTC/KRW premium=+0.38%
[INFO] Position CLOSED: BTC/KRW | P&L: 28000 KRW ($20.29)
[INFO] Capital: $2000.00 -> $2020.29 (+1.01%)
```

## 주의사항

1. **테스트 먼저**: 소액으로 충분히 테스트 후 본격 운용
2. **API 권한**: 거래 권한 필수, 출금 권한 제거 권장
3. **자금 분리**: 거래용 자금만 거래소에 유지
4. **모니터링**: 주기적으로 포지션 상태 확인
5. **네트워크**: 안정적인 네트워크 환경 필수

## 라이선스

Private - All Rights Reserved
