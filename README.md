<h1 align="center">Spot-Spot Relay Arbitrage Engine</h1>

<p align="center">
  <strong>Low-latency KRW premium execution engine for Korean spot vs offshore spot margin hedging</strong>
</p>

<p align="center">
  C++20 &bull; Lock-free &bull; SIMD &bull; WebSocket &bull; Delta-neutral
</p>

<p align="center">
  <a href="#architecture">Architecture</a> &bull;
  <a href="#execution-flow">Execution Flow</a> &bull;
  <a href="#safety">Safety</a> &bull;
  <a href="#performance">Performance</a> &bull;
  <a href="#build">Build</a>
</p>

---

## Overview

A high-frequency delta-neutral arbitrage engine that captures the **Kimchi Premium** — the persistent price gap between Korean crypto exchanges (Bithumb) and offshore exchanges (Bybit).

```
Bithumb (KRW Spot)          Bybit (USDT Spot Margin)
     BUY  ◄────── Premium Gap ──────►  SHORT

     When premium is high → Enter (buy Korean, short foreign)
     When premium drops   → Exit  (sell Korean, cover foreign)
     Profit = premium captured - fees
```

### Core Constraints

| Rule | Detail |
|------|--------|
| **No futures** | Spot-only on both sides — no funding rate risk |
| **No GateIO** | Bybit spot margin only |
| **Target coins** | Common pairs across Bithumb KRW + Bybit USDT spot margin |
| **Entry gate** | 70 USDT per split, both sides 1-tick instant fill, net edge > 0 after fees |
| **Fee model** | Bithumb ×1 (buy) + Bybit ×3 (borrow + short + cover) |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                     MARKET DATA (WebSocket)                         │
│         Bithumb          Bybit          OKX          Upbit          │
│     ticker + depth    orderbook      orderbook      ticker          │
└──────────┬───────────────┬──────────────┬──────────────┬────────────┘
           │  simdjson      │  simdjson     │              │
           │  fast-parse    │  ondemand     │              │
           ▼               ▼              ▼              ▼
    ┌─────────────────────────────────────────────────────────┐
    │              PriceCache (64 shards, lock-free BBO)       │
    │         atomic reads — zero mutex on hot path            │
    └────────────────────────┬────────────────────────────────┘
                             │
                             ▼
    ┌─────────────────────────────────────────────────────────┐
    │              ArbitrageEngine                             │
    │  • Premium calc (SIMD: AVX2 4x / NEON 2x)              │
    │  • Spread guards (Korean + Foreign)                      │
    │  • Quote freshness check (MAX_QUOTE_AGE_MS)             │
    │  • Entry/exit signal generation                          │
    └────────────────────────┬────────────────────────────────┘
                             │
                ┌────────────┴────────────┐
                ▼                         ▼
         Entry Signal                Exit Signal
                │                         │
                ▼                         ▼
    ┌─────────────────────────────────────────────────────────┐
    │         LifecycleExecutor (4-8 fixed workers)            │
    │              SPSC Ring Buffer — lock-free                │
    └────────────────────────┬────────────────────────────────┘
                             │
                             ▼
    ┌─────────────────────────────────────────────────────────┐
    │                   OrderManager                           │
    │                                                          │
    │  ENTRY:                          EXIT:                   │
    │  1. Bybit SHORT (WS)            1. Bybit COVER (WS)     │
    │  2. Wait fill confirmation      2. Wait fill             │
    │  3. Bithumb BUY (REST)          3. Bithumb SELL (REST)   │
    │  4. Parallel fill queries       4. Parallel fill queries │
    │  5. Delta hedge if mismatch     5. Delta hedge if needed │
    │  6. Position registered         6. P&L realized          │
    └─────────────────────────────────────────────────────────┘
```

---

## Execution Flow

### Entry (Sequential — guarantees delta-neutral)

```
Signal: Premium > threshold + net edge > 0
  │
  ├─ 1. Bybit spot margin SHORT (via Trade WebSocket)
  │     └─ Failed? → Skip, retry next cycle
  │
  ├─ 2. Confirm fill → actual_filled quantity
  │
  ├─ 3. Bithumb BUY exact actual_filled amount (REST)
  │     └─ Failed? → Rollback Bybit SHORT (cover)
  │                    └─ Rollback failed? → CRITICAL STOP
  │
  ├─ 4. Parallel fill queries (std::latch)
  │
  ├─ 5. quantities_match(foreign, korean)?
  │     ├─ Match → Position registered ✅
  │     ├─ foreign > korean → Cover excess short
  │     └─ korean > foreign → Sell excess spot
  │           └─ Correction failed? → CRITICAL STOP
  │
  └─ 6. Still mismatched after correction? → CRITICAL STOP
```

> **Why sequential?** The foreign short fills first, then the Korean buy uses the exact filled quantity. This guarantees `short_qty == long_qty` — perfect delta-neutral. Parallel would risk quantity mismatch.

### Exit (Mirror of entry)

```
Signal: Premium dropped below exit threshold
  │
  ├─ 1. Bybit COVER (buy to close short)
  ├─ 2. Bithumb SELL (exact matched quantity)
  ├─ 3. Delta hedge if mismatch
  └─ 4. Realized P&L calculated
