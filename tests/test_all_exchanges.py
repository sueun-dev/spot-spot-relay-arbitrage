import logging

logger = logging.getLogger(__name__)

"""
Comprehensive Exchange Testing
Tests all exchanges (Upbit, Bithumb, OKX, Gate.io) for proper functionality
"""
import asyncio
import os
import sys
from pathlib import Path
from dotenv import load_dotenv
from datetime import datetime

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent))

from src.exchanges.connector import ExchangeConnector
from src.exchanges.upbit import UpbitExchange
from src.exchanges.bithumb import BithumbExchange
from src.exchanges.okx import OKXExchange
from src.exchanges.gate import GateExchange
import yaml

load_dotenv()


async def test_exchange_detailed(exchange_name: str, exchange):
    """Test a single exchange in detail"""
    logger.info(f"\n{'='*60}")
    logger.info(f"🧪 Testing {exchange_name.upper()}")
    logger.info(f"{'='*60}")
    
    results = {
        'connection': False,
        'balance': False,
        'ticker': False,
        'orderbook': False,
        'symbols': False,
        'funding': False
    }
    
    # Test 1: Balance (tests authentication)
    logger.info(f"\n1️⃣ Testing Balance API...")
    try:
        balances = await exchange.get_balance()
        if isinstance(balances, dict):
            logger.info(f"✅ Balance check successful")
            non_zero = {k: v for k, v in balances.items() if v.total > 0}
            if non_zero:
                for currency, balance in list(non_zero.items())[:3]:  # Show first 3
                    logger.info(f"   {currency}: {balance.total:.8f}")
            else:
                logger.info(f"   No balances (account empty)")
            results['balance'] = True
            results['connection'] = True
        else:
            logger.error(f"❌ Balance check failed")
    except Exception as e:
        error_msg = str(e)
        if "no_authorization_ip" in error_msg:
            logger.error(f"❌ IP not whitelisted (Upbit)")
        elif "Invalid Apikey" in error_msg:
            logger.error(f"❌ Invalid API key")
        elif "5100" in error_msg:  # Bithumb empty response
            logger.info(f"✅ Auth OK (no balance)")
            results['balance'] = True
            results['connection'] = True
        else:
            logger.error(f"❌ Error: {error_msg[:100]}...")
    
    # Test 2: Ticker
    logger.info(f"\n2️⃣ Testing Ticker API...")
    try:
        symbol = "BTC" if exchange_name in ['upbit', 'bithumb'] else "BTC"
        ticker = await exchange.get_ticker(symbol)
        if ticker and ticker.get('last'):
            price = ticker['last']
            if exchange_name in ['upbit', 'bithumb']:
                logger.info(f"✅ BTC Price: ₩{price:,.0f}")
            else:
                logger.info(f"✅ BTC Price: ${price:,.2f}")
            results['ticker'] = True
        else:
            logger.error(f"❌ Ticker failed")
    except Exception as e:
        logger.error(f"❌ Ticker error: {str(e)[:100]}...")
    
    # Test 3: Orderbook
    logger.info(f"\n3️⃣ Testing Orderbook API...")
    try:
        orderbook = await exchange.get_orderbook(symbol, limit=5)
        if orderbook and orderbook.get('bids') and orderbook.get('asks'):
            bid = orderbook['bids'][0][0]
            ask = orderbook['asks'][0][0]
            spread = (ask - bid) / bid * 100
            logger.info(f"✅ Orderbook OK - Spread: {spread:.3f}%")
            results['orderbook'] = True
        else:
            logger.error(f"❌ Orderbook failed")
    except Exception as e:
        logger.error(f"❌ Orderbook error: {str(e)[:100]}...")
    
    # Test 4: Symbols
    logger.info(f"\n4️⃣ Testing Symbol List...")
    try:
        symbols = await exchange.get_symbols()
        if symbols and len(symbols) > 0:
            logger.info(f"✅ Found {len(symbols)} trading pairs")
            results['symbols'] = True
        else:
            logger.error(f"❌ No symbols found")
    except Exception as e:
        logger.error(f"❌ Symbols error: {str(e)[:100]}...")
    
    # Test 5: Funding Rate (futures only)
    if exchange_name in ['okx', 'gate']:
        logger.info(f"\n5️⃣ Testing Funding Rate...")
        try:
            if hasattr(exchange, 'get_funding_rate'):
                rate = await exchange.get_funding_rate("BTC")
                if rate is not None:
                    logger.info(f"✅ BTC Funding Rate: {rate*100:.4f}%")
                    results['funding'] = True
                else:
                    logger.error(f"❌ Funding rate failed")
        except Exception as e:
            logger.error(f"❌ Funding error: {str(e)[:100]}...")
    
    return results


