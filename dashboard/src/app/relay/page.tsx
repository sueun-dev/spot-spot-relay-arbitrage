'use client'

import { useEffect, useMemo, useState } from 'react'

interface RelaySummary {
  tracked: number
  bithumbSeen: number
  bybitSeen: number
  bothSeen: number
  fresh: number
  shown: number
  positiveShown: number
  filteredNegative: number
  positiveOnly: boolean
  targetUsdt: number
  usdtBid: number
  exchangeRateLabel: string
  bithumbFeeRate: number
  bybitFeeSummary: string
}

interface RelayRow {
  base: string
  bithumbAsk: number
  bithumbAskQty: number
  bithumbTopKrw: number
  bithumbTopUsdt: number
  bybitBid: number
  bybitBidQty: number
  bybitTopUsdt: number
  bybitTopKrw: number
  matchQty: number
  maxTradableUsdtAtBest: number
  targetCoinQty: number
  bithumbCanFillTarget: boolean
  bybitCanFillTarget: boolean
  bothCanFillTarget: boolean
  grossEdgePct: number
  netEdgePct: number
  bithumbTradeFeeKrw: number
  bybitTradeFeeUsdt: number
  bybitTradeFeeKrw: number
  netProfitKrw: number
  ageMs: number
}

interface RelayPayload {
  ts: number
  summary: RelaySummary
  rows: RelayRow[]
}

const EMPTY_PAYLOAD: RelayPayload = {
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
    targetUsdt: 70,
    usdtBid: 0,
    exchangeRateLabel: '원화/달러',
    bithumbFeeRate: 0.0004,
    bybitFeeSummary: '',
  },
  rows: [],
}

