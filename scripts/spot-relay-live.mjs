#!/usr/bin/env node
import crypto from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import {
  buildFeeModelLabel,
  normalizeFeeEvents,
} from './lib/spot-relay-math.mjs';
import { buildRelaySnapshot } from './lib/spot-relay-snapshot.mjs';

const DEFAULT_BITHUMB_WS_URL = 'wss://pubwss.bithumb.com/pub/ws';
const DEFAULT_BYBIT_SPOT_WS_URL = 'wss://stream.bybit.com/v5/public/spot';
const DEFAULT_BITHUMB_ORDERBOOK_URL = 'https://api.bithumb.com/public/orderbook/ALL_KRW?count=5';
const DEFAULT_BYBIT_INSTRUMENTS_URL = 'https://api.bybit.com/v5/market/instruments-info?category=spot&limit=1000';
const DEFAULT_BYBIT_FEE_RATE_URL = 'https://api.bybit.com/v5/account/fee-rate';
const BITHUMB_TAKER_FEE_RATE = 0.0004;
const BYBIT_DEFAULT_SPOT_TAKER_FEE_RATE = 0.001;
const BYBIT_FEE_RATE_DELAY_MS = 220;

const args = parseArgs(process.argv.slice(2));
const displayTop = args.top ?? null;
const refreshMs = args.refreshMs ?? 250;
const staleMs = args.staleMs ?? 30000;
const runDurationMs = args.durationSec ? args.durationSec * 1000 : 0;
const clearScreen = !!args.clear && !args.noClear;
const priceDigits = args.priceDigits ?? 16;
const qtyDigits = args.qtyDigits ?? 12;
const edgeDigits = args.edgeDigits ?? 8;
const positiveOnly = !args.includeNegative;
const bithumbFeeRate = args.bithumbFeeRate ?? BITHUMB_TAKER_FEE_RATE;
const bybitFeeOverride = args.bybitFeeRate ?? null;
const targetUsdt = args.targetUsdt ?? 70;
const feeEvents = normalizeFeeEvents({
  bithumb: args.bithumbFeeEvents,
  bybit: args.bybitFeeEvents,
});
const bithumbWsUrl = args.bithumbWsUrl ?? DEFAULT_BITHUMB_WS_URL;
const bybitWsUrl = args.bybitWsUrl ?? DEFAULT_BYBIT_SPOT_WS_URL;
const bithumbOrderbookUrl = args.bithumbOrderbookUrl ?? DEFAULT_BITHUMB_ORDERBOOK_URL;
const bybitInstrumentsUrl = args.bybitInstrumentsUrl ?? DEFAULT_BYBIT_INSTRUMENTS_URL;
const bybitFeeRateUrl = args.bybitFeeRateUrl ?? DEFAULT_BYBIT_FEE_RATE_URL;
const jsonOutPath = args.jsonOut
  ? path.resolve(args.jsonOut)
  : path.resolve(process.cwd(), 'data', 'spot-relay-live.json');

const bithumbBooks = new Map();
const bybitBooks = new Map();
let candidateBases = [];
let renderTimer = null;
let pingTimer = null;
let stopTimer = null;
let shuttingDown = false;
let bithumbWs = null;
let bybitWs = null;
let snapshotWriteQueue = Promise.resolve();
const bybitFeeState = {
  defaultTakerRate: bybitFeeOverride ?? BYBIT_DEFAULT_SPOT_TAKER_FEE_RATE,
  perSymbolTakerRates: new Map(),
  exactSymbols: 0,
  totalSymbols: 0,
  source: bybitFeeOverride == null
    ? 'official VIP0 fallback'
    : 'manual override',
};

main().catch((error) => {
  console.error('[spot-relay-live] fatal:', error);
  process.exit(1);
});

async function main() {
  const snapshot = await fetchJson(bithumbOrderbookUrl);
  if (snapshot.status !== '0000') {
    throw new Error('Bithumb orderbook snapshot failed');
  }

  seedBithumbBooks(snapshot.data);
  const marginEnabledBases = await fetchBybitMarginEnabledBases();

  candidateBases = [...bithumbBooks.keys()]
    .filter((base) => base !== 'USDT' && marginEnabledBases.has(base))
    .sort();

  if (candidateBases.length === 0) {
    throw new Error('No common Bithumb/Bybit margin-enabled spot symbols found');
  }

  bybitFeeState.totalSymbols = candidateBases.length;
  void warmBybitSpotFeeRates(candidateBases);

  console.log(
    `[spot-relay-live] monitoring ${candidateBases.length} common symbols ` +
    `(Bybit public margin-enabled spot only)`
  );

  connectBithumb();
  connectBybit();

  renderTimer = setInterval(render, refreshMs);
  render();

  if (runDurationMs > 0) {
    stopTimer = setTimeout(() => shutdown(0), runDurationMs);
  }

  process.on('SIGINT', () => shutdown(0));
  process.on('SIGTERM', () => shutdown(0));
}