async def main():
    """Run comprehensive tests"""
    logger.info("=" * 80)
    logger.info(f"{'🚀 Kimchi Premium Bot - Exchange Testing':^80}")
    logger.info("=" * 80)
    logger.info(f"Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    
    # Load config
    with open('config.yaml') as f:
        config = yaml.safe_load(f)
    
    # Test each exchange individually
    exchange_results = {}
    
    # Test Korean exchanges
    logger.info("\n📍 KOREAN EXCHANGES (Spot)")
    
    # Upbit
    if os.getenv('UPBIT_ACCESS_KEY') and os.getenv('UPBIT_SECRET_KEY'):
        exchange = UpbitExchange(
            os.getenv('UPBIT_ACCESS_KEY'),
            os.getenv('UPBIT_SECRET_KEY')
        )
        async with exchange:
            results = await test_exchange_detailed('upbit', exchange)
            exchange_results['upbit'] = results
    else:
        logger.warning("⚠️  Upbit credentials not found in .env")
        exchange_results['upbit'] = {'connection': False}
    
    # Bithumb
    if os.getenv('BITHUMB_API_KEY') and os.getenv('BITHUMB_SECRET_KEY'):
        exchange = BithumbExchange(
            os.getenv('BITHUMB_API_KEY'),
            os.getenv('BITHUMB_SECRET_KEY')
        )
        async with exchange:
            results = await test_exchange_detailed('bithumb', exchange)
            exchange_results['bithumb'] = results
    else:
        logger.warning("⚠️  Bithumb credentials not found in .env")
        exchange_results['bithumb'] = {'connection': False}
    
    # Test Global exchanges
    logger.info("\n\n🌍 GLOBAL EXCHANGES (Futures)")
    
    # OKX
    if (os.getenv('OKX_API_KEY') and 
        os.getenv('OKX_SECRET_KEY') and 
        os.getenv('OKX_PASSPHRASE')):
        exchange = OKXExchange(
            os.getenv('OKX_API_KEY'),
            os.getenv('OKX_SECRET_KEY'),
            os.getenv('OKX_PASSPHRASE')
        )
        async with exchange:
            results = await test_exchange_detailed('okx', exchange)
            exchange_results['okx'] = results
    else:
        logger.warning("⚠️  OKX credentials not found in .env")
        exchange_results['okx'] = {'connection': False}
    
    # Gate.io
    if os.getenv('GATE_API_KEY') and os.getenv('GATE_SECRET_KEY'):
        exchange = GateExchange(
            os.getenv('GATE_API_KEY'),
            os.getenv('GATE_SECRET_KEY')
        )
        async with exchange:
            results = await test_exchange_detailed('gate', exchange)
            exchange_results['gate'] = results
    else:
        logger.warning("⚠️  Gate.io credentials not found in .env")
        exchange_results['gate'] = {'connection': False}
    
    # Summary
    logger.info("\n" + "="*80)
    logger.info("📊 TEST SUMMARY")
    logger.info("="*80)
    
    korean_ok = any(r.get('connection', False) for ex, r in exchange_results.items() 
                    if ex in ['upbit', 'bithumb'])
    global_ok = any(r.get('connection', False) for ex, r in exchange_results.items() 
                    if ex in ['okx', 'gate'])
    
    logger.info("\n🏦 Exchange Status:")
    for exchange, results in exchange_results.items():
        if results.get('connection'):
            status = "✅ READY"
            details = []
            if not results.get('balance'):
                details.append("⚠️ Balance")
            if not results.get('ticker'):
                details.append("⚠️ Ticker")
            if not results.get('orderbook'):
                details.append("⚠️ Orderbook")
            
            detail_str = f" ({', '.join(details)})" if details else ""
            logger.info(f"  {exchange.upper()}: {status}{detail_str}")
        else:
            logger.error(f"  {exchange.upper()}: ❌ NOT CONNECTED")
    
    # Trading readiness
    logger.info("\n🎯 Trading Readiness:")
    if korean_ok and global_ok:
        logger.info("✅ READY TO TRADE!")
        logger.info("   - At least 1 Korean exchange connected")
        logger.info("   - At least 1 Global exchange connected")
        logger.info("\n🚀 Run: ./start_trading.sh")
    else:
        logger.error("❌ NOT READY TO TRADE")
        if not korean_ok:
            logger.error("   - Need at least 1 Korean exchange (Upbit or Bithumb)")
        if not global_ok:
            logger.error("   - Need at least 1 Global exchange (OKX or Gate.io)")
        
        logger.info("\n🔧 Common Fixes:")
        if not exchange_results.get('upbit', {}).get('connection'):
            logger.info("  Upbit: Add your IP to whitelist at upbit.com")
        if not exchange_results.get('bithumb', {}).get('connection'):
            logger.info("  Bithumb: Check API key/secret in .env file")
        logger.info("\n📖 See API_SETUP_GUIDE.md for detailed instructions")


if __name__ == "__main__":
    asyncio.run(main())