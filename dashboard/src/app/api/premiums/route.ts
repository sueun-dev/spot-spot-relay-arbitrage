import { NextResponse } from 'next/server'
import { promises as fs } from 'fs'
import path from 'path'

export const dynamic = 'force-dynamic'

interface PremiumData {
  symbol: string
  koreanPrice: number
  foreignPrice: number
  usdtRate: number
  premium: number
  signal: 'ENTRY' | 'EXIT' | null
  timestamp: number
}

export async function GET() {
  try {
    // Resolve premium file path with explicit override + build/legacy fallbacks.
    const candidates = [
      process.env.KIMP_PREMIUMS_PATH,
      path.join(process.cwd(), '..', 'kimp_arb_cpp', 'build', 'data', 'premiums.json'),
      path.join(process.cwd(), '..', 'kimp_arb_cpp', 'data', 'premiums.json'),
    ].filter((value): value is string => typeof value === 'string' && value.length > 0)

    let premiums: PremiumData[] = []
    let status = {
      connected: false,
      upbitConnected: false,
      bybitConnected: false,
      symbolCount: 0,
      lastUpdate: 0,
    }

    let loaded = false
    for (const candidate of candidates) {
      try {
        const fileContent = await fs.readFile(candidate, 'utf-8')
        const data = JSON.parse(fileContent)
        premiums = data.premiums || []
        status = {
          connected: data.status?.connected || false,
          upbitConnected: data.status?.upbitConnected || false,
          bybitConnected: data.status?.bybitConnected || false,
          symbolCount: premiums.length,
          lastUpdate: data.status?.lastUpdate || Date.now(),
        }
        loaded = true
        break
      } catch {
        // Try next path.
      }
    }

    if (!loaded) {
      // File doesn't exist or is invalid - return empty state.
      console.log('Premium data file not found, returning empty state')
    }

    return NextResponse.json({
      premiums,
      status,
    })
  } catch (error) {
    console.error('Error reading premium data:', error)
    return NextResponse.json(
      { error: 'Failed to read premium data' },
      { status: 500 }
    )
  }
}