function parseArgs(argv) {
  const parsed = {};
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--top' && argv[i + 1]) {
      parsed.top = Number(argv[++i]);
    } else if (arg === '--refresh-ms' && argv[i + 1]) {
      parsed.refreshMs = Number(argv[++i]);
    } else if (arg === '--stale-ms' && argv[i + 1]) {
      parsed.staleMs = Number(argv[++i]);
    } else if (arg === '--duration-sec' && argv[i + 1]) {
      parsed.durationSec = Number(argv[++i]);
    } else if (arg === '--clear') {
      parsed.clear = true;
    } else if (arg === '--no-clear') {
      parsed.noClear = true;
    } else if (arg === '--price-digits' && argv[i + 1]) {
      parsed.priceDigits = Number(argv[++i]);
    } else if (arg === '--qty-digits' && argv[i + 1]) {
      parsed.qtyDigits = Number(argv[++i]);
    } else if (arg === '--edge-digits' && argv[i + 1]) {
      parsed.edgeDigits = Number(argv[++i]);
    } else if (arg === '--bithumb-fee-rate' && argv[i + 1]) {
      parsed.bithumbFeeRate = Number(argv[++i]);
    } else if (arg === '--bybit-fee-rate' && argv[i + 1]) {
      parsed.bybitFeeRate = Number(argv[++i]);
    } else if (arg === '--bithumb-fee-events' && argv[i + 1]) {
      parsed.bithumbFeeEvents = Number(argv[++i]);
    } else if (arg === '--bybit-fee-events' && argv[i + 1]) {
      parsed.bybitFeeEvents = Number(argv[++i]);
    } else if (arg === '--target-usdt' && argv[i + 1]) {
      parsed.targetUsdt = Number(argv[++i]);
    } else if (arg === '--json-out' && argv[i + 1]) {
      parsed.jsonOut = argv[++i];
    } else if (arg === '--bithumb-ws-url' && argv[i + 1]) {
      parsed.bithumbWsUrl = argv[++i];
    } else if (arg === '--bybit-ws-url' && argv[i + 1]) {
      parsed.bybitWsUrl = argv[++i];
    } else if (arg === '--bithumb-orderbook-url' && argv[i + 1]) {
      parsed.bithumbOrderbookUrl = argv[++i];
    } else if (arg === '--bybit-instruments-url' && argv[i + 1]) {
      parsed.bybitInstrumentsUrl = argv[++i];
    } else if (arg === '--bybit-fee-rate-url' && argv[i + 1]) {
      parsed.bybitFeeRateUrl = argv[++i];
    } else if (arg === '--include-negative') {
      parsed.includeNegative = true;
    }
  }
  return parsed;
}

async function fetchJson(url) {
  const response = await fetch(url, {
    headers: { 'user-agent': 'kimchi-premium-arb/spot-relay-live' },
  });
  if (!response.ok) {
    throw new Error(`HTTP ${response.status} for ${url}`);
  }
  return response.json();
}

async function fetchBybitMarginEnabledBases() {
  const bases = new Set();
  let cursor = '';

  while (true) {
    const url = new URL(bybitInstrumentsUrl);
    if (cursor) {
      url.searchParams.set('cursor', cursor);
    }
    const payload = await fetchJson(url.toString());
    if (payload.retCode !== 0) {
      throw new Error(`Bybit instruments error: ${payload.retMsg}`);
    }
    const result = payload.result ?? {};
    for (const item of result.list ?? []) {
      if (item.quoteCoin !== 'USDT') continue;
      if (item.status !== 'Trading') continue;
      if (String(item.marginTrading || '').toLowerCase() === 'none') continue;
      bases.add(item.baseCoin);
    }
    cursor = result.nextPageCursor || '';
    if (!cursor) break;
  }

  return bases;
}

async function warmBybitSpotFeeRates(bases) {
  if (bybitFeeOverride != null) {
    bybitFeeState.source = 'manual override';
    return;
  }

  const apiKey = process.env.BYBIT_API_KEY;
  const apiSecret = process.env.BYBIT_SECRET_KEY;
  if (!apiKey || !apiSecret) {
    bybitFeeState.source = 'official VIP0 fallback (no BYBIT_API_KEY/BYBIT_SECRET_KEY)';
    return;
  }

  bybitFeeState.source = 'account fee-rate API';

  try {
    for (const base of bases) {
      const symbol = `${base}USDT`;
      const takerRate = await fetchBybitSpotTakerFeeRate(symbol, apiKey, apiSecret);
      if (Number.isFinite(takerRate) && takerRate >= 0) {
        bybitFeeState.perSymbolTakerRates.set(base, takerRate);
        bybitFeeState.exactSymbols += 1;
      }
      await sleep(BYBIT_FEE_RATE_DELAY_MS);
    }
  } catch (error) {
    bybitFeeState.perSymbolTakerRates.clear();
    bybitFeeState.exactSymbols = 0;
    bybitFeeState.source = 'official VIP0 fallback (fee-rate API unavailable)';
    console.error('[spot-relay-live] bybit fee-rate fallback:', error.message);
  }
}

async function fetchBybitSpotTakerFeeRate(symbol, apiKey, apiSecret) {
  const queryString = new URLSearchParams({
    category: 'spot',
    symbol,
  }).toString();
  const timestamp = String(Date.now());
  const recvWindow = '5000';
  const signaturePayload = `${timestamp}${apiKey}${recvWindow}${queryString}`;
  const signature = crypto
    .createHmac('sha256', apiSecret)
    .update(signaturePayload)
    .digest('hex');

  const response = await fetch(`${bybitFeeRateUrl}?${queryString}`, {
    headers: {
      'user-agent': 'kimchi-premium-arb/spot-relay-live',
      'X-BAPI-API-KEY': apiKey,
      'X-BAPI-TIMESTAMP': timestamp,
      'X-BAPI-RECV-WINDOW': recvWindow,
      'X-BAPI-SIGN': signature,
    },
  });

  if (!response.ok) {
    throw new Error(`Bybit fee-rate HTTP ${response.status}`);
  }

  const payload = await response.json();
  if (payload.retCode !== 0) {
    throw new Error(`Bybit fee-rate retCode=${payload.retCode} retMsg=${payload.retMsg}`);
  }

  const rate = Number(payload.result?.list?.[0]?.takerFeeRate);
  return Number.isFinite(rate) ? rate : null;
}

function getBybitTakerFeeRate(base) {
  return bybitFeeState.perSymbolTakerRates.get(base) ?? bybitFeeState.defaultTakerRate;
}

function getBybitFeeSummary() {
  const pct = (bybitFeeState.defaultTakerRate * 100).toFixed(4);
  if (bybitFeeState.exactSymbols > 0) {
    return `Bybit taker=per-symbol API (${bybitFeeState.exactSymbols}/${bybitFeeState.totalSymbols} exact, fallback ${pct}%)`;
  }
  return `Bybit taker=${pct}% (${bybitFeeState.source})`;
}

