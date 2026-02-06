# System Verification Checklist

## 1. 거래소 연결 - Bithumb/Bybit WebSocket/REST
- [x] Bithumb REST API 연결 (api.bithumb.com, AWS Seoul)
- [x] Bybit REST API 연결 (api.bybit.com, AWS Singapore via CloudFront)
- [x] Bithumb 448개 KRW 마켓 로딩
- [x] Bybit 589개 USDT 무기한 로딩
- [x] 교집합 332개 공통 심볼 매칭
- [x] USDT 환율 심볼 포함 확인
- [x] 대소문자 일관성 검증
- [x] Bybit pagination 불필요 확인 (589 < 1000)
- [x] instruments-info vs tickers 일관성 (1개 차이, 허용 범위)
- [x] 테스트: `kimp_test_symbols` (16/16 PASS)

## 2. 시세 수신 - 티커, 오더북, USDT/KRW 환율
- [ ] WebSocket 티커 스트리밍 정상 수신
- [ ] Bithumb orderbookdepth 실시간 bid/ask 업데이트
- [ ] USDT/KRW 환율 실시간 갱신
- [ ] REST fallback (60초 주기 가격 리프레시)
- [ ] WebSocket 자동 재연결 (끊김 시)
- [ ] 테스트: `kimp_test_price` (10분 bid/ask 비교)

## 3. 프리미엄 계산 - entry/exit premium 수식
- [ ] Entry premium: `(korean_ask - foreign_bid * usdt) / (foreign_bid * usdt) * 100`
- [ ] Exit premium: `(korean_bid - foreign_ask * usdt) / (foreign_ask * usdt) * 100`
- [ ] SIMD 가속 배치 계산 정확성
- [ ] 이벤트 드리븐 즉시 체크 (티커 도착 → 0ms 프리미엄 재계산)
- [ ] 100ms 백업 모니터 루프

## 4. 진입 시그널 - 조건 (≤-0.75%, 펀딩 8h, 펀딩>0)
- [ ] Entry threshold ≤ -0.75% 필터
- [ ] 펀딩 인터벌 == 8시간 필터 (4h/2h/1h 제외)
- [ ] 펀딩비 > 0% 필터 (양수만)
- [ ] MAX_POSITIONS 초과 시 진입 차단
- [ ] 동일 심볼 중복 포지션 방지
- [ ] 최적 프리미엄 코인 선별 (lowest premium)
- [ ] 테스트: `kimp_test_entry`, `kimp_test_live_entry`

## 5. 청산 시그널 - 동적 exit threshold
- [ ] Dynamic exit = entry_pm + 0.79% (fees 0.19% + profit 0.60%)
- [ ] Fallback exit threshold 0.04% (entry_premium 없을 때)
- [ ] 포지션별 개별 exit threshold 적용
- [ ] 테스트: `kimp_test_fill` 내 dynamic exit 테스트

## 6. 주문 실행 - Adaptive split, fill price, rollback
- [ ] Adaptive entry loop (while 루프, $25/split)
- [ ] Adaptive exit loop (while 루프, $25/split)
- [ ] Futures-first 실행 순서 (숏 → 매수, 커버 → 매도)
- [ ] Fill price 추출: Bybit query_order_fill()
- [ ] Fill price 추출: Bithumb query_order_detail() VWAP
- [ ] Fallback: fill 실패 시 캐시 가격 사용
- [ ] Rollback: 스팟 실패 시 선물 언와인드
- [ ] normalize_order_qty: lotSize/qtyStep 준수
- [ ] 1초 간격 split 실행
- [ ] graceful shutdown (running_ flag)
- [ ] 테스트: `kimp_test_fill` (23/23 PASS)

## 7. P&L 계산 - 한국/외국 각 사이드 손익
- [ ] Korean P&L: (매도가 - 매수가) × 수량
- [ ] Foreign P&L: (숏진입가 - 커버가) × 수량 × usdt_rate
- [ ] Total P&L = Korean + Foreign
- [ ] 수수료 포함 계산 (Bithumb 0.04%, Bybit 0.055%)
- [ ] 복리 자본 성장: add_realized_pnl()
- [ ] trade_logs/trades.csv 기록
- [ ] entry_splits.csv / exit_splits.csv 기록

## 8. 포지션 관리 - 등록/해제, crash recovery
- [ ] open_position() / close_position() 정상 동작
- [ ] active_position.json 저장 (atomic write: tmp → rename)
- [ ] crash recovery: 재시작 시 포지션 파일 로딩
- [ ] 수동 포지션 복원 (사용자 입력)
- [ ] partial exit 후 잔량 재등록

## 9. 안전장치 - 외부 포지션 감지, 레버리지, 잔고
- [ ] is_safe_to_trade(): 외부 포지션 블랙리스트
- [ ] pre_set_leverage(): 전 심볼 1x 레버리지 설정
- [ ] get_balance() 잔고 확인
- [ ] g_entry_in_flight: 동시 진입 방지 (CAS)
- [ ] SIGINT/SIGTERM graceful shutdown
- [ ] order_manager.request_shutdown() → adaptive loop 탈출
