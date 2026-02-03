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
    // Read premium data from the JSON file that C++ bot writes
    const dataPath = path.join(process.cwd(), '..', 'kimp_arb_cpp', 'data', 'premiums.json')

    let premiums: PremiumData[] = []
    let status = {
      connected: false,
      upbitConnected: false,
      bybitConnected: false,
      symbolCount: 0,
      lastUpdate: 0,
    }

    try {
      const fileContent = await fs.readFile(dataPath, 'utf-8')
      const data = JSON.parse(fileContent)
      premiums = data.premiums || []
      status = {
        connected: data.status?.connected || false,
        upbitConnected: data.status?.upbitConnected || false,
        bybitConnected: data.status?.bybitConnected || false,
        symbolCount: premiums.length,
        lastUpdate: data.status?.lastUpdate || Date.now(),
      }
    } catch (e) {
      // File doesn't exist or is invalid - return empty state
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