function getFeeModelSummary() {
  return `${buildFeeModelLabel(feeEvents)} 기준`;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function seedBithumbBooks(data) {
  for (const [base, value] of Object.entries(data)) {
    if (base === 'timestamp' || base === 'payment_currency') continue;

    const book = {
      bids: new Map(),
      asks: new Map(),
      bestBid: 0,
      bestBidQty: 0,
      bestAsk: 0,
      bestAskQty: 0,
      ts: Date.now(),
    };

    for (const bid of value.bids ?? []) {
      const price = Number(bid.price);
      const qty = Number(bid.quantity);
      if (price > 0 && qty > 0) {
        book.bids.set(price, qty);
      }
    }
    for (const ask of value.asks ?? []) {
      const price = Number(ask.price);
      const qty = Number(ask.quantity);
      if (price > 0 && qty > 0) {
        book.asks.set(price, qty);
      }
    }

    recomputeBestBid(book);
    recomputeBestAsk(book);
    bithumbBooks.set(base, book);
  }
}

function connectBithumb() {
  bithumbWs = new WebSocket(bithumbWsUrl);

  bithumbWs.addEventListener('open', () => {
    const symbols = [...candidateBases, 'USDT'].map((base) => `${base}_KRW`);
    bithumbWs.send(JSON.stringify({ type: 'orderbookdepth', symbols }));
  });

  bithumbWs.addEventListener('message', (event) => {
    try {
      const msg = JSON.parse(String(event.data));
      if (msg.type !== 'orderbookdepth') return;
      const touched = new Set();
      for (const row of msg.content?.list ?? []) {
        const [base] = String(row.symbol || '').split('_');
        if (!base) continue;
        const book = bithumbBooks.get(base);
        if (!book) continue;

        const side = row.orderType === 'bid' ? 'bids' : 'asks';
        const bestKey = row.orderType === 'bid' ? 'bestBid' : 'bestAsk';
        const bestQtyKey = row.orderType === 'bid' ? 'bestBidQty' : 'bestAskQty';
        const price = Number(row.price);
        const qty = Number(row.quantity);
        if (!Number.isFinite(price) || price <= 0) continue;

        if (!Number.isFinite(qty) || qty <= 0) {
          book[side].delete(price);
          if (book[bestKey] === price) {
            if (row.orderType === 'bid') recomputeBestBid(book);
            else recomputeBestAsk(book);
          }
        } else {
          book[side].set(price, qty);
          if (row.orderType === 'bid') {
            if (price > book.bestBid || book.bestBid === 0) {
              book.bestBid = price;
              book.bestBidQty = qty;
            } else if (price === book.bestBid) {
              book.bestBidQty = qty;
            }
          } else {
            if (price < book.bestAsk || book.bestAsk === 0) {
              book.bestAsk = price;
              book.bestAskQty = qty;
            } else if (price === book.bestAsk) {
              book.bestAskQty = qty;
            }
          }
        }
        book.ts = Date.now();
        touched.add(base);
      }

      for (const base of touched) {
        const book = bithumbBooks.get(base);
        if (!book) continue;
        if (book.bestBidQty === 0 && book.bestBid > 0) recomputeBestBid(book);
        if (book.bestAskQty === 0 && book.bestAsk > 0) recomputeBestAsk(book);
      }
    } catch (error) {
      console.error('[bithumb] parse error:', error.message);
    }
  });

  bithumbWs.addEventListener('close', () => {
    if (!shuttingDown) {
      setTimeout(connectBithumb, 1000);
    }
  });

  bithumbWs.addEventListener('error', () => {
    try { bithumbWs.close(); } catch {}
  });
}

function connectBybit() {
  bybitWs = new WebSocket(bybitWsUrl);

  bybitWs.addEventListener('open', () => {
    const batchSize = 10;
    for (let i = 0; i < candidateBases.length; i += batchSize) {
      const batch = candidateBases.slice(i, i + batchSize)
        .map((base) => `orderbook.1.${base}USDT`);
      bybitWs.send(JSON.stringify({ op: 'subscribe', args: batch }));
    }

    if (pingTimer) clearInterval(pingTimer);
    pingTimer = setInterval(() => {
      if (bybitWs && bybitWs.readyState === WebSocket.OPEN) {
        bybitWs.send(JSON.stringify({ op: 'ping' }));
      }
    }, 20000);
  });

  bybitWs.addEventListener('message', (event) => {
    try {
      const msg = JSON.parse(String(event.data));
      if (!msg.topic || !msg.topic.startsWith('orderbook.1.')) return;

      const symbol = msg.data?.s || msg.topic.split('.').pop();
      if (!symbol || !symbol.endsWith('USDT')) return;
      const base = symbol.slice(0, -4);

      const bidRow = Array.isArray(msg.data?.b) && msg.data.b.length > 0 ? msg.data.b[0] : null;
      const askRow = Array.isArray(msg.data?.a) && msg.data.a.length > 0 ? msg.data.a[0] : null;
      const bid = bidRow ? Number(bidRow[0]) : 0;
      const bidQty = bidRow ? Number(bidRow[1]) : 0;
      const ask = askRow ? Number(askRow[0]) : 0;
      const askQty = askRow ? Number(askRow[1]) : 0;

      bybitBooks.set(base, {
        bid,
        bidQty,
        ask,
        askQty,
        ts: Date.now(),
      });
    } catch (error) {
      console.error('[bybit] parse error:', error.message);
    }
  });

  bybitWs.addEventListener('close', () => {
    if (pingTimer) {
      clearInterval(pingTimer);
      pingTimer = null;
    }
    if (!shuttingDown) {
      setTimeout(connectBybit, 1000);
    }
  });

  bybitWs.addEventListener('error', () => {
    try { bybitWs.close(); } catch {}
  });
}

function recomputeBestBid(book) {
  let bestPrice = 0;
  let bestQty = 0;
  for (const [price, qty] of book.bids.entries()) {
    if (qty <= 0) continue;
    if (price > bestPrice) {
      bestPrice = price;
      bestQty = qty;
    }
  }
  book.bestBid = bestPrice;
  book.bestBidQty = bestQty;
}

function recomputeBestAsk(book) {
  let bestPrice = 0;
  let bestQty = 0;
  for (const [price, qty] of book.asks.entries()) {
    if (qty <= 0) continue;
    if (bestPrice === 0 || price < bestPrice) {
      bestPrice = price;
      bestQty = qty;
    }
  }
  book.bestAsk = bestPrice;
  book.bestAskQty = bestQty;
}

function render() {
  const now = Date.now();
  const {
    usdtBid,
    bithumbSeen,
    bybitSeen,
    bothSeen,
    freshRows,
    shownRows,
    filteredNegative,
  } = buildRelaySnapshot({
    candidateBases,
    bithumbBooks,
    bybitBooks,
    now,
    staleMs,
    targetUsdt,
    bithumbFeeRate,
    feeEvents,
    getBybitTakerFeeRate,
    positiveOnly,
  });
  const visibleRows = displayTop ? shownRows.slice(0, displayTop) : shownRows;
  persistSnapshot({
    ts: now,
    summary: {
      tracked: candidateBases.length,
      bithumbSeen,
      bybitSeen,
      bothSeen,
      fresh: freshRows.length,
      shown: shownRows.length,
      positiveShown: positiveOnly ? shownRows.length : freshRows.filter((row) => row.netEdgePct > 0).length,
      filteredNegative,
      positiveOnly,
      targetUsdt,
      usdtBid,
      exchangeRateLabel: '원화/USDT',
      bithumbFeeRate,
      bithumbFeeEvents: feeEvents.bithumb,
      bybitFeeEvents: feeEvents.bybit,
      feeModelLabel: getFeeModelSummary(),
      bybitFeeSummary: getBybitFeeSummary(),
    },
    rows: freshRows,
  });

  if (clearScreen) {
    process.stdout.write('\x1b[2J\x1b[H');
  } else {
    console.log('');
  }
  console.log(
    `=== Spot Relay Live | ${new Date().toLocaleTimeString('ko-KR')} | ` +
    `tracked=${candidateBases.length} | bithumbSeen=${bithumbSeen} | bybitSeen=${bybitSeen} | bothSeen=${bothSeen} | fresh=${freshRows.length} | ${positiveOnly ? 'positiveShown' : 'shown'}=${shownRows.length} | 환율=${fmtFloat(usdtBid, priceDigits)} 원화/USDT ===`
  );
  console.log(
    `Fees: Bithumb taker=${(bithumbFeeRate * 100).toFixed(4)}% x ${feeEvents.bithumb}회 | ${getBybitFeeSummary()} x ${feeEvents.bybit}회`
  );
  console.log(`Fee model: ${getFeeModelSummary()} 반영`);
  console.log(
    positiveOnly
      ? 'Logic: Bithumb best ask buy vs Bybit spot best bid sell, positive net edges only by default'
      : 'Logic: Bithumb best ask buy vs Bybit spot best bid sell, showing both positive and negative net edges'
  );
  console.log(`Target: ${fmtFloat(targetUsdt, 2)} USDT 기준 1틱 체결 가능 여부 포함`);
  if (positiveOnly) {
    console.log(`Filter: positive net only, negative/zero filtered=${filteredNegative}`);
  }
  printTable(visibleRows, [
    { title: 'Coin', align: 'left', minWidth: 5, value: (row) => row.base },
    { title: '빗썸 매수가(원화)', minWidth: 16, value: (row) => fmtFloat(row.bithumbAsk, priceDigits) },
    { title: '빗썸 수량(코인)', minWidth: 16, value: (row) => fmtFloat(row.bithumbAskQty, qtyDigits) },
    { title: '빗썸 총액(원화)', minWidth: 16, value: (row) => fmtFloat(row.bithumbTopKrw, 0) },
    { title: '빗썸 총액(USDT)', minWidth: 16, value: (row) => fmtFloat(row.bithumbTopUsdt, 4) },
    { title: '바이비트 매도가(USDT)', minWidth: 18, value: (row) => fmtFloat(row.bybitBid, priceDigits) },
    { title: '바이비트 수량(코인)', minWidth: 18, value: (row) => fmtFloat(row.bybitBidQty, qtyDigits) },
    { title: '바이비트 총액(USDT)', minWidth: 18, value: (row) => fmtFloat(row.bybitTopUsdt, 4) },
    { title: '바이비트 총액(원화)', minWidth: 18, value: (row) => fmtFloat(row.bybitTopKrw, 0) },
    { title: '체결수량(코인)', minWidth: 14, value: (row) => fmtFloat(row.matchQty, qtyDigits) },
    { title: '1틱 최대진입(USDT)', minWidth: 18, value: (row) => fmtFloat(row.maxTradableUsdtAtBest, 4) },
    { title: `${fmtFloat(targetUsdt, 2)}USDT 필요수량`, minWidth: 18, value: (row) => fmtFloat(row.targetCoinQty, 8) },
    { title: '양쪽1틱가능', minWidth: 12, value: (row) => row.bothCanFillTarget ? '가능' : '불가' },
    { title: '총차익(%)', minWidth: 12, value: (row) => `${row.grossEdgePct.toFixed(edgeDigits)}%` },
    { title: '순차익(%)', minWidth: 12, value: (row) => `${row.netEdgePct.toFixed(edgeDigits)}%` },
    { title: '빗썸 총수수료(원화)', minWidth: 18, value: (row) => fmtFloat(row.bithumbTotalFeeKrw, 2) },
    { title: '바이비트 총수수료(USDT)', minWidth: 20, value: (row) => fmtFloat(row.bybitTotalFeeUsdt, 6) },
    { title: '바이비트 총수수료(원화)', minWidth: 20, value: (row) => fmtFloat(row.bybitTotalFeeKrw, 2) },
    { title: '총수수료(원화)', minWidth: 16, value: (row) => fmtFloat(row.totalFeeKrw, 2) },
    { title: '순손익(원화)', minWidth: 14, value: (row) => fmtFloat(row.netProfitKrw, 2) },
    { title: '지연(ms)', minWidth: 8, value: (row) => String(row.ageMs) },
  ]);

  if (visibleRows.length === 0) {
    console.log(
      positiveOnly
        ? `[spot-relay-live] no positive live opportunities in current freshness window (fresh=${freshRows.length}, filtered=${filteredNegative})`
        : '[spot-relay-live] no live opportunities in current freshness window'
    );
  }
}

function fmtFloat(value, digits) {
  return Number(value || 0).toLocaleString('en-US', {
    maximumFractionDigits: digits,
    minimumFractionDigits: 0,
  });
}

function printTable(rows, columns) {
  const renderedRows = rows.map((row) => columns.map((column) => String(column.value(row))));
  const widths = columns.map((column, index) => {
    let width = Math.max(column.title.length, column.minWidth ?? 0);
    for (const renderedRow of renderedRows) {
      width = Math.max(width, renderedRow[index].length);
    }
    return width;
  });

  const separator = `+-${widths.map((width) => '-'.repeat(width)).join('-+-')}-+`;
  const header = `| ${columns.map((column, index) => padCell(column.title, widths[index], column.align)).join(' | ')} |`;

  console.log(separator);
  console.log(header);
  console.log(separator);

  for (const renderedRow of renderedRows) {
    console.log(
      `| ${renderedRow.map((cell, index) => padCell(cell, widths[index], columns[index].align)).join(' | ')} |`
    );
  }

  console.log(separator);
}

function padCell(text, width, align = 'right') {
  const value = String(text);
  return align === 'left' ? value.padEnd(width) : value.padStart(width);
}

function persistSnapshot(snapshot) {
  snapshotWriteQueue = snapshotWriteQueue
    .catch(() => {})
    .then(async () => {
      await mkdir(path.dirname(jsonOutPath), { recursive: true });
      await writeFile(jsonOutPath, JSON.stringify(snapshot, null, 2));
    });
}

function shutdown(code) {
  if (shuttingDown) return;
  shuttingDown = true;

  if (renderTimer) clearInterval(renderTimer);
  if (pingTimer) clearInterval(pingTimer);
  if (stopTimer) clearTimeout(stopTimer);

  try { bithumbWs?.close(); } catch {}
  try { bybitWs?.close(); } catch {}

  setTimeout(() => process.exit(code), 100);
}
