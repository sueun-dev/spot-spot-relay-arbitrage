import test from 'node:test';
import assert from 'node:assert/strict';

import {
  buildFeeModelLabel,
  calculateRelayMetrics,
  DEFAULT_FEE_EVENTS,
  normalizeFeeEvents,
} from './spot-relay-math.mjs';

test('default fee events are bithumb 1 and bybit 3', () => {
  assert.deepEqual(DEFAULT_FEE_EVENTS, { bithumb: 1, bybit: 3 });
  assert.deepEqual(normalizeFeeEvents(), { bithumb: 1, bybit: 3 });
  assert.deepEqual(normalizeFeeEvents({ bithumb: 0, bybit: -7 }), { bithumb: 1, bybit: 3 });
  assert.equal(buildFeeModelLabel(DEFAULT_FEE_EVENTS), '빗썸 1회 + 바이비트 3회');
});

test('net profit subtracts bithumb once and bybit three times', () => {
  const row = calculateRelayMetrics({
    bithumbAsk: 1000,
    bithumbAskQty: 10,
    bybitBid: 0.8,
    bybitBidQty: 10,
    usdtBid: 1300,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    bybitTakerFeeRate: 0.001,
    feeEvents: DEFAULT_FEE_EVENTS,
  });

  assert.equal(row.matchQty, 10);
  assert.equal(row.matchBuyKrw, 10000);
  assert.equal(row.matchSellKrw, 10400);
  assert.equal(row.bithumbTotalFeeKrw, 4);
  assert.equal(row.bybitTotalFeeUsdt, 0.024);
  assert.ok(Math.abs(row.bybitTotalFeeKrw - 31.2) < 1e-12);
  assert.ok(Math.abs(row.totalFeeKrw - 35.2) < 1e-12);
  assert.ok(Math.abs(row.netProfitKrw - 364.8) < 1e-12);
  const expectedNetEdgePct = (364.8 / 10035.2) * 100;
  assert.ok(Math.abs(row.netEdgePct - expectedNetEdgePct) < 1e-12);
});

test('small spread turns negative after fee model is applied', () => {
  const row = calculateRelayMetrics({
    bithumbAsk: 1000,
    bithumbAskQty: 10,
    bybitBid: 0.771,
    bybitBidQty: 10,
    usdtBid: 1300,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    bybitTakerFeeRate: 0.001,
    feeEvents: DEFAULT_FEE_EVENTS,
  });

  assert.equal(row.matchSellKrw, 10023);
  assert.equal(row.grossSpreadKrw, 23);
  assert.ok(row.netProfitKrw < 0);
  assert.ok(row.netEdgePct < 0);
});

test('target fill requires both venues to have enough top-of-book quantity', () => {
  const row = calculateRelayMetrics({
    bithumbAsk: 1000,
    bithumbAskQty: 50,
    bybitBid: 2,
    bybitBidQty: 34,
    usdtBid: 1300,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    bybitTakerFeeRate: 0.001,
    feeEvents: DEFAULT_FEE_EVENTS,
  });

  assert.equal(row.targetCoinQty, 35);
  assert.equal(row.bithumbCanFillTarget, true);
  assert.equal(row.bybitCanFillTarget, false);
  assert.equal(row.bothCanFillTarget, false);
});

test('target fill passes exactly at 70 USDT boundary', () => {
  const row = calculateRelayMetrics({
    bithumbAsk: 1000,
    bithumbAskQty: 35,
    bybitBid: 2,
    bybitBidQty: 35,
    usdtBid: 1300,
    targetUsdt: 70,
    bithumbFeeRate: 0.0004,
    bybitTakerFeeRate: 0.001,
    feeEvents: DEFAULT_FEE_EVENTS,
  });

  assert.equal(row.targetCoinQty, 35);
  assert.equal(row.bithumbCanFillTarget, true);
  assert.equal(row.bybitCanFillTarget, true);
  assert.equal(row.bothCanFillTarget, true);
});
