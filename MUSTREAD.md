# MUSTREAD

`kimp_bot` 실행은 경로/환경을 정확히 맞춰야 한다. 아래 규칙을 반드시 지킨다.

## 1) 경로 규칙 (가장 중요)
- `./kimp_bot` 는 `kimp_arb_cpp/build` 폴더 안에서만 동작한다.
- 저장소 루트(`kimp_arb_bot`)에서는 `./kimp_bot`가 없다.
- 루트에서 직접 실행하려면 아래처럼 전체 경로를 써야 한다.

```bash
env -u MallocStackLogging -u MallocStackLoggingNoCompact -u MallocStackLoggingDirectory \
./kimp_arb_cpp/build/kimp_bot --config ./kimp_arb_cpp/config/config.yaml --monitor-interval-sec 1
```

## 2) 권장 실행 방식 (재발 방지)
- 앞으로는 루트에서 아래 스크립트만 사용한다.

```bash
./run_monitor.sh --monitor-interval-sec 1
```

- 순수 모니터만 돌릴 때(질문/주문 없음):

```bash
./run_monitor.sh --monitor-interval-sec 1
```

- 자동매매(실주문)까지 돌릴 때만:

```bash
./run_bot.sh --monitor-interval-sec 1
```

- 스크립트가 자동으로:
  - 올바른 빌드 경로 사용
  - `--config` 미지정 시 기본 config 자동 주입
  - 루트 `.env` 자동 로드
  - 필수 API 키(`BITHUMB_*`, `BYBIT_*`) 사전 검증
  - Bithumb/Bybit REST 사전 연결 확인(`curl`)
  - `MallocStackLogging` 환경 변수 정리
  - `kimp_bot` 빌드/실행

- `--monitor-only` 모드 동작:
  - 포지션 복구 질문 건너뜀
  - `MAX_POSITIONS` 입력 질문 건너뜀
  - 자동매매 콜백 비활성화 (주문 실행 안 함)
- `run_monitor.sh`는 내부적으로 항상 `--monitor-only`를 강제하므로, 모니터 중에 포지션 질문이 다시 뜨지 않는다.

## 3) 환경변수 확인 명령
- 오타 없이 아래 명령 사용 (`intenv`, `ntenv` 아님):

```bash
printenv | rg -i 'MallocStackLogging|DYLD'
```

## 4) 기본 점검 순서
```bash
pwd
ls -la kimp_arb_cpp/build | rg kimp_bot
./run_monitor.sh --monitor-interval-sec 1
```

## 5) 실제 실행에서 확인된 오류 대응
- 에러:
  - `FATAL: Environment variable 'BITHUMB_API_KEY' is not set`
  - `Exchange 'bithumb': api_key or secret_key is missing`
- 원인:
  - 셸에 키가 export되지 않았거나 `.env` 값이 비어 있음
- 대응:
  - `run_bot.sh`가 `.env`를 자동 로드하므로 보통 추가 조치 불필요
  - 그래도 실패하면 `.env`에서 아래 키를 확인:
    - `BITHUMB_API_KEY`
    - `BITHUMB_SECRET_KEY`
    - `BYBIT_API_KEY`
    - `BYBIT_SECRET_KEY`

- 에러:
  - `[Bithumb] Failed to initialize REST connection pool`
  - `[Bybit] Failed to initialize REST connection pool`
  - `curl: (6) Could not resolve host: api.bithumb.com`
  - `ERROR: Cannot reach Bithumb REST endpoint (https://api.bithumb.com).`
- 원인:
  - 외부망 차단, DNS/VPN 문제, 거래소 도메인 접근 불가
- 대응:
  - 네트워크 확인:
    - `curl -sS -I https://api.bithumb.com/public/ticker/USDT_KRW`
    - `curl -sS -I 'https://api.bybit.com/v5/market/tickers?category=linear'`
  - 둘 중 하나라도 실패하면 봇 실행 전에 네트워크부터 복구
  - 코드 수정 완료: 연결 실패 조기 종료 시 스레드 정리 후 종료하도록 반영 (`libc++abi terminating` 방지)

## 6) 실행 전 체크 (항상 먼저)
- 봇 실행 전 실제로 아래를 먼저 실행해서 확인:

```bash
./run_bot.sh --help
./run_monitor.sh --monitor-interval-sec 1
```

- 실행 중 오류가 새로 나오면, 그 오류와 해결 절차를 이 문서에 즉시 추가한다.

## 7) 요약
- 에러 `env: ./kimp_bot: No such file or directory` 는 봇 문제가 아니라 **현재 위치(CWD) 경로 문제**다.
- `./run_bot.sh`로 실행하면 같은 실수를 반복하지 않는다.
- `./run_monitor.sh --monitor-interval-sec 1` 을 쓰면 모니터만 동작하고 포지션 질문이 나오지 않는다.

## 8) 런타임 메모리 크래시 대응
- 에러:
  - `malloc: Incorrect checksum for freed object ... probably modified after being freed`
  - `Abort trap: 6`
- 원인(기존 버그):
  - WS/REST JSON 파서를 스레드 간 공유 사용하면서 힙 오염 발생
- 대응:
  - 최신 코드로 업데이트 후 `./run_bot.sh` 재실행 (스크립트가 자동 빌드)
  - 수정 반영: Bithumb/Bybit/Upbit 파싱 경로를 함수별 로컬 parser로 변경
  - 동일 증상 재발 시 즉시 중단하고 크래시 리포트 확인:

```bash
ls -1t ~/Library/Logs/DiagnosticReports | rg '^kimp_bot.*\\.(crash|ips)$' | head -n 1
```
