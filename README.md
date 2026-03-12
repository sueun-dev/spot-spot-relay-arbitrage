# Spot Relay Engine

이 저장소는 `빗썸 현물 매수호가`와 `바이비트 현물 마진 숏 매도호가`를 실시간으로 비교하는
`spot relay` 엔진과 모니터를 포함합니다.

핵심 기준:
- 선물 미사용
- GateIO 미사용
- 대상 코인: `빗썸 KRW` 와 `바이비트 USDT spot margin` 공통 코인
- 진입 게이트: `70 USDT`, `양쪽 1틱 즉시 체결 가능`, `수수료 반영 후 net edge > 0`
- 수수료 모델: `빗썸 1회 + 바이비트 3회`

주요 경로:
- C++ 엔진: `/Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/kimp_arb_cpp`
- 실시간 모니터: `/Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/scripts/spot-relay-live.mjs`
- 웹 대시보드: `/Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/dashboard/src/app/relay/page.tsx`

실행:

```bash
cd /Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/kimp_arb_cpp
cmake --build build/build/Release -j 10
./build/build/Release/kimp_bot --monitor-only
```

웹:

```bash
cd /Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine
npm run relay:web
```
