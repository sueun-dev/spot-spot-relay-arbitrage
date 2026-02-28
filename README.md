# Kimchi Premium Arbitrage Bot

A production-focused arbitrage bot that monitors KRW spot markets and USDT perpetual markets, then executes hedged entry and exit based on premium spread rules.

## English

### 1. What This System Does

This project trades the spread between:
- Korean spot market: Bithumb (`BASE/KRW`)
- Foreign perpetual market: Bybit (`BASE/USDT`)

It opens a hedged position when reverse premium is deep enough, then closes when exit conditions are met.

### 2. Core Formula

Entry premium (for opening):

```text
entry_pm(%) = ((korean_ask - foreign_bid * usdt_krw) / (foreign_bid * usdt_krw)) * 100
```

Exit premium (for closing):

```text
exit_pm(%) = ((korean_bid - foreign_ask * usdt_krw) / (foreign_ask * usdt_krw)) * 100
```

### 3. End-to-End Trading Logic

1. Symbol universe build
- Load symbols from Bithumb and Bybit
- Intersect by base symbol
- Register common symbols as `BASE/KRW` (and map to `BASE/USDT` internally)

2. Market data ingest
- Initial REST snapshot load from both exchanges in parallel
- Real-time WebSocket ticker updates
- Bithumb orderbook overlay for executable bid/ask quality
- Periodic fallback refresh threads (price and funding)

3. Premium compute
- `ArbitrageEngine` stores quotes in a sharded `PriceCache`
- Computes entry/exit premium per symbol
- Applies quote freshness, desync, and spread quality filters

4. Entry gate
- Entry threshold: `entry_pm <= -0.99`
- Funding constraints:
  - `MIN_FUNDING_INTERVAL_HOURS = 4`
  - `REQUIRE_POSITIVE_FUNDING = true`
- Safety gate checks external/manual positions before trading

5. Split entry execution
- Position size per side: `250 USD`
- Split count: `10`
- Split size: `25 USD`
- For each split:
  - Buy spot on Bithumb
  - Open short on Bybit
- If one leg fails, rollback is attempted immediately

6. Dynamic exit logic
- Fee model:
  - Bithumb fee: `0.04%`
  - Bybit fee: `0.055%`
  - Round trip fee: `0.19%`
- Dynamic spread:
  - `DYNAMIC_EXIT_SPREAD = 0.19%`
- Required exit premium:

```text
required_exit_pm = max(entry_pm + DYNAMIC_EXIT_SPREAD, EXIT_PREMIUM_THRESHOLD)
```

- Exit floor:
  - `EXIT_PREMIUM_THRESHOLD = 0.25`

7. Failure handling and critical stop
- Rollback failures are treated as critical
- On critical mismatch:
  - trading loop is stopped
  - new entries are suppressed
  - mismatch snapshot is persisted for manual intervention

8. Recovery and persistence
- Startup recovery can detect and resume existing live positions
- Persisted outputs:
  - `trade_logs/trades.csv`
  - `trade_logs/entry_splits.csv`
  - `trade_logs/exit_splits.csv`
  - `trade_logs/active_position.json`
  - `kimp_arb_cpp/data/premiums.json`

### 4. Monitoring and Dashboard

- `ArbitrageEngine` exports premium JSON every `200ms`
- Dedicated WebSocket broadcast server pushes updates every `50ms` on port `8765`
- Next.js dashboard reads premium data from:
  1. `KIMP_PREMIUMS_PATH` (env override)
  2. `../kimp_arb_cpp/build/data/premiums.json`
  3. `../kimp_arb_cpp/data/premiums.json`

### 5. Concurrency Model

- Sharded `PriceCache` for reduced lock contention
- Separate I/O, strategy, execution, refresh, and broadcast paths
- CSV writes are protected with synchronization to prevent row corruption
- Position and signal structures are designed for thread-safe access

### 6. Build and Run

```bash
# C++ build
cd kimp_arb_cpp
cmake -S . -B build
cmake --build build -j8

# Run bot
./build/kimp_bot

# Or helper script from repository root
cd ..
./run_bot.sh
```

### 7. Test Commands

```bash
cd kimp_arb_cpp
cmake --build build -j8

# Run full C++ suite
for t in build/kimp_test_*; do "$t"; done

# Dashboard production build
cd ../dashboard
npm run build
```

Notes:
- Live trade tests require valid exchange API keys.
- If keys are missing, live execution tests now report `SKIP` and exit successfully.

---

## 한국어

### 1. 이 시스템이 하는 일

이 프로젝트는 아래 두 시장의 가격 괴리를 거래합니다.
- 한국 현물: Bithumb (`BASE/KRW`)
- 해외 무기한 선물: Bybit (`BASE/USDT`)

