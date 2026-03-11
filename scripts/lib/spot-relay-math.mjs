export const DEFAULT_FEE_EVENTS = Object.freeze({
  bithumb: 1,
  bybit: 3,
});

export function normalizeFeeEvents({
  bithumb = DEFAULT_FEE_EVENTS.bithumb,
  bybit = DEFAULT_FEE_EVENTS.bybit,
} = {}) {
  return {
    bithumb: normalizePositiveInteger(bithumb, DEFAULT_FEE_EVENTS.bithumb),
    bybit: normalizePositiveInteger(bybit, DEFAULT_FEE_EVENTS.bybit),
  };
}

export function buildFeeModelLabel(feeEvents) {
  return `빗썸 ${feeEvents.bithumb}회 + 바이비트 ${feeEvents.bybit}회`;
}

export function calculateRelayMetrics({
  bithumbAsk,
  bithumbAskQty,
  bybitBid,
  bybitBidQty,
  usdtBid,
  targetUsdt,
  bithumbFeeRate,
  bybitTakerFeeRate,
  feeEvents = DEFAULT_FEE_EVENTS,
}) {
  const normalizedFeeEvents = normalizeFeeEvents(feeEvents);
  const matchQty = Math.min(bithumbAskQty, bybitBidQty);

  const bithumbTopKrw = bithumbAsk * bithumbAskQty;
  const bithumbTopUsdt = usdtBid > 0 ? bithumbTopKrw / usdtBid : 0;
  const bybitTopUsdt = bybitBid * bybitBidQty;
  const bybitTopKrw = bybitTopUsdt * usdtBid;
  const matchBuyKrw = bithumbAsk * matchQty;
  const matchSellUsdt = bybitBid * matchQty;
  const matchSellKrw = matchSellUsdt * usdtBid;
  const maxTradableUsdtAtBest = bybitBid * matchQty;
  const targetCoinQty = bybitBid > 0 ? targetUsdt / bybitBid : 0;
  const bithumbCanFillTarget = bithumbAskQty >= targetCoinQty;
  const bybitCanFillTarget = bybitBidQty >= targetCoinQty;
  const bothCanFillTarget = bithumbCanFillTarget && bybitCanFillTarget;

  const bithumbFeePerTradeKrw = matchBuyKrw * bithumbFeeRate;
  const bybitFeePerTradeUsdt = matchSellUsdt * bybitTakerFeeRate;
  const bybitFeePerTradeKrw = matchSellKrw * bybitTakerFeeRate;
  const bithumbTotalFeeKrw = bithumbFeePerTradeKrw * normalizedFeeEvents.bithumb;
  const bybitTotalFeeUsdt = bybitFeePerTradeUsdt * normalizedFeeEvents.bybit;
  const bybitTotalFeeKrw = bybitFeePerTradeKrw * normalizedFeeEvents.bybit;
  const totalFeeKrw = bithumbTotalFeeKrw + bybitTotalFeeKrw;

  const grossSpreadKrw = matchSellKrw - matchBuyKrw;
  const grossEdgePct = matchBuyKrw > 0 ? (grossSpreadKrw / matchBuyKrw) * 100 : 0;
  const netProfitKrw = grossSpreadKrw - totalFeeKrw;
  const netBasisKrw = matchBuyKrw + totalFeeKrw;
  const netEdgePct = netBasisKrw > 0 ? (netProfitKrw / netBasisKrw) * 100 : 0;

  return {
    feeEvents: normalizedFeeEvents,
    matchQty,
    bithumbTopKrw,
    bithumbTopUsdt,
    bybitTopUsdt,
    bybitTopKrw,
    matchBuyKrw,
    matchSellUsdt,
    matchSellKrw,
    maxTradableUsdtAtBest,
    targetCoinQty,
    bithumbCanFillTarget,
    bybitCanFillTarget,
    bothCanFillTarget,
    bithumbFeePerTradeKrw,
    bybitFeePerTradeUsdt,
    bybitFeePerTradeKrw,
    bithumbTotalFeeKrw,
    bybitTotalFeeUsdt,
    bybitTotalFeeKrw,
    totalFeeKrw,
    grossSpreadKrw,
    grossEdgePct,
    netProfitKrw,
    netBasisKrw,
    netEdgePct,
  };
}

function normalizePositiveInteger(value, fallback) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return fallback;
  }
  return Math.floor(parsed);
}
