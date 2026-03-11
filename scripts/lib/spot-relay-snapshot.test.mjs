import test from 'node:test';
import assert from 'node:assert/strict';

import { DEFAULT_FEE_EVENTS } from './spot-relay-math.mjs';
import { buildRelaySnapshot } from './spot-relay-snapshot.mjs';

test('snapshot counts seen symbols separately from fresh symbols', () => {
  const now = 1_000_000;
  const bithumbBooks = new Map([
    ['USDT', { bestBid: 1300, bestAsk: 1301, bestAskQty: 1000, ts: now }],
    ['AAA', { bestAsk: 1000, bestAskQty: 10, ts: now }],
    ['BBB', { bestAsk: 1000, bestAskQty: 10, ts: now - 10_000 }],
  ]);
  const bybitBooks = new Map([
    ['AAA', { bid: 0.8, bidQty: 10, ts: now }],
    ['BBB', { bid: 0.8, bidQty: 10, ts: now }],
  ]);

  const snapshot = buildRelaySnapshot({
    candidateBases: ['AAA', 'BBB'],
    bithumbBooks,
    bybitBooks,
    now,
    staleMs: 5_000,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    feeEvents: DEFAULT_FEE_EVENTS,
    getBybitTakerFeeRate: () => 0.001,
  });

  assert.equal(snapshot.bithumbSeen, 2);
  assert.equal(snapshot.bybitSeen, 2);
  assert.equal(snapshot.bothSeen, 2);
  assert.equal(snapshot.freshRows.length, 1);
  assert.equal(snapshot.freshRows[0].base, 'AAA');
});

test('snapshot positiveOnly filters negative rows and keeps descending order', () => {
  const now = 1_000_000;
  const bithumbBooks = new Map([
    ['USDT', { bestBid: 1300, bestAsk: 1301, bestAskQty: 1000, ts: now }],
    ['AAA', { bestAsk: 1000, bestAskQty: 10, ts: now }],
    ['BBB', { bestAsk: 1000, bestAskQty: 10, ts: now }],
    ['CCC', { bestAsk: 1000, bestAskQty: 10, ts: now }],
  ]);
  const bybitBooks = new Map([
    ['AAA', { bid: 0.82, bidQty: 10, ts: now }],
    ['BBB', { bid: 0.8, bidQty: 10, ts: now }],
    ['CCC', { bid: 0.771, bidQty: 10, ts: now }],
  ]);

  const snapshot = buildRelaySnapshot({
    candidateBases: ['AAA', 'BBB', 'CCC'],
    bithumbBooks,
    bybitBooks,
    now,
    staleMs: 5_000,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    feeEvents: DEFAULT_FEE_EVENTS,
    getBybitTakerFeeRate: () => 0.001,
    positiveOnly: true,
  });

  assert.equal(snapshot.freshRows.length, 3);
  assert.equal(snapshot.shownRows.length, 2);
  assert.equal(snapshot.filteredNegative, 1);
  assert.deepEqual(snapshot.shownRows.map((row) => row.base), ['AAA', 'BBB']);
  assert.ok(snapshot.shownRows[0].netEdgePct > snapshot.shownRows[1].netEdgePct);
});

test('snapshot keeps all rows when positiveOnly is false', () => {
  const now = 1_000_000;
  const bithumbBooks = new Map([
    ['USDT', { bestBid: 1300, bestAsk: 1301, bestAskQty: 1000, ts: now }],
    ['AAA', { bestAsk: 1000, bestAskQty: 10, ts: now }],
    ['CCC', { bestAsk: 1000, bestAskQty: 10, ts: now }],
  ]);
  const bybitBooks = new Map([
    ['AAA', { bid: 0.8, bidQty: 10, ts: now }],
    ['CCC', { bid: 0.771, bidQty: 10, ts: now }],
  ]);

  const snapshot = buildRelaySnapshot({
    candidateBases: ['AAA', 'CCC'],
    bithumbBooks,
    bybitBooks,
    now,
    staleMs: 5_000,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    feeEvents: DEFAULT_FEE_EVENTS,
    getBybitTakerFeeRate: () => 0.001,
    positiveOnly: false,
  });

  assert.equal(snapshot.freshRows.length, 2);
  assert.equal(snapshot.shownRows.length, 2);
  assert.equal(snapshot.filteredNegative, 0);
  assert.ok(snapshot.shownRows.some((row) => row.netEdgePct < 0));
});

test('snapshot excludes rows when usdt bid is stale', () => {
  const now = 1_000_000;
  const bithumbBooks = new Map([
    ['USDT', { bestBid: 1300, bestAsk: 1301, bestAskQty: 1000, ts: now - 9_000 }],
    ['AAA', { bestAsk: 1000, bestAskQty: 10, ts: now }],
  ]);
  const bybitBooks = new Map([
    ['AAA', { bid: 0.8, bidQty: 10, ts: now }],
  ]);

  const snapshot = buildRelaySnapshot({
    candidateBases: ['AAA'],
    bithumbBooks,
    bybitBooks,
    now,
    staleMs: 5_000,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    feeEvents: DEFAULT_FEE_EVENTS,
    getBybitTakerFeeRate: () => 0.001,
  });

  assert.equal(snapshot.bothSeen, 1);
  assert.equal(snapshot.freshRows.length, 0);
  assert.equal(snapshot.shownRows.length, 0);
});
