import { NextResponse } from 'next/server'
import { promises as fs } from 'fs'
import path from 'path'

export const dynamic = 'force-dynamic'

export async function GET() {
  const candidates = [
    process.env.KIMP_RELAY_PATH,
    path.join(process.cwd(), '..', 'data', 'spot-relay-live.json'),
    path.join(process.cwd(), 'data', 'spot-relay-live.json'),
  ].filter((value): value is string => typeof value === 'string' && value.length > 0)

  for (const candidate of candidates) {
    try {
      const raw = await fs.readFile(candidate, 'utf-8')
      return NextResponse.json(JSON.parse(raw))
    } catch {
      // Try next path.
    }
  }

  return NextResponse.json({
    ts: 0,
    summary: {
      tracked: 0,
      bithumbSeen: 0,
      bybitSeen: 0,
      bothSeen: 0,
      fresh: 0,
      shown: 0,
      positiveShown: 0,
      filteredNegative: 0,
      positiveOnly: true,
      usdtBid: 0,
      exchangeRateLabel: '원화/달러',
      bithumbFeeRate: 0.0004,
      bybitFeeSummary: '',
    },
    rows: [],
  })
}