export default function RelayDashboardPage() {
  const [payload, setPayload] = useState<RelayPayload>(EMPTY_PAYLOAD)
  const [filter, setFilter] = useState<'positive' | 'all'>('positive')
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let cancelled = false

    const fetchRelay = async () => {
      try {
        const res = await fetch('/api/relay', { cache: 'no-store' })
        if (!res.ok) return
        const data = (await res.json()) as RelayPayload
        if (!cancelled) {
          setPayload(data)
          setLoading(false)
        }
      } catch {
        // Keep last snapshot.
      }
    }

    fetchRelay()
    const timer = setInterval(fetchRelay, 500)

    return () => {
      cancelled = true
      clearInterval(timer)
    }
  }, [])

  const rows = useMemo(() => {
    if (filter === 'positive') {
      return payload.rows.filter((row) => row.netEdgePct > 0)
    }
    return payload.rows
  }, [filter, payload.rows])

  return (
    <main className="min-h-screen bg-black text-zinc-100">
      <div className="mx-auto flex max-w-[1800px] flex-col gap-6 px-6 py-8">
        <header className="border border-zinc-800 bg-zinc-950/90 p-5">
          <div className="flex flex-wrap items-start justify-between gap-4">
            <div>
              <p className="text-xs uppercase tracking-[0.35em] text-zinc-500">Spot Relay Monitor</p>
              <h1 className="mt-2 text-3xl font-semibold text-white">빗썸 → 바이비트 실시간 중계 대시보드</h1>
              <p className="mt-2 text-sm text-zinc-400">
                양쪽 웹소켓 수신 현황, 1틱 호가 잔량, 70달러 기준 진입 가능 여부, 순손익 기준 상위 코인을 한 화면에서 본다.
              </p>
            </div>
            <div className="text-right text-sm text-zinc-400">
              <div>마지막 갱신: {payload.ts ? new Date(payload.ts).toLocaleTimeString('ko-KR') : '대기 중'}</div>
              <div>환율: {fmt(payload.summary.usdtBid, 2)} {payload.summary.exchangeRateLabel}</div>
            </div>
          </div>
        </header>

        <section className="grid gap-3 md:grid-cols-2 xl:grid-cols-6">
          <MetricCard label="추적 코인" value={String(payload.summary.tracked)} />
          <MetricCard label="빗썸 수신" value={String(payload.summary.bithumbSeen)} />
          <MetricCard label="바이비트 수신" value={String(payload.summary.bybitSeen)} />
          <MetricCard label="양쪽 수신" value={String(payload.summary.bothSeen)} />
          <MetricCard label="Fresh" value={String(payload.summary.fresh)} />
          <MetricCard label="양수 순차익" value={String(payload.summary.positiveShown)} accent="lime" />
          <MetricCard label="목표 달러" value={`${fmt(payload.summary.targetUsdt, 2)} USD`} accent="lime" />
        </section>

        <section className="border border-zinc-800 bg-zinc-950/90 p-4 text-sm text-zinc-300">
          <div>빗썸 수수료: {(payload.summary.bithumbFeeRate * 100).toFixed(4)}%</div>
          <div>바이비트 수수료: {payload.summary.bybitFeeSummary || '대기 중'}</div>
          <div>필터 제외: {payload.summary.filteredNegative}개</div>
          <div>1틱 기준 목표 금액: {fmt(payload.summary.targetUsdt, 2)} 달러</div>
        </section>

        <section className="flex flex-wrap items-center justify-between gap-3">
          <div className="flex gap-2">
            <button
              className={filterButtonClass(filter === 'positive')}
              onClick={() => setFilter('positive')}
            >
              양수만
            </button>
            <button
              className={filterButtonClass(filter === 'all')}
              onClick={() => setFilter('all')}
            >
              전체
            </button>
          </div>
          <div className="text-sm text-zinc-400">
            현재 표시: {rows.length}개
          </div>
        </section>

        <section className="overflow-hidden border border-zinc-800 bg-zinc-950/90">
          <div className="overflow-x-auto">
            <table className="min-w-full border-collapse text-sm">
              <thead className="sticky top-0 bg-zinc-900 text-zinc-300">
                <tr className="border-b border-zinc-800">
                  {[
                    '코인',
                    '빗썸 매수가(원화)',
                    '빗썸 수량(코인)',
                    '빗썸 총액(원화)',
                    '빗썸 총액(달러)',
                    '바이비트 매도가(달러)',
                    '바이비트 수량(코인)',
                    '바이비트 총액(달러)',
                    '바이비트 총액(원화)',
                    '체결수량(코인)',
                    '1틱 최대진입(달러)',
                    `${fmt(payload.summary.targetUsdt, 2)}달러 필요수량`,
                    '양쪽 1틱 가능',
                    '총차익(%)',
                    '순차익(%)',
                    '빗썸 수수료(원화)',
                    '바이비트 수수료(달러)',
                    '바이비트 수수료(원화)',
                    '순손익(원화)',
                    '지연(ms)',
                  ].map((label) => (
                    <th key={label} className="whitespace-nowrap px-4 py-3 text-right font-medium first:text-left">
                      {label}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map((row) => (
                  <tr key={row.base} className="border-b border-zinc-900 text-zinc-100">
                    <td className="whitespace-nowrap px-4 py-3 text-left font-semibold text-white">{row.base}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bithumbAsk, 8)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bithumbAskQty, 8)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bithumbTopKrw, 0)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bithumbTopUsdt, 4)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitBid, 8)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitBidQty, 8)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitTopUsdt, 4)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitTopKrw, 0)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.matchQty, 8)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right text-cyan-300">{fmt(row.maxTradableUsdtAtBest, 4)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.targetCoinQty, 8)}</td>
                    <td className={`whitespace-nowrap px-4 py-3 text-right font-semibold ${row.bothCanFillTarget ? 'text-lime-300' : 'text-rose-300'}`}>
                      {row.bothCanFillTarget ? '가능' : '불가'}
                    </td>
                    <td className="whitespace-nowrap px-4 py-3 text-right text-cyan-300">{pct(row.grossEdgePct)}</td>
                    <td className={`whitespace-nowrap px-4 py-3 text-right ${row.netEdgePct > 0 ? 'text-lime-300' : 'text-rose-300'}`}>
                      {pct(row.netEdgePct)}
                    </td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bithumbTradeFeeKrw, 2)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitTradeFeeUsdt, 6)}</td>
                    <td className="whitespace-nowrap px-4 py-3 text-right">{fmt(row.bybitTradeFeeKrw, 2)}</td>
                    <td className={`whitespace-nowrap px-4 py-3 text-right ${row.netProfitKrw > 0 ? 'text-lime-300' : 'text-rose-300'}`}>
                      {fmt(row.netProfitKrw, 2)}
                    </td>
                    <td className="whitespace-nowrap px-4 py-3 text-right text-zinc-400">{row.ageMs}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {!loading && rows.length === 0 && (
            <div className="px-6 py-10 text-center text-zinc-500">표시할 행이 없다.</div>
          )}
          {loading && (
            <div className="px-6 py-10 text-center text-zinc-500">데이터 연결 중...</div>
          )}
        </section>
      </div>
    </main>
  )
}

function MetricCard({
  label,
  value,
  accent = 'zinc',
}: {
  label: string
  value: string
  accent?: 'zinc' | 'lime'
}) {
  const accentClass = accent === 'lime' ? 'text-lime-300' : 'text-white'

  return (
    <div className="border border-zinc-800 bg-zinc-950/90 p-4">
      <div className="text-xs uppercase tracking-[0.24em] text-zinc-500">{label}</div>
      <div className={`mt-3 text-3xl font-semibold ${accentClass}`}>{value}</div>
    </div>
  )
}

function filterButtonClass(active: boolean) {
  return active
    ? 'border border-lime-400 bg-lime-400/10 px-4 py-2 text-sm text-lime-200'
    : 'border border-zinc-700 bg-zinc-900 px-4 py-2 text-sm text-zinc-300'
}

function fmt(value: number, digits: number) {
  return Number(value || 0).toLocaleString('ko-KR', {
    maximumFractionDigits: digits,
    minimumFractionDigits: 0,
  })
}

function pct(value: number) {
  const sign = value >= 0 ? '+' : ''
  return `${sign}${value.toFixed(6)}%`
}
