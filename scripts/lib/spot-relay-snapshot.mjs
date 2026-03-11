import { calculateRelayMetrics } from './spot-relay-math.mjs';

export function buildRelaySnapshot({
  candidateBases,
  bithumbBooks,
  bybitBooks,
  now,
  staleMs,
  targetUsdt,
  bithumbFeeRate,
  feeEvents,
  getBybitTakerFeeRate,
  positiveOnly = true,
}) {
  const usdt = bithumbBooks.get('USDT');
  const usdtBid = usdt?.bestBid || 0;

  let bithumbSeen = 0;
  let bybitSeen = 0;
  let bothSeen = 0;
  const freshRows = [];

  for (const base of candidateBases) {
    const kr = bithumbBooks.get(base);
    const by = bybitBooks.get(base);
    const krSeen = !!kr && kr.bestAsk > 0;
    const bySeen = !!by && by.bid > 0;

    if (krSeen) bithumbSeen += 1;
    if (bySeen) bybitSeen += 1;
    if (krSeen && bySeen) bothSeen += 1;

    if (!krSeen || !bySeen || usdtBid <= 0) continue;
    if (now - kr.ts > staleMs || now - by.ts > staleMs || now - (usdt?.ts || 0) > staleMs) continue;

    const bybitTakerFeeRate = getBybitTakerFeeRate(base);
    const metrics = calculateRelayMetrics({
      bithumbAsk: kr.bestAsk,
      bithumbAskQty: kr.bestAskQty,
      bybitBid: by.bid,
      bybitBidQty: by.bidQty,
      usdtBid,
      targetUsdt,
      bithumbFeeRate,
      bybitTakerFeeRate,
      feeEvents,
    });
    if (metrics.matchQty <= 0) continue;

    freshRows.push({
      base,
      bithumbAsk: kr.bestAsk,
      bithumbAskQty: kr.bestAskQty,
      bybitBid: by.bid,
      bybitBidQty: by.bidQty,
      bybitTakerFeeRate,
      usdtBid,
      ...metrics,
      ageMs: Math.max(now - kr.ts, now - by.ts),
    });
  }

  freshRows.sort((a, b) => b.netEdgePct - a.netEdgePct);
  const shownRows = positiveOnly
    ? freshRows.filter((row) => row.netEdgePct > 0)
    : freshRows;
  const filteredNegative = freshRows.length - shownRows.length;

  return {
    usdtBid,
    bithumbSeen,
    bybitSeen,
    bothSeen,
    freshRows,
    shownRows,
    filteredNegative,
  };
}
