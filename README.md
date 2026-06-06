<h1 align="center">Spot-Spot Relay Arbitrage Engine</h1>

<p align="center">
  <strong>Low-latency KRW premium execution engine for Korean spot vs offshore spot-margin hedging</strong>
</p>

<p align="center">
  C++20 &bull; Lock-free &bull; SIMD &bull; WebSocket &bull; Delta-neutral
</p>

<p align="center">
  <a href="#overview">Overview</a> &bull;
  <a href="#repository-layout">Repository</a> &bull;
  <a href="#architecture">Architecture</a> &bull;
  <a href="#execution-flow">Execution Flow</a> &bull;
  <a href="#safety">Safety</a> &bull;
  <a href="#performance">Performance</a> &bull;
  <a href="#build--run">Build &amp; Run</a>
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

The project ships in two parts:

- **`kimp_arb_cpp/`** — the C++20 execution engine (`kimp_bot`). This is the live trader: it ingests
  market data over WebSocket, computes net edge, and places hedged orders on Bithumb + Bybit.
- **A Node.js "spot-relay" monitoring layer** (`scripts/` + `dashboard/`) — a standalone read-only
  scanner that streams Bithumb/Bybit spot books, computes the same fee-adjusted relay edge, writes a
  live JSON snapshot, and renders it in a Next.js dashboard. It places **no orders**.

### Core Constraints

| Rule | Detail |
|------|--------|
| **No futures** | Spot-only on both sides — no funding rate risk |
| **No GateIO** | Bybit spot margin only |
| **Target coins** | Common pairs across Bithumb KRW + Bybit USDT spot margin |
| **Entry gate** | Per-add notional unit, both sides 1-tick instant fill, net edge > 0 after fees |
| **Fee model** | Bithumb ×1 (buy) + Bybit ×3 (borrow + short + cover) |

---

## Repository Layout

```
.
├── kimp_arb_cpp/          # C++20 execution engine (the live trading bot)
│   ├── include/kimp/      # Headers (core, exchange, execution, memory, network, strategy, utils)
│   ├── src/               # Implementation files + main.cpp (CLI entry point)
│   ├── config/config.yaml # Runtime configuration loaded by kimp_bot
│   ├── tests/             # Regression, benchmark, and live-smoke test binaries
│   ├── third_party/       # Vendored jwt-cpp (MIT)
│   ├── CMakeLists.txt     # Build definition (targets: kimp_bot + tests)
│   └── conanfile.py       # Conan dependency manifest
├── scripts/               # Node.js spot-relay monitor (read-only)
│   ├── spot-relay-live.mjs    # Live scanner → writes data/spot-relay-live.json
│   └── lib/                   # Edge math + snapshot builder (+ node:test suites)
├── dashboard/             # Next.js dashboard that visualizes the relay snapshot
├── run_bot.sh             # Build + run the live trading engine
├── run_monitor.sh         # Build + run the engine in monitor-only mode (no orders)
├── package.json           # npm scripts for the Node relay layer (relay:*)
└── MUSTREAD.md            # Operational runbook (paths, env, common errors) — Korean
```

---

## Architecture (C++ engine)

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
    │              PriceCache (sharded, lock-free BBO)         │
    │         atomic reads — zero mutex on hot path            │
    └────────────────────────┬────────────────────────────────┘
                             │
                             ▼
    ┌─────────────────────────────────────────────────────────┐
    │              ArbitrageEngine                             │
    │  • Premium calc (SIMD: AVX2 4x / NEON 2x / scalar)     │
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
    │              LifecycleExecutor (fixed worker pool)       │
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
| **Quantity matching** | `quantities_match()` — relative tolerance `HEDGE_QUANTITY_TOLERANCE_RATIO = 5e-5` (0.005%) |
| **Auto correction** | flatten extra foreign short / flatten extra Korean long |
| **Critical stop** | Bot halts + position snapshot preserved if correction fails |
| **Rollback** | Failed Korean buy → automatic foreign short cover |
| **Quote freshness** | Entry: 700ms max age, Exit: 8000ms max age |
| **Spread guard** | Rejects entry if spread too wide on either side |
| **Min profit floor** | `MIN_ENTRY_NET_PROFIT_KRW = 600` — no entry below this |

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