```

---

## Safety

### Delta-Neutral Guarantees

| Layer | Mechanism |
|-------|-----------|
| **Quantity matching** | `quantities_match()` — relative tolerance `1e-6` (0.0001%) |
| **Auto correction** | `flatten_extra_foreign_short()` / `flatten_extra_korean_long()` |
| **Critical stop** | Bot halts + position snapshot preserved if correction fails |
| **Rollback** | Failed Korean buy → automatic foreign short cover |
| **Quote freshness** | Entry: 700ms max age, Exit: 8000ms max age |
| **Spread guard** | Rejects entry if spread too wide on either side |
| **Min profit floor** | `MIN_ENTRY_NET_PROFIT_KRW = 300` — no entry below this |

### Failure Modes

| Scenario | Response | Risk Exposure |
|----------|----------|---------------|
| Foreign short fails | Skip cycle | None |
| Korean buy fails | Cover foreign short | None (if cover succeeds) |
| Quantity mismatch | Auto delta hedge | None (if hedge succeeds) |
| Delta hedge fails | **CRITICAL STOP** | Logged mismatch position |
| Exchange disconnect | Auto-reconnect + resubscribe | Monitoring paused |

---

## Performance

### Latency Profile

| Stage | Latency | Bottleneck |
|-------|---------|------------|
| WS message → PriceCache | ~10-50µs | simdjson parse + atomic store |
| PriceCache → Signal | ~1-5µs | SIMD premium calc + threshold check |
| Signal → Bybit WS order | ~5-20ms | Network RTT |
| Bybit fill confirmation | ~10-50ms | Exchange processing |
| Bithumb REST order | ~100-300ms | REST API (no WS order API) |
| **Total entry** | **~150-400ms** | Bithumb REST dominates |

### Low-Latency Design

| Component | Technique |
|-----------|-----------|
| **Price updates** | Lock-free atomic BBO reads (0 mutex on ticker path) |
| **JSON parsing** | simdjson ondemand (zero heap alloc) + manual fast-parse fallback |
| **Premium calc** | SIMD batch: AVX2 (4x parallel) / NEON (2x) / scalar fallback |
| **Signal queue** | SPSC ring buffer, cache-line aligned, lock-free |
| **Workers** | Fixed thread pool (no spawn/join overhead) |
| **Memory** | Stack-allocated signals, pre-allocated orders, no `new`/`delete` on hot path |
| **Connections** | Pre-warmed SSL, TCP_NODELAY, HTTP keep-alive, connection pool |
| **CPU** | Core pinning, SCHED_FIFO realtime priority (Linux) |

---

## Configuration

```yaml
# Key trading parameters (types.hpp::TradingConfig)
MAX_POSITIONS: 1-4                    # Concurrent arbitrage positions
TARGET_ENTRY_USDT: 35.0               # Per-split notional
MAX_POSITIONS_USD: 3000.0             # Per-exchange risk budget
MIN_NET_EDGE_PCT: 0.0                 # Entry threshold (after fees)
MIN_ENTRY_NET_PROFIT_KRW: 300.0       # Minimum profit floor per entry
BITHUMB_FEE_RATE: 0.04%              # Maker/taker
BYBIT_FEE_RATE: 0.10%                # Borrow + trade
MAX_QUOTE_AGE_MS: 700                 # Entry quote freshness
MAX_QUOTE_AGE_MS_EXIT: 8000           # Exit quote freshness (relaxed)
ENTRY_FAST_SCAN_COOLDOWN_MS: 20       # Re-entry debounce
ENTRY_STALL_TIMEOUT_MS: 120000        # Finalize partial position if stuck
```

---

## Project Structure

```
kimp_arb_cpp/
├── include/kimp/
│   ├── core/           # Types, config, logger, SIMD helpers, latency probe
│   ├── exchange/       # Bithumb, Bybit, OKX, Upbit connectors
│   ├── execution/      # OrderManager, LifecycleExecutor (worker pool)
│   ├── memory/         # Lock-free SPSC/MPMC ring buffers, atomic bitset
│   ├── network/        # WebSocket client, HTTP connection pool, broadcast server
│   ├── strategy/       # ArbitrageEngine, PriceCache, signal generation
│   └── utils/          # HMAC-SHA512/256 crypto for exchange auth
├── src/                # Implementation files
├── config/             # YAML runtime configuration
└── tests/              # Test binaries
```

---

## Build

```bash
# Dependencies: Boost, OpenSSL, simdjson, spdlog, fmt, yaml-cpp, jwt-cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j $(nproc)
```

### Run

```bash
# Monitor only (no trading)
./build/kimp_bot --monitor-only

# Live trading
./build/kimp_bot --config config/production.yaml
```

---

## Exchanges

| Exchange | Role | Connection | Order API |
|----------|------|------------|-----------|
| **Bithumb** | Korean spot (BUY/SELL) | WS (ticker + depth) + REST (orders) | REST only |
| **Bybit** | Foreign spot margin (SHORT/COVER) | WS (orderbook + private) + Trade WS (orders) | WebSocket |
| **OKX** | Alt foreign (monitoring) | WS (orderbook) | REST |
| **Upbit** | Alt Korean (monitoring) | WS (ticker) | REST |

---

<p align="center">
  <sub>Delta-neutral spot-spot arbitrage — no futures, no funding, pure premium capture</sub>
</p>