역프가 충분히 깊어지면 헤지 진입하고, 청산 조건이 충족되면 양쪽 포지션을 함께 종료합니다.

### 2. 핵심 수식

진입 프리미엄:

```text
entry_pm(%) = ((korean_ask - foreign_bid * usdt_krw) / (foreign_bid * usdt_krw)) * 100
```

청산 프리미엄:

```text
exit_pm(%) = ((korean_bid - foreign_ask * usdt_krw) / (foreign_ask * usdt_krw)) * 100
```

### 3. 실제 동작 로직

1. 심볼 유니버스 구성
- Bithumb, Bybit 심볼 목록 로드
- 코인 베이스 기준 교집합 계산
- 공통 심볼을 `BASE/KRW`로 등록하고 내부에서 `BASE/USDT`로 매핑

2. 시세 수집
- 시작 시 양 거래소 REST 스냅샷 병렬 로드
- 이후 WebSocket 실시간 티커 반영
- Bithumb 오더북 오버레이로 실제 체결 가능한 bid/ask 품질 강화
- 백업용 주기적 refresh 스레드 운영 (가격, 펀딩)

3. 프리미엄 계산
- `ArbitrageEngine`가 sharded `PriceCache`에 시세 저장
- 심볼별 entry/exit 프리미엄 계산
- 시세 신선도, 거래소 간 시점 차이, 스프레드 품질 필터 적용

4. 진입 조건
- 진입 임계값: `entry_pm <= -0.99`
- 펀딩 필터:
  - `MIN_FUNDING_INTERVAL_HOURS = 4`
  - `REQUIRE_POSITIVE_FUNDING = true`
- 외부 수동 포지션과 충돌하지 않도록 안전 체크 후 진입

5. 분할 진입 실행
- 한쪽 기준 포지션 크기: `250 USD`
- 분할 횟수: `10`
- 분할당 주문 크기: `25 USD`
- 각 분할마다:
  - Bithumb 현물 매수
  - Bybit 선물 숏 진입
- 한쪽 체결 실패 시 즉시 롤백 시도

6. 동적 청산 로직
- 수수료 모델:
  - Bithumb: `0.04%`
  - Bybit: `0.055%`
  - 왕복 합계: `0.19%`
- 동적 스프레드:
  - `DYNAMIC_EXIT_SPREAD = 0.19%`
- 요구 청산 프리미엄:

```text
required_exit_pm = max(entry_pm + DYNAMIC_EXIT_SPREAD, EXIT_PREMIUM_THRESHOLD)
```

- 청산 하한:
  - `EXIT_PREMIUM_THRESHOLD = 0.25`

7. 장애 대응과 강제 정지
- 롤백 실패는 치명 장애로 처리
- 치명 불일치 발생 시:
  - 거래 루프 정지
  - 신규 진입 억제
  - 불일치 스냅샷 저장 후 수동 개입 유도

8. 복구와 저장
- 시작 시 기존 실포지션을 스캔하고 복구 재개 가능
- 주요 저장 파일:
  - `trade_logs/trades.csv`
  - `trade_logs/entry_splits.csv`
  - `trade_logs/exit_splits.csv`
  - `trade_logs/active_position.json`
  - `kimp_arb_cpp/data/premiums.json`

### 4. 모니터링과 대시보드

- `ArbitrageEngine`는 `200ms` 주기로 프리미엄 JSON을 갱신
- 전용 WebSocket 브로드캐스트 서버가 `50ms` 주기로 `8765` 포트에서 전송
- Next.js 대시보드는 아래 순서로 프리미엄 파일을 탐색
  1. `KIMP_PREMIUMS_PATH` 환경변수
  2. `../kimp_arb_cpp/build/data/premiums.json`
  3. `../kimp_arb_cpp/data/premiums.json`

### 5. 동시성 모델

- `PriceCache`를 shard로 분할해 락 경합 완화
- I/O, 전략, 실행, refresh, broadcast를 분리 운영
- CSV 기록은 동기화로 행 깨짐을 방지
- 포지션/시그널 구조는 스레드 안전 접근 기준으로 구성

### 6. 빌드와 실행

```bash
# C++ 빌드
cd kimp_arb_cpp
cmake -S . -B build
cmake --build build -j8

# 봇 실행
./build/kimp_bot

# 저장소 루트 헬퍼 스크립트
cd ..
./run_bot.sh
```

### 7. 테스트 실행

```bash
cd kimp_arb_cpp
cmake --build build -j8

# C++ 전체 테스트
for t in build/kimp_test_*; do "$t"; done

# 대시보드 프로덕션 빌드
cd ../dashboard
npm run build
```

참고:
- 실거래 테스트는 유효한 API 키가 필요합니다.
- 키가 없으면 라이브 실행 테스트는 `SKIP`으로 처리되고 성공 종료됩니다.

## License

MIT License