> Numbers above are design targets. Measure your own with `kimp_bot --latency-probe`
> (writes `trade_logs/latency_events.mmapbin`; add `--latency-probe-summary` for per-span CSV).

### Low-Latency Design

| Component | Technique |
|-----------|-----------|
| **Price updates** | Lock-free atomic BBO reads (0 mutex on ticker path) |
| **JSON parsing** | simdjson ondemand + manual fast-parse fallback |
| **Premium calc** | SIMD batch: AVX2 (4x parallel) / NEON (2x) / scalar fallback |
| **Signal queue** | SPSC ring buffer, cache-line aligned, lock-free |
| **Workers** | Fixed thread pool (no spawn/join overhead) |
| **Memory** | Stack-allocated signals, pre-allocated orders, no `new`/`delete` on hot path |
| **Connections** | Pre-warmed SSL, TCP_NODELAY, HTTP keep-alive, connection pool |
| **CPU** | Optional core pinning / realtime priority (configurable, off by default) |

---

## Configuration

Two layers configure the engine:

**1. Runtime YAML — `kimp_arb_cpp/config/config.yaml`** (loaded by `kimp_bot` at startup):

```yaml
trading:
  max_positions: 1               # Concurrent arbitrage positions (1–4)
  position_size_usd: 70.0        # Per-symbol side exposure budget
  order_size_usd: 70.0           # Notional per submitted order
  entry_premium_threshold: 0.0   # Enter when net edge is positive
  exit_premium_threshold: 0.25   # Exit floor (%)
```

**2. Compile-time defaults — `kimp_arb_cpp/include/kimp/core/types.hpp` (`TradingConfig`):**

```cpp
MAX_POSITIONS              = 1        // runtime-overridable, range 1–4
TARGET_ENTRY_USDT         = 35.0     // per-check entry unit (one add)
CAPITAL_PER_EXCHANGE_USD  = 3000.0   // risk budget per venue
MIN_NET_EDGE_PCT          = 0.0      // entry threshold (after fees)
MIN_ENTRY_NET_PROFIT_KRW  = 600.0    // minimum projected NetKRW per entry
BITHUMB_FEE_RATE          = 0.0004   // 0.04% taker (coupon)
BYBIT_FEE_RATE            = 0.0010   // 0.10% taker (VIP0)
MAX_QUOTE_AGE_MS          = 700      // entry quote freshness
MAX_QUOTE_AGE_MS_EXIT     = 8000     // exit quote freshness (relaxed)
ENTRY_FAST_SCAN_COOLDOWN_MS = 20     // re-entry debounce
ENTRY_STALL_TIMEOUT_MS    = 120000   // finalize partial position if stuck (2 min)
```

### Required credentials

`kimp_bot` reads exchange keys from environment variables (the `run_*.sh` scripts auto-load a
root `.env`). Live trading requires:

```
BITHUMB_API_KEY  BITHUMB_SECRET_KEY
BYBIT_API_KEY    BYBIT_SECRET_KEY
```

OKX/Upbit are monitor-only and use `OKX_*` / `UPBIT_*` if configured. `--monitor-only` runs without
private keys.

---

## Build & Run

### Prerequisites

