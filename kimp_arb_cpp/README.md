# kimp_arb_cpp

현재 C++ 엔진은 `spot relay` 전용입니다.

동작:
- 빗썸 `KRW` 현물 호가 수신
- 바이비트 `USDT spot` 호가 수신
- `Bybit spot margin short` 가능 코인만 사용
- `70 USDT` 기준으로 양쪽 1틱 수량이 동시에 충분한지 검사
- `빗썸 1회 + 바이비트 3회` 수수료를 반영한 `net edge` 로 진입 판단

실행 예시:

```bash
cd /Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/kimp_arb_cpp
cmake --build build/build/Release -j 10
./build/build/Release/kimp_bot --monitor-only
```

핵심 테스트:

```bash
./build/build/Release/kimp_test_entry
./build/build/Release/kimp_test_entry_capacity
./build/build/Release/kimp_test_atomic_bitset
./build/build/Release/kimp_test_entry_bitmap
./build/build/Release/kimp_test_lifecycle_executor
./build/build/Release/kimp_test_s1_to_s4
./build/build/Release/kimp_test_s6_to_s8
```
