# Kimchi Premium Arbitrage Bot

김치 프리미엄을 활용한 암호화폐 자동 차익거래 봇

## 📋 목차

- [개요](#개요)
- [작동 원리](#작동-원리)
- [시스템 아키텍처](#시스템-아키텍처)
- [설치 방법](#설치-방법)
- [설정](#설정)
- [실행 방법](#실행-방법)
- [거래 전략 상세](#거래-전략-상세)
- [API 요구사항](#api-요구사항)
- [위험 관리](#위험-관리)
- [문제 해결](#문제-해결)

## 개요

이 봇은 한국 거래소(업비트, 빗썸)와 글로벌 선물 거래소(OKX, Gate.io) 간의 가격 차이(김치 프리미엄)를 이용해 자동으로 차익거래를 실행합니다.

### 주요 특징

- ✅ **완전 자동화**: 24/7 시장 모니터링 및 자동 거래 실행
- ✅ **분할 진입/청산**: 리스크 분산을 위한 단계적 포지션 관리
- ✅ **완벽한 헤지**: 동일 수량으로 현물-선물 헤지
- ✅ **실시간 환율 적용**: USDT/KRW 실시간 환율 사용
- ✅ **다중 거래소 지원**: 4개 거래소 동시 모니터링
- ✅ **안전장치**: 동시성 제어, 에러 처리, Graceful Shutdown

## 작동 원리

### 1. 김치 프리미엄이란?

김치 프리미엄은 한국 거래소의 암호화폐 가격이 해외 거래소보다 높은 현상을 말합니다.

```
프리미엄(%) = ((한국 가격 / USDT 환율) - 해외 가격) / 해외 가격 × 100
```

### 2. 차익거래 프로세스

```
역프리미엄 발생 (-0.5% 이하) → 진입
1. 한국 거래소: 현물 매수 (싸게 구매)
2. 해외 거래소: 선물 숏 (비싸게 판매)

정프리미엄 전환 (+0.5% 이상) → 청산
1. 한국 거래소: 현물 매도 (비싸게 판매)
2. 해외 거래소: 선물 롱 청산 (싸게 구매)

수익 = 프리미엄 변화폭 - 수수료
```

## 시스템 아키텍처

### 전체 구조

```
main.py
├── config.py                    # 전략 설정
├── src/
│   ├── exchanges/              # 거래소 연동
│   │   ├── base.py            # 추상 클래스
│   │   ├── connector.py       # 거래소 관리자
│   │   ├── upbit.py          # 업비트 구현
│   │   ├── bithumb.py        # 빗썸 구현
│   │   ├── okx.py            # OKX 구현
│   │   └── gate.py           # Gate.io 구현
│   ├── strategies/            # 거래 전략
│   │   └── split_entry_strategy.py
│   ├── utils/                 # 유틸리티
│   │   └── premium_calculator.py
│   └── models/                # 데이터 모델
│       └── models.py
└── tests/                     # 테스트 코드
```

### 실행 흐름 (Step by Step)

#### 1단계: 초기화
```python
# main.py 시작
1. 환경 변수 로드 (.env)
2. 로깅 설정
3. ExchangeConnector 생성
   - 각 거래소 API 인증
   - HTTP 세션 초기화
4. SplitEntryStrategy 생성
   - 포지션 관리자 초기화
   - 전략 파라미터 설정
```

#### 2단계: 모니터링 루프
```python
# _monitor_loop() 실행 (5초 간격)
1. 공통 심볼 조회
   - 한국 거래소 심볼 ∩ 해외 거래소 심볼
   
2. 각 심볼별 순차 처리:
   a. 프리미엄 계산 (_get_best_premium)
      - 한국: 가장 싼 거래소 선택
      - 해외: 가장 비싼 거래소 선택
      - USDT 환율 실시간 업데이트
      
   b. 진입 조건 확인
      - 프리미엄 ≤ -0.5%
      - -10% ≤ 프리미엄 (너무 큰 차이 제외)
      - 펀딩비 ≥ 0 (양수만)
      - 최대 5개 코인 제한
      
   c. 청산 조건 확인
      - 프리미엄 ≥ +0.5%
      - 프리미엄 ≤ +10% (너무 큰 차이 제외)
      - 포지션 보유 중
```

#### 3단계: 진입 실행 (_handle_entry)
```python
1. 포지션 상태 변경 (Lock 사용)
   - active_positions 체크
   - status = 'entering'

2. 분할 진입 (10,000원씩, 최대 10만원)
   while 총액 < 10만원:
      a. 프리미엄 재확인
      b. 주문 실행 (_execute_entry)
         - 업비트: 10,000원 시장가 매수
         - 빗썸: 코인 수량 계산 후 매수
         - 해외: 동일 수량 선물 숏
      c. 2분 대기 (마지막 제외)

3. 상태 변경: 'holding'
```

#### 4단계: 주문 실행 상세 (_execute_entry)
```python
1. USDT 환율 업데이트
2. 최신 호가창 조회
3. 거래소별 처리:
   
   # 업비트
   - 10,000원 시장가 매수
   - 수수료(0.05%) 차감 후 실제 코인 수량 계산
   
   # 빗썸  
   - 10,000원 어치 코인 수량 계산
   - 수수료(0.04%) 고려한 수량 조정
   
4. 유동성 체크 및 조정
5. 병렬 주문 실행 (asyncio.gather)
6. 실패 시 롤백 처리
```

#### 5단계: 청산 실행 (_handle_exit)
```python
1. FIFO 방식으로 진입 기록 추출
2. 분할 청산 (진입의 역순)
   while 포지션 > 0:
      a. 프리미엄 재확인
      b. 주문 실행 (_execute_exit)
         - 한국: 현물 시장가 매도
         - 해외: 선물 롱 청산
      c. 수익 계산 및 로깅
      d. 2분 대기 (마지막 제외)
      
3. 포지션 초기화
```

## 설치 방법

### 1. 저장소 클론
```bash
git clone https://github.com/yourusername/kimp_arb_bot.git
cd kimp_arb_bot
```

### 2. 가상환경 생성 (권장)
```bash
python -m venv venv
source venv/bin/activate  # Windows: venv\Scripts\activate
```

### 3. 패키지 설치
```bash
pip install -r requirements.txt
```

### 4. 환경 변수 설정
```bash
cp .env.example .env
# .env 파일을 열어 API 키 입력
```

## 설정

### 1. API 키 설정 (.env)
```env
# 한국 거래소
UPBIT_ACCESS_KEY=your_access_key
UPBIT_SECRET_KEY=your_secret_key

BITHUMB_API_KEY=your_api_key
BITHUMB_SECRET_KEY=your_secret_key

# 해외 거래소
OKX_API_KEY=your_api_key
OKX_SECRET_KEY=your_secret_key
OKX_PASSPHRASE=your_passphrase

GATE_API_KEY=your_api_key
GATE_SECRET_KEY=your_secret_key
```

### 2. 전략 파라미터 (config.py)
```python
# 진입/청산 설정
entry_threshold = -0.5    # 진입 프리미엄 (%)
exit_threshold = 0.5      # 청산 프리미엄 (%)
entry_amount_krw = 10000  # 회당 진입 금액
max_amount_per_coin = 100000  # 코인당 최대 금액
max_coins = 5            # 최대 보유 코인 수
entry_interval = 120     # 진입 간격 (초)
```

## 실행 방법

### 1. 기본 실행
```bash
python main.py
```

### 2. 백그라운드 실행 (Linux/Mac)
```bash
nohup python main.py > bot.log 2>&1 &
```

### 3. 중지 방법
```bash
# Ctrl+C 또는
kill -SIGINT <process_id>
```

## 거래 전략 상세

### 진입 조건
1. **프리미엄**: -0.5% 이하 (역프리미엄)
2. **프리미엄 범위**: -10% ~ -0.5% (극단값 제외)
3. **펀딩비**: 0 이상 (양수)
4. **포지션 제한**: 최대 5개 코인
5. **금액 제한**: 코인당 최대 10만원

### 청산 조건
1. **프리미엄**: +0.5% 이상 (정프리미엄)
2. **프리미엄 범위**: +0.5% ~ +10% (극단값 제외)
3. **포지션 상태**: 보유 중

### 리스크 관리
- **분할 진입**: 10,000원씩 2분 간격
- **포지션 제한**: 최대 5개 코인, 총 50만원
- **헤지 비율**: 1:1 완벽 헤지
- **유동성 체크**: 호가 물량 확인 및 조정
- **동시성 제어**: Lock을 통한 레이스 컨디션 방지

## API 요구사항

### 한국 거래소
- **업비트**: 주문 권한 필요
- **빗썸**: 거래 권한 필요

### 해외 거래소
- **OKX**: 선물 거래 권한 필요
- **Gate.io**: USDT 선물 거래 권한 필요

### API 제한
- 모든 거래소: 8 requests/second 제한
- 자동 rate limiting 적용

## 위험 관리

### 주의사항
1. **환율 변동**: USDT/KRW 환율 급변동 시 손실 가능
2. **거래 수수료**: 한국 0.04-0.05%, 해외 0.05%
3. **펀딩비**: 선물 포지션 유지 비용
4. **슬리피지**: 시장가 주문으로 인한 체결 가격 차이
5. **API 장애**: 네트워크 문제로 인한 헤지 실패 위험

### 안전장치
- 극단적 프리미엄(±10% 초과) 제외
- 음수 펀딩비 회피
- 실패 시 자동 롤백
- Graceful Shutdown 지원

## 문제 해결

### 자주 발생하는 문제

1. **"No USDT rate" 에러**
   - 원인: USDT/KRW 환율 조회 실패
   - 해결: 거래소 API 상태 확인

2. **"Order execution failed" 에러**
   - 원인: 잔고 부족 또는 API 권한 문제
   - 해결: 계정 잔고 및 API 권한 확인

3. **"Insufficient liquidity" 경고**
   - 원인: 호가창 물량 부족
   - 해결: 자동으로 수량 조정됨

4. **포지션이 청산되지 않음**
   - 원인: 프리미엄이 +0.5% 미만
   - 해결: 프리미엄 회복 대기

### 로그 확인
```bash
# 실시간 로그 확인
tail -f bot.log

# 에러만 확인
grep ERROR bot.log
```

## 라이선스

MIT License

## 기여하기

Pull Request는 언제나 환영입니다!

## 면책조항

이 봇은 교육 목적으로 제작되었습니다. 실제 거래에 사용 시 발생하는 손실에 대해 개발자는 책임지지 않습니다.