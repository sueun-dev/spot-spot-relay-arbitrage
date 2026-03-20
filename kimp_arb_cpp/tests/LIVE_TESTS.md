# Live Test Commands

All commands assume:

```bash
cd /Volumes/DevSSD/Developer/Codebase/kimchi-premium-arbitrage-engine/kimp_arb_cpp/build/build/Release
```

The binaries auto-load `.env` at runtime. No manual `source .env` is required.

## 1. Readiness Smoke

```bash
./kimp_test_live_entry
./kimp_test_live_hedge
./kimp_test_parallel_fill
./kimp_test_multicoin_hedge
```

What they confirm:

- dotenv/config load
- private REST auth on 4 venues
- non-zero balance fetch
- common tradable universe
- no stale `EMPTY/SKIP` old-binary problem

## 2. Margin / Leverage Setup

Read-only for positions, but it can change account setup state:

- Bybit spot margin mode enable
- OKX cross-margin 1x leverage set

```bash
./kimp_test_live_margin_setup
./kimp_test_live_margin_setup --symbols ADA,SOL
```

Pass condition:

- `BybitMargin=PASS`
- `OKXCross1x=PASS`
- liabilities before/after should not jump unexpectedly

## 3. Pair Preflight Dry-Run

No orders are placed.

```bash
./kimp_test_live_pair_roundtrip --pair Bi-By --symbol ADA
./kimp_test_live_pair_roundtrip --pair Bi-Ok --symbol ADA
./kimp_test_live_pair_roundtrip --pair Up-By --symbol ADA
./kimp_test_live_pair_roundtrip --pair Up-Ok --symbol ADA
```

What to inspect:

- top-of-book ask/bid
- top-of-book size
- target quantity
- estimated KRW cost
- pre-trade base balance / foreign liability

## 4. Entry-Only Live Fill

Actually places entry legs.

```bash
./kimp_test_live_pair_roundtrip --pair Bi-By --symbol ADA --execute-entry-only --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Bi-Ok --symbol ADA --execute-entry-only --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Up-By --symbol ADA --execute-entry-only --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Up-Ok --symbol ADA --execute-entry-only --confirm LIVE
```

Pass condition:

- `ENTRY AUDIT` prints `MATCH`
- Korean base balance increases by roughly the filled amount
- foreign liability increases by roughly the filled amount

If `ENTRY AUDIT` prints `MISMATCH`, keep the output. That is still useful.

## 5. Full Roundtrip Live Fill

Actually places entry, then immediate exit.

```bash
./kimp_test_live_pair_roundtrip --pair Bi-By --symbol ADA --execute-roundtrip --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Bi-Ok --symbol ADA --execute-roundtrip --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Up-By --symbol ADA --execute-roundtrip --confirm LIVE
./kimp_test_live_pair_roundtrip --pair Up-Ok --symbol ADA --execute-roundtrip --confirm LIVE
```

Pass condition:

- `ENTRY AUDIT` is `MATCH`
- `EXIT AUDIT` is `MATCH`
- `RESIDUAL AUDIT` is near zero
- process exit code `0`

Useful failure signals:

- exit code `2`: quantity mismatch
- exit code `3`: residual Korean base or foreign liability remained
- exit code `1`: hard failure

## 6. What To Send Back

Send back:

- exact command used
- full stdout
- process exit code
- symbol and pair

For roundtrip runs, include:

- `ENTRY AUDIT`
- `EXIT AUDIT`
- `RESIDUAL AUDIT`
- `PNL ROUGH`
