'use client'

import { useEffect, useState, useRef, useCallback } from 'react'
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, ReferenceLine } from 'recharts'

interface PremiumData {
  symbol: string
  koreanPrice: number
  foreignPrice: number
  usdtRate: number
  premium: number
  fundingRate: number
  fundingIntervalHours: number
  nextFundingTime: number
  signal: 'ENTRY' | 'EXIT' | null
  timestamp: number
}

interface SystemStatus {
  connected: boolean
  upbitConnected: boolean
  bybitConnected: boolean
  symbolCount: number
  lastUpdate: number
  latencyMs: number  // WebSocket latency in ms
}

interface HistoryPoint {
  time: string
  timestamp: number
  koreanPrice: number
  foreignKrw: number
  premium: number
}

const MAX_HISTORY_POINTS = 600  // 10분 히스토리

export default function Dashboard() {
  const [premiums, setPremiums] = useState<PremiumData[]>([])
  const [status, setStatus] = useState<SystemStatus>({
    connected: false,
    upbitConnected: false,
    bybitConnected: false,
    symbolCount: 0,
    lastUpdate: 0,
    latencyMs: 0,
  })
  const [filter, setFilter] = useState<'all' | 'signals'>('all')
  const [sortBy, setSortBy] = useState<'premium' | 'symbol'>('premium')
  const [currentTime, setCurrentTime] = useState<string>('')
  const [mounted, setMounted] = useState(false)
  const [selectedSymbol, setSelectedSymbol] = useState<string | null>(null)
  const [chartTab, setChartTab] = useState<'price' | 'premium'>('premium')

  // History storage using ref to avoid re-renders
  const historyRef = useRef<Map<string, HistoryPoint[]>>(new Map())
  const lastHistoryUpdateRef = useRef<number>(0)

  // WebSocket ref to persist across strict mode double-mount
  const wsRef = useRef<WebSocket | null>(null)
  const reconnectTimerRef = useRef<NodeJS.Timeout | null>(null)

  const updateHistory = useCallback((newPremiums: PremiumData[]) => {
    const now = Date.now()
    // Only update history every 1 second (for chart smoothness)
    if (now - lastHistoryUpdateRef.current < 1000) {
      return
    }
    lastHistoryUpdateRef.current = now

    const timeStr = new Date().toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })

    newPremiums.forEach(p => {
      const history = historyRef.current.get(p.symbol) || []
      const newPoint: HistoryPoint = {
        time: timeStr,
        timestamp: now,
        koreanPrice: p.koreanPrice,
        foreignKrw: p.foreignPrice * p.usdtRate,
        premium: p.premium
      }

      history.push(newPoint)

      // Keep only last N points
      if (history.length > MAX_HISTORY_POINTS) {
        history.shift()
      }

      historyRef.current.set(p.symbol, history)
    })
  }, [])

  useEffect(() => {
    setMounted(true)

    // Initial fetch via HTTP (fallback)
    const fetchData = async () => {
      try {
        const res = await fetch('/api/premiums')
        if (res.ok) {
          const data = await res.json()
          if (data.premiums && data.premiums.length > 0) {
            setPremiums(data.premiums)
            updateHistory(data.premiums)
          }
          if (data.status) {
            setStatus(data.status)
          }
        }
      } catch (e) {
        console.error('Failed to fetch data:', e)
      }
    }

    const updateTime = () => {
      setCurrentTime(new Date().toLocaleTimeString('ko-KR'))
    }

    fetchData()
    updateTime()

    // WebSocket connection for REAL-TIME updates
    // Use refs to survive React strict mode double-mount
    const connectWebSocket = () => {
      // Don't create duplicate connections
      if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
        return
      }

      try {
        const ws = new WebSocket('ws://localhost:8765')
        wsRef.current = ws

        ws.onopen = () => {
          console.log('WebSocket connected - real-time mode active')
          setStatus(prev => ({ ...prev, connected: true }))
        }

        ws.onmessage = (event) => {
          try {
            const receiveTime = Date.now()
            const msg = JSON.parse(event.data)
            if (msg.type === 'premiums' && msg.data) {
              // Calculate latency from server timestamp
              const latencyMs = msg.ts ? (receiveTime - msg.ts) : 0

              // Convert compact format to full format
              const fullPremiums: PremiumData[] = msg.data.map((p: any) => ({
                symbol: p.s,
                koreanPrice: p.kp,
                foreignPrice: p.fp,
                usdtRate: p.r,
                premium: p.pm,
                fundingRate: p.fr,
                fundingIntervalHours: p.fi || 8,  // Use actual interval from server
                nextFundingTime: 0,
                signal: p.sg === 1 ? 'ENTRY' : p.sg === 2 ? 'EXIT' : null,
                timestamp: receiveTime
              }))
              setPremiums(fullPremiums)
              updateHistory(fullPremiums)
              setStatus(prev => ({
                ...prev,
                connected: true,
                symbolCount: fullPremiums.length,
                lastUpdate: receiveTime,
                latencyMs: latencyMs
              }))
            }
          } catch (e) {
            console.error('Failed to parse WebSocket message:', e)
          }
        }

        ws.onclose = () => {
          console.log('WebSocket disconnected, reconnecting...')
          setStatus(prev => ({ ...prev, connected: false }))
          wsRef.current = null
          reconnectTimerRef.current = setTimeout(connectWebSocket, 1000)
        }

        ws.onerror = () => {
          // Silently handle - onclose will trigger reconnect
        }
      } catch (e) {
        console.error('WebSocket connection failed:', e)
        reconnectTimerRef.current = setTimeout(connectWebSocket, 2000)
      }
    }

    connectWebSocket()

    // Fallback HTTP polling (if WebSocket fails)
    const dataInterval = setInterval(() => {
      if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) {
        fetchData()
      }
    }, 500)

    const timeInterval = setInterval(updateTime, 1000)

    return () => {
      // Only close if actually open
      if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
        wsRef.current.close()
      }
      if (reconnectTimerRef.current) clearTimeout(reconnectTimerRef.current)
      clearInterval(dataInterval)
      clearInterval(timeInterval)
    }
  }, [updateHistory])

  const filteredPremiums = premiums
    .filter(p => filter === 'all' || p.signal !== null)
    .sort((a, b) => {
      if (sortBy === 'premium') return a.premium - b.premium
      return a.symbol.localeCompare(b.symbol)
    })

  const formatPrice = (price: number, decimals: number = 0) => {
    return price.toLocaleString('ko-KR', { maximumFractionDigits: decimals })
  }

  // Smart USD price formatter - shows more decimals for small prices
  const formatUsdPrice = (price: number) => {
    if (price >= 100) return price.toFixed(2)
    if (price >= 1) return price.toFixed(4)
    if (price >= 0.01) return price.toFixed(6)
    return price.toFixed(8)
  }

  const formatPremium = (premium: number) => {
    const sign = premium >= 0 ? '+' : ''
    return `${sign}${premium.toFixed(2)}%`
  }

  const getPremiumClass = (premium: number) => {
    if (premium <= -0.75) return 'premium-negative'
    if (premium >= 1) return 'premium-positive'
    return 'premium-neutral'
  }

  const formatFundingRate = (rate: number) => {
    const percent = rate * 100
    const sign = percent >= 0 ? '+' : ''
    return `${sign}${percent.toFixed(4)}%`
  }

  const getFundingClass = (rate: number) => {
    if (rate > 0) return 'text-green-400'
    if (rate < 0) return 'text-red-400'
    return 'text-gray-400'
  }

  const entrySignals = premiums.filter(p => p.signal === 'ENTRY').length
  const exitSignals = premiums.filter(p => p.signal === 'EXIT').length

  const selectedPremium = premiums.find(p => p.symbol === selectedSymbol)
  const selectedHistory = selectedSymbol ? (historyRef.current.get(selectedSymbol) || []) : []

  // Custom tooltip for charts
  const PriceTooltip = ({ active, payload }: any) => {
    if (active && payload && payload.length) {
      const data = payload[0].payload
      return (
        <div className="bg-gray-900 border border-gray-600 rounded-lg p-3 text-sm">
          <p className="text-gray-400 mb-1">{data.time}</p>
          <p className="text-yellow-400">한국: ₩{formatPrice(data.koreanPrice)}</p>
          <p className="text-blue-400">해외: ₩{formatPrice(data.foreignKrw)}</p>
          <p className={data.premium >= 0 ? 'text-red-400' : 'text-green-400'}>
            프리미엄: {formatPremium(data.premium)}
          </p>
        </div>
      )
    }
    return null
  }

  const PremiumTooltip = ({ active, payload }: any) => {
    if (active && payload && payload.length) {
      const data = payload[0].payload
      return (
        <div className="bg-gray-900 border border-gray-600 rounded-lg p-3 text-sm">
          <p className="text-gray-400 mb-1">{data.time}</p>
          <p className={data.premium >= 0 ? 'text-red-400' : 'text-green-400'}>
            프리미엄: {formatPremium(data.premium)}
          </p>
        </div>
      )
    }
    return null
  }

  return (
    <div className="min-h-screen p-6">
      {/* Header */}
      <header className="mb-8">
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-3xl font-bold text-white">김프 모니터</h1>
            <p className="text-gray-400 mt-1">실시간 김치 프리미엄 차익거래</p>
          </div>
          <div className="flex items-center gap-4">
            <div className={`px-3 py-1 rounded-full text-sm ${status.connected ? 'bg-green-500/20 text-green-400' : 'bg-red-500/20 text-red-400'}`}>
              {status.connected ? '● 연결됨' : '○ 연결 끊김'}
            </div>
            <div className="text-gray-400 text-sm">
              {mounted ? currentTime : ''}
            </div>
          </div>
        </div>
      </header>

      {/* Stats Cards */}
      <div className="grid grid-cols-5 gap-4 mb-8">
        <div className="bg-gray-800/50 rounded-lg p-4 border border-gray-700">
          <div className="text-gray-400 text-sm">심볼 수</div>
          <div className="text-2xl font-bold text-white">{status.symbolCount}</div>
        </div>
        <div className="bg-gray-800/50 rounded-lg p-4 border border-gray-700">
          <div className="text-gray-400 text-sm">USDT/KRW 환율</div>
          <div className="text-2xl font-bold text-white">
            {premiums[0]?.usdtRate ? formatPrice(premiums[0].usdtRate, 2) : '-'}
          </div>
        </div>
        <div className={`rounded-lg p-4 border ${
          status.latencyMs <= 50 ? 'bg-green-500/10 border-green-500/50' :
          status.latencyMs <= 100 ? 'bg-yellow-500/10 border-yellow-500/50' :
          'bg-red-500/10 border-red-500/50'
        }`}>
          <div className="text-gray-400 text-sm">레이턴시</div>
          <div className={`text-2xl font-bold ${
            status.latencyMs <= 50 ? 'text-green-400' :
            status.latencyMs <= 100 ? 'text-yellow-400' :
            'text-red-400'
          }`}>
            {status.latencyMs}ms
          </div>
        </div>
        <div className={`rounded-lg p-4 border ${entrySignals > 0 ? 'bg-green-500/10 border-green-500/50 animate-pulse-green' : 'bg-gray-800/50 border-gray-700'}`}>
          <div className="text-gray-400 text-sm">역프 진입 기회 (−%)</div>
          <div className={`text-2xl font-bold ${entrySignals > 0 ? 'text-green-400' : 'text-white'}`}>{entrySignals}</div>
        </div>
        <div className={`rounded-lg p-4 border ${exitSignals > 0 ? 'bg-red-500/10 border-red-500/50 animate-pulse-red' : 'bg-gray-800/50 border-gray-700'}`}>
          <div className="text-gray-400 text-sm">김프 청산 기회 (+%)</div>
          <div className={`text-2xl font-bold ${exitSignals > 0 ? 'text-red-400' : 'text-white'}`}>{exitSignals}</div>
        </div>
      </div>

      {/* Chart Modal */}
      {selectedSymbol && selectedPremium && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50" onClick={() => setSelectedSymbol(null)}>
          <div
            className="bg-gray-900 rounded-xl border border-gray-700 w-[90%] max-w-4xl max-h-[85vh] overflow-hidden"
            onClick={e => e.stopPropagation()}
          >
            {/* Modal Header */}
            <div className="flex items-center justify-between p-4 border-b border-gray-700">
              <div className="flex items-center gap-4">
                <h2 className="text-xl font-bold text-white">{selectedSymbol}</h2>
                <span className={`text-lg font-bold ${getPremiumClass(selectedPremium.premium)}`}>
                  {formatPremium(selectedPremium.premium)}
                </span>
                {selectedPremium.signal && (
                  <span className={`px-2 py-1 rounded-full text-xs ${
                    selectedPremium.signal === 'ENTRY'
                      ? 'bg-green-500/20 text-green-400'
                      : 'bg-red-500/20 text-red-400'
                  }`}>
                    {selectedPremium.signal === 'ENTRY' ? '역프 진입' : '김프 청산'}
                  </span>
                )}
              </div>
              <button
                onClick={() => setSelectedSymbol(null)}
                className="text-gray-400 hover:text-white text-2xl leading-none"
              >
                ×
              </button>
            </div>

            {/* Current Prices */}
            <div className="grid grid-cols-3 gap-4 p-4 border-b border-gray-700">
              <div className="bg-gray-800/50 rounded-lg p-3">
                <div className="text-gray-400 text-xs">한국 (빗썸)</div>
                <div className="text-yellow-400 text-lg font-bold">₩{formatPrice(selectedPremium.koreanPrice)}</div>
              </div>
              <div className="bg-gray-800/50 rounded-lg p-3">
                <div className="text-gray-400 text-xs">해외 (바이빗)</div>
                <div className="text-blue-400 text-lg font-bold">
                  ${formatUsdPrice(selectedPremium.foreignPrice)}
                  <span className="text-gray-500 text-sm ml-2">
                    (₩{formatPrice(selectedPremium.foreignPrice * selectedPremium.usdtRate)})
                  </span>
                </div>
              </div>
              <div className="bg-gray-800/50 rounded-lg p-3">
                <div className="text-gray-400 text-xs">펀딩비 ({selectedPremium.fundingIntervalHours}h)</div>
                <div className={`text-lg font-bold ${getFundingClass(selectedPremium.fundingRate)}`}>
                  {formatFundingRate(selectedPremium.fundingRate)}
                </div>
              </div>
            </div>

            {/* Chart Tabs */}
            <div className="flex gap-2 p-4 pb-0">
              <button
                onClick={() => setChartTab('premium')}
                className={`px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
                  chartTab === 'premium'
                    ? 'bg-purple-500/20 text-purple-400 border border-purple-500/50'
                    : 'bg-gray-800 text-gray-400 hover:text-white'
                }`}
              >
                프리미엄 추이
              </button>
              <button
                onClick={() => setChartTab('price')}
                className={`px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
                  chartTab === 'price'
                    ? 'bg-blue-500/20 text-blue-400 border border-blue-500/50'
                    : 'bg-gray-800 text-gray-400 hover:text-white'
                }`}
              >
                가격 비교
              </button>
            </div>

            {/* Charts */}
            <div className="p-4 h-[400px]">
              {selectedHistory.length < 2 ? (
                <div className="h-full flex items-center justify-center text-gray-500">
                  데이터 수집 중... ({selectedHistory.length}개 포인트)
                </div>
              ) : chartTab === 'premium' ? (
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={selectedHistory}>
                    <XAxis
                      dataKey="time"
                      stroke="#6b7280"
                      tick={{ fill: '#9ca3af', fontSize: 11 }}
                      interval="preserveStartEnd"
                    />
                    <YAxis
                      stroke="#6b7280"
                      tick={{ fill: '#9ca3af', fontSize: 11 }}
                      tickFormatter={(v) => `${v.toFixed(1)}%`}
                      domain={['auto', 'auto']}
                    />
                    <Tooltip content={<PremiumTooltip />} />
                    <ReferenceLine y={0} stroke="#4b5563" strokeDasharray="3 3" />
                    <ReferenceLine y={-0.75} stroke="#22c55e" strokeDasharray="5 5" label={{ value: '진입', fill: '#22c55e', fontSize: 10 }} />
                    <ReferenceLine y={1.0} stroke="#ef4444" strokeDasharray="5 5" label={{ value: '청산', fill: '#ef4444', fontSize: 10 }} />
                    <Line
                      type="monotone"
                      dataKey="premium"
                      stroke="#a855f7"
                      strokeWidth={2}
                      dot={false}
                      isAnimationActive={false}
                    />
                  </LineChart>
                </ResponsiveContainer>
              ) : (
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={selectedHistory}>
                    <XAxis
                      dataKey="time"
                      stroke="#6b7280"
                      tick={{ fill: '#9ca3af', fontSize: 11 }}
                      interval="preserveStartEnd"
                    />
                    <YAxis
                      stroke="#6b7280"
                      tick={{ fill: '#9ca3af', fontSize: 11 }}
                      tickFormatter={(v) => `₩${(v/1000).toFixed(0)}k`}
                      domain={['auto', 'auto']}
                    />
                    <Tooltip content={<PriceTooltip />} />
                    <Line
                      type="monotone"
                      dataKey="koreanPrice"
                      stroke="#eab308"
                      strokeWidth={2}
                      dot={false}
                      name="한국"
                      isAnimationActive={false}
                    />
                    <Line
                      type="monotone"
                      dataKey="foreignKrw"
                      stroke="#3b82f6"
                      strokeWidth={2}
                      dot={false}
                      name="해외"
                      isAnimationActive={false}
                    />
                  </LineChart>
                </ResponsiveContainer>
              )}
            </div>

            {/* Legend */}
            <div className="px-4 pb-4 flex gap-6 text-sm">
              {chartTab === 'premium' ? (
                <>
                  <span className="flex items-center gap-2">
                    <span className="w-3 h-3 rounded-full bg-purple-500"></span>
                    <span className="text-gray-400">프리미엄 %</span>
                  </span>
                  <span className="flex items-center gap-2">
                    <span className="w-3 h-0.5 bg-green-500"></span>
                    <span className="text-gray-400">진입 기준 (-0.75%)</span>
                  </span>
                  <span className="flex items-center gap-2">
                    <span className="w-3 h-0.5 bg-red-500"></span>
                    <span className="text-gray-400">청산 기준 (+1.0%)</span>
                  </span>
                </>
              ) : (
                <>
                  <span className="flex items-center gap-2">
                    <span className="w-3 h-3 rounded-full bg-yellow-500"></span>
                    <span className="text-gray-400">한국 (빗썸)</span>
                  </span>
                  <span className="flex items-center gap-2">
                    <span className="w-3 h-3 rounded-full bg-blue-500"></span>
                    <span className="text-gray-400">해외 (바이빗 × 환율)</span>
                  </span>
                </>
              )}
            </div>
          </div>
        </div>
      )}

      {/* Filters */}
      <div className="flex items-center gap-4 mb-4">
        <div className="flex bg-gray-800 rounded-lg p-1">
          <button
            onClick={() => setFilter('all')}
            className={`px-4 py-2 rounded-md text-sm transition-colors ${filter === 'all' ? 'bg-gray-700 text-white' : 'text-gray-400 hover:text-white'}`}
          >
            전체 ({premiums.length})
          </button>
          <button
            onClick={() => setFilter('signals')}
            className={`px-4 py-2 rounded-md text-sm transition-colors ${filter === 'signals' ? 'bg-gray-700 text-white' : 'text-gray-400 hover:text-white'}`}
          >
            기회만 ({entrySignals + exitSignals})
          </button>
        </div>
        <div className="flex bg-gray-800 rounded-lg p-1">
          <button
            onClick={() => setSortBy('premium')}
            className={`px-4 py-2 rounded-md text-sm transition-colors ${sortBy === 'premium' ? 'bg-gray-700 text-white' : 'text-gray-400 hover:text-white'}`}
          >
            프리미엄순
          </button>
          <button
            onClick={() => setSortBy('symbol')}
            className={`px-4 py-2 rounded-md text-sm transition-colors ${sortBy === 'symbol' ? 'bg-gray-700 text-white' : 'text-gray-400 hover:text-white'}`}
          >
            심볼순
          </button>
        </div>
        <div className="text-gray-500 text-sm ml-auto">
          행을 클릭하면 차트 보기
        </div>
      </div>

      {/* Premium Table */}
      <div className="bg-gray-800/50 rounded-lg border border-gray-700 overflow-hidden">
        <table className="w-full">
          <thead>
            <tr className="border-b border-gray-700 bg-gray-800">
              <th className="text-left p-4 text-gray-400 font-medium">심볼</th>
              <th className="text-right p-4 text-gray-400 font-medium">한국 (KRW)</th>
              <th className="text-right p-4 text-gray-400 font-medium">해외 (USD)</th>
              <th className="text-right p-4 text-gray-400 font-medium">해외 (KRW)</th>
              <th className="text-right p-4 text-gray-400 font-medium">프리미엄</th>
              <th className="text-center p-4 text-gray-400 font-medium">펀딩피</th>
              <th className="text-center p-4 text-gray-400 font-medium">시그널</th>
            </tr>
          </thead>
          <tbody>
            {filteredPremiums.length === 0 ? (
              <tr>
                <td colSpan={7} className="text-center p-8 text-gray-500">
                  {status.connected ? '데이터 로딩 중...' : '연결 대기 중...'}
                </td>
              </tr>
            ) : (
              filteredPremiums.map((p) => (
                <tr
                  key={p.symbol}
                  onClick={() => setSelectedSymbol(p.symbol)}
                  className={`border-b border-gray-700/50 hover:bg-gray-700/30 transition-colors cursor-pointer ${
                    p.signal === 'ENTRY' ? 'signal-entry' : p.signal === 'EXIT' ? 'signal-exit' : ''
                  }`}
                >
                  <td className="p-4 font-medium text-white">{p.symbol}</td>
                  <td className="p-4 text-right text-gray-300">{formatPrice(p.koreanPrice)}</td>
                  <td className="p-4 text-right text-gray-300">{formatUsdPrice(p.foreignPrice)}</td>
                  <td className="p-4 text-right text-gray-300">{formatPrice(p.foreignPrice * p.usdtRate)}</td>
                  <td className={`p-4 text-right font-bold ${getPremiumClass(p.premium)}`}>
                    {formatPremium(p.premium)}
                  </td>
                  <td className="p-4 text-center">
                    <div className={`font-medium ${getFundingClass(p.fundingRate)}`}>
                      {formatFundingRate(p.fundingRate)}
                    </div>
                    <div className="text-xs text-gray-500">
                      {p.fundingIntervalHours}시간마다
                    </div>
                  </td>
                  <td className="p-4 text-center">
                    {p.signal === 'ENTRY' && (
                      <span className="px-3 py-1 bg-green-500/20 text-green-400 rounded-full text-sm font-medium">
                        역프 진입
                      </span>
                    )}
                    {p.signal === 'EXIT' && (
                      <span className="px-3 py-1 bg-red-500/20 text-red-400 rounded-full text-sm font-medium">
                        김프 청산
                      </span>
                    )}
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>

      {/* Footer */}
      <footer className="mt-8 text-center text-gray-500 text-sm">
        <p>역프 진입: 프리미엄 ≤ -0.75% (한국이 해외보다 쌈) | 김프 청산: 프리미엄 ≥ +1.0% (한국이 해외보다 비쌈)</p>
        <p className="mt-1">펀딩피: <span className="text-green-400">+양수</span> = 숏이 수익 | <span className="text-red-400">−음수</span> = 숏이 지불</p>
        <p className="mt-1">마지막 업데이트: {status.lastUpdate ? new Date(status.lastUpdate).toLocaleTimeString('ko-KR') : '-'}</p>
      </footer>
    </div>
  )
}
