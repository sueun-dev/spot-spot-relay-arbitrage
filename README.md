# Kimchi Premium Arbitrage Bot

A high-frequency trading bot that exploits the price difference between Korean and foreign cryptocurrency exchanges.

## What is Kimchi Premium?

**Kimchi Premium** refers to the price gap between Korean crypto exchanges and international exchanges.

```
Premium (%) = ((Korean Price - Foreign Price) / Foreign Price) × 100
```

| Premium | Korean Name | Meaning | Action |
|---------|-------------|---------|--------|
| **Positive (+%)** | 김프 (Kimchi Premium) | Korean price > Foreign price | **EXIT** (Sell) |
| **Negative (-%)** | 역프 (Reverse Premium) | Korean price < Foreign price | **ENTRY** (Buy) |

### How the Arbitrage Works

```
┌─────────────────────────────────────────────────────────────────────┐
│                     ARBITRAGE CYCLE                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ENTRY (역프 / Negative Premium)                                    │
│   ├── Korean price is CHEAPER than foreign                          │
│   ├── Buy SPOT on Korean exchange (Upbit)                           │
│   └── Open SHORT on foreign exchange (Bybit)                        │
│                                                                      │
│                         ↓ Wait for premium reversal ↓                │
│                                                                      │
│   EXIT (김프 / Positive Premium)                                     │
│   ├── Korean price is now MORE EXPENSIVE than foreign               │
│   ├── Sell SPOT on Korean exchange                                  │
│   └── Close SHORT on foreign exchange                               │
│                                                                      │
│   PROFIT = Entry Premium Gap + Exit Premium Gap                      │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Example Trade

1. **Entry** at -2% premium (역프):
   - BTC price: Korea ₩100M, Foreign $70K (≈₩102M)
   - Buy 0.01 BTC spot on Upbit
   - Short 0.01 BTC on Bybit

2. **Exit** at +2% premium (김프):
   - BTC price: Korea ₩104M, Foreign $70K (≈₩102M)
   - Sell spot on Upbit (+4% gain)
   - Close short on Bybit (0% change)
   - **Net profit: ~4%** (minus fees)

## Features

- **Real-time monitoring** via WebSocket connections
- **Automatic entry/exit** based on premium thresholds
- **Hedged positions** - spot + futures short eliminates directional risk
- **Split orders** - minimizes market impact
- **Multi-position management** - up to 3 concurrent positions

## Configuration

| Parameter | Value | Description |
|-----------|-------|-------------|
| Entry Threshold | ≤ -1.0% | Enter when reverse premium |
| Exit Threshold | ≥ +1.0% | Exit when kimchi premium |
| Position Size | 50 USD | Per position (25 USD × 2 splits) |
| Max Positions | 3 | Concurrent positions |
| Order Interval | 1 second | Between split orders |

## Tech Stack

- **Language**: C++20 (HFT optimized)
- **JSON Parser**: simdjson (fastest)
- **WebSocket**: Beast/Asio for low-latency connections
- **Exchanges**: Bithumb (Korea), Bybit (Foreign)
- **Dashboard**: Next.js (real-time monitoring)

## Quick Start

```bash
# Build
cd kimp_arb_cpp && mkdir build && cd build
cmake .. && make -j8

# Configure
cp config/config.yaml.example config/config.yaml
# Edit config.yaml with your API keys

# Run
./kimp_bot
```

---

# 김치 프리미엄 차익거래 봇

한국 거래소와 해외 거래소 간의 가격 차이를 이용한 고빈도 차익거래 봇입니다.

## 김치 프리미엄이란?

**김치 프리미엄**은 한국 암호화폐 거래소와 해외 거래소 간의 가격 차이를 말합니다.

```
프리미엄 (%) = ((한국 가격 - 해외 가격) / 해외 가격) × 100
```

| 프리미엄 | 명칭 | 의미 | 행동 |
|---------|------|------|------|
| **양수 (+%)** | 김프 (김치 프리미엄) | 한국 가격 > 해외 가격 | **청산** (매도) |
| **음수 (-%)** | 역프 (역프리미엄) | 한국 가격 < 해외 가격 | **진입** (매수) |

### 차익거래 원리

```
┌─────────────────────────────────────────────────────────────────────┐
│                        차익거래 사이클                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   진입 (역프 / 마이너스 프리미엄)                                      │
│   ├── 한국 가격이 해외보다 저렴할 때                                   │
│   ├── 한국 거래소(Upbit)에서 현물 매수                                │
│   └── 해외 거래소(Bybit)에서 선물 숏 진입                             │
│                                                                      │
│                      ↓ 프리미엄 역전 대기 ↓                           │
│                                                                      │
│   청산 (김프 / 플러스 프리미엄)                                        │
│   ├── 한국 가격이 해외보다 비싸질 때                                   │
│   ├── 한국 거래소에서 현물 매도                                       │
│   └── 해외 거래소에서 선물 숏 청산                                    │
│                                                                      │
│   수익 = 진입 프리미엄 갭 + 청산 프리미엄 갭                           │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 거래 예시

1. **진입** -2% 역프 시점:
   - BTC 가격: 한국 1억원, 해외 $70K (≈1.02억원)
   - Upbit에서 0.01 BTC 현물 매수
   - Bybit에서 0.01 BTC 숏 진입

2. **청산** +2% 김프 시점:
   - BTC 가격: 한국 1.04억원, 해외 $70K (≈1.02억원)
   - Upbit에서 현물 매도 (+4% 수익)
   - Bybit에서 숏 청산 (0% 변동)
   - **순수익: ~4%** (수수료 제외)

## 주요 기능

- **실시간 모니터링** - WebSocket 연결
- **자동 진입/청산** - 프리미엄 임계값 기반
- **헤지 포지션** - 현물 + 선물 숏으로 방향성 리스크 제거
- **분할 주문** - 시장 충격 최소화
- **다중 포지션** - 최대 3개 동시 운영

## 설정 값

| 파라미터 | 값 | 설명 |
|---------|------|------|
| 진입 임계값 | ≤ -1.0% | 역프일 때 진입 |
| 청산 임계값 | ≥ +1.0% | 김프일 때 청산 |
| 포지션 크기 | 50 USD | 포지션당 (25 USD × 2회 분할) |
| 최대 포지션 | 3개 | 동시 운영 |
| 주문 간격 | 1초 | 분할 주문 사이 |

## 기술 스택

- **언어**: C++17 (HFT 최적화)
- **WebSocket**: Beast/Asio 저지연 연결
- **거래소**: Upbit (한국), Bybit (해외)
- **대시보드**: Next.js (실시간 모니터링)

## 빠른 시작

```bash
# 빌드
cd kimp_arb_cpp && mkdir build && cd build
cmake .. && make -j8

# 설정
cp config/config.yaml.example config/config.yaml
# config.yaml에 API 키 입력

# 실행
./kimp_bot
```

## 라이선스

MIT License
