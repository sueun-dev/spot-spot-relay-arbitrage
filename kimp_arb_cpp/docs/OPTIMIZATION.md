# 추가 최적화 방안

## 1. Kernel Bypass I/O

### 현재 (일반 소켓)
```
Exchange → NIC → Kernel → TCP Stack → Userspace → App
           ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
                   ~5-10 μs 오버헤드
```

### 최적화 (io_uring / DPDK)
```
Exchange → NIC → Userspace (직접)
                  ~0.5-1 μs
```

**구현 방법:**
```cpp
// io_uring 예시 (Linux 5.1+)
#include <liburing.h>

struct io_uring ring;
io_uring_queue_init(256, &ring, 0);

// Zero-copy receive
io_uring_prep_recv(sqe, sockfd, buffer, len, 0);
io_uring_submit(&ring);
```

**예상 개선:** 5-10μs → 0.5-1μs (10x 개선)

---

## 2. Order Execution via WebSocket

### 현재 (REST API)
```
Signal → HTTP Request → TLS Handshake → Server → Response
         ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
                      50-200 ms
```

### 최적화 (WebSocket Private Channel)
```
Signal → Existing WS Connection → Server → Response
         ~~~~~~~~~~~~~~~~~~~~~~~~~
               5-20 ms
```

**거래소별 Private WebSocket 지원:**

| Exchange | Private WS | 주문 WebSocket |
|----------|------------|----------------|
| Upbit    | ❌         | REST only      |
| Bithumb  | ❌         | REST only      |
| Bybit    | ✅         | wss://stream.bybit.com/v5/private |
| Gate.io  | ✅         | wss://fx-ws.gateio.ws/v4/ws/usdt |

**예상 개선:** 50-200ms → 5-20ms (10x 개선)

---

## 3. CPU Pinning & NUMA

### 현재
```
Thread → Any CPU Core → Context switches → Cache misses
```

### 최적화
```cpp
// Thread를 특정 CPU 코어에 고정
void pin_to_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// 권장 배치:
// Core 0: WebSocket I/O (Upbit)
// Core 1: WebSocket I/O (Bithumb)
// Core 2: WebSocket I/O (Bybit)
// Core 3: WebSocket I/O (Gate.io)
// Core 4: Strategy Engine
// Core 5: Order Execution
// Core 6-7: Reserved for OS
```

**예상 개선:** p99 latency 20-30% 감소

---

## 4. Huge Pages

### 현재 (4KB pages)
```
Virtual Address → TLB lookup → Page Table Walk (4 levels) → Physical
                               ~~~~~~~~~~~~~~~~~~~~~~~~
                                    Cache miss = 100ns+
```

### 최적화 (2MB huge pages)
```bash
# Linux: Enable huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# C++ allocation
void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
```

**예상 개선:** TLB miss 감소로 5-10% 전반적 성능 향상

---

## 5. Binary Protocol (FlatBuffers)

### 현재 (JSON)
```json
{"symbol":"BTCUSDT","price":52345.67,"qty":1.234}
```
→ simdjson 파싱: ~100ns

### 최적화 (FlatBuffers)
```cpp
// Zero-copy access
auto ticker = GetTicker(buffer);
double price = ticker->price();  // Direct memory access
```
→ FlatBuffers 접근: ~5ns (20x 빠름)

**단점:** 거래소 프로토콜 변경 불가, 내부 통신에만 적용 가능

---

## 6. Busy-Wait Polling

### 현재 (Sleep/Yield)
```cpp
while (!queue.try_pop(item)) {
    std::this_thread::yield();  // Context switch overhead
}
```

### 최적화 (Busy-Wait with CPU hints)
```cpp
while (!queue.try_pop(item)) {
    _mm_pause();  // CPU yield hint, no context switch
}
```

**주의:** CPU 100% 사용, 전용 서버 필요

---

## 7. Colocation (물리적 최적화)

### 현재
```
Your Server (한국/해외) ←→ Exchange Server
              ~1-50ms RTT
```

### 최적화
```
Your Server (거래소 데이터센터 내) ←→ Exchange Server
              ~0.1-0.5ms RTT
```

**비용:** 월 $500-5000 (거래소별 상이)

---

## WebSocket 전략: 모든 코인 구독

### 권장: 예, 모든 공통 심볼 구독

```cpp
// 공통 심볼 (Korean Spot ∩ Foreign Futures)
// 보통 50-100개 심볼

// 각 거래소 WebSocket 연결
Upbit WS    → Subscribe: ["KRW-BTC", "KRW-ETH", ...]  // ~50-100
Bithumb WS  → Subscribe: ["BTC_KRW", "ETH_KRW", ...]  // ~50-100
Bybit WS    → Subscribe: ["BTCUSDT", "ETHUSDT", ...]  // ~50-100
Gate.io WS  → Subscribe: ["BTC_USDT", "ETH_USDT", ...] // ~50-100
```

### Rate Limit 고려

| Exchange | WS Message Limit | 권장 심볼 수 |
|----------|------------------|-------------|
| Upbit    | 100 symbols/connection | 100 |
| Bithumb  | 50 symbols/connection | 50 |
| Bybit    | 200 topics/connection | 200 |
| Gate.io  | 20 topics/message | Multiple msgs |

### 다중 연결 전략 (필요시)
```cpp
// Bithumb은 50개 제한 → 2개 연결 사용
BithumbWS_1 → 심볼 1-50
BithumbWS_2 → 심볼 51-100
```

---

## 우선순위 최적화 순서

1. **Order Execution WebSocket** - 가장 큰 개선 (50-200ms → 5-20ms)
2. **CPU Pinning** - 쉬운 구현, 좋은 효과
3. **io_uring** - Linux 서버에서 효과적
4. **Colocation** - 비용 있지만 확실한 효과
5. **Huge Pages** - 서버 설정만으로 가능