- A C++20 compiler (Clang or GCC) and CMake ≥ 3.20
- [Conan 2](https://conan.io/) (resolves the C++ dependencies below)
- Dependencies (installed by Conan): Boost, OpenSSL, ZLIB, simdjson, spdlog, fmt, yaml-cpp
  (jwt-cpp is vendored under `third_party/`)
- For the optional monitoring layer: Node.js 18+ and npm

### Recommended: helper scripts

The scripts handle Conan install, the CMake preset, the correct build path, `.env` loading, and a
pre-flight network check — then launch `kimp_bot`. Run them from the repository root:

```bash
# Monitor only (no orders, no position prompts)
./run_monitor.sh --monitor-interval-sec 1

# Live trading (requires BITHUMB_/BYBIT_ keys in ./.env)
./run_bot.sh --monitor-interval-sec 1

# Show the full CLI help
./run_bot.sh --help
```

> See `MUSTREAD.md` for the operational runbook (path rules, env checks, and fixes for common
> runtime errors).

### Manual build (Conan + CMake)

```bash
cd kimp_arb_cpp
conan install . --output-folder=build --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build build/build/Release --target kimp_bot -j8

# The binary lands at: kimp_arb_cpp/build/build/Release/kimp_bot
./build/build/Release/kimp_bot --monitor-only
```

### Useful CLI flags (`kimp_bot`)

| Flag | Effect |
|------|--------|
| `-c, --config <path>` | Config file (default `config/config.yaml`) |
| `--monitor-only` | Monitor only — no auto-trading, no position prompts |
| `--monitor-interval-sec <n>` | Monitor refresh interval (default 2) |
| `--scan-spot-relay` | Scan Bithumb↔Bybit spot-transfer candidates and exit |
| `--show-balances` | Print non-zero balances on all configured exchanges |
| `--manual-confirm-once` | Wait for one live candidate and trade only after manual confirmation |
| `--dashboard-stream` | Enable JSON exporter + local relay WS output |
| `--latency-probe` | Record async latency events (`--latency-probe-summary` for CSV spans) |

### Tests

Test binaries are defined in `CMakeLists.txt` and built alongside the engine. Build a target, then
run it from the build directory, e.g.:

```bash
cmake --build build/build/Release --target kimp_test_entry kimp_test_order_manager_pnl -j8
./build/build/Release/kimp_test_entry
./build/build/Release/kimp_test_order_manager_pnl
```

Live-network smoke tests (`kimp_test_live_*`) require valid API keys; see
`kimp_arb_cpp/tests/LIVE_TESTS.md`.

---

## Spot-Relay Monitor & Dashboard (Node.js)

A standalone, **read-only** layer that streams Bithumb (KRW spot) and Bybit (USDT spot) order books,
computes the fee-adjusted relay edge, and writes a live snapshot for the dashboard. It never places
orders. From the repository root:

```bash
# 1. Install dashboard deps (one time)
npm --prefix ./dashboard install

# 2. Run the unit tests (node:test)
npm run relay:test

# 3a. Live scanner only → writes data/spot-relay-live.json
npm run relay:monitor

# 3b. Dashboard only (Next.js, http://localhost:3213)
npm run relay:dashboard

# 3c. Both together (scanner + dashboard)
npm run relay:web
```

The dashboard reads `data/spot-relay-live.json` (override with the `KIMP_RELAY_PATH` env var) and
serves on port **3213**.

---

## Exchanges

| Exchange | Role | Connection | Order API |
|----------|------|------------|-----------|
| **Bithumb** | Korean spot (BUY/SELL) | WS (ticker + depth) + REST (orders) | REST only |
| **Bybit** | Foreign spot margin (SHORT/COVER) | WS (orderbook + private) + Trade WS (orders) | WebSocket |
| **OKX** | Alt foreign (monitoring) | WS (orderbook) | REST |
| **Upbit** | Alt Korean (monitoring) | WS (ticker) | REST |

---

## License

No top-level license file is included; all rights reserved unless stated otherwise by the author.
The vendored `kimp_arb_cpp/third_party/jwt-cpp` library is distributed under its own MIT license.

---

<p align="center">
  <sub>Delta-neutral spot-spot arbitrage — no futures, no funding, pure premium capture</sub>
</p>
