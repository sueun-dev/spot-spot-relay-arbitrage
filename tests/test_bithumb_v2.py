import logging

logger = logging.getLogger(__name__)

"""
Test Bithumb v2.0 API Implementation
Tests authentication, balance check, and basic API functionality
"""
import asyncio
import os
import sys
from pathlib import Path
from dotenv import load_dotenv
import json

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent))

from src.exchanges.bithumb import BithumbExchange

load_dotenv()


async def test_bithumb_v2():
    """Test Bithumb v2.0 API implementation"""
    
    # Check environment variables
    api_key = os.getenv('BITHUMB_API_KEY')
    secret_key = os.getenv('BITHUMB_SECRET_KEY')
    
    if not api_key or not secret_key:
        logger.error("❌ BITHUMB_API_KEY and BITHUMB_SECRET_KEY must be set in .env file")
        return False
        
    logger.info("=" * 60)
    logger.info("🧪 Testing Bithumb v2.0 API Implementation")
    logger.info("=" * 60)
    
    try:
        # Initialize exchange
        exchange = BithumbExchange(api_key, secret_key)
        
        async with exchange:
            # Test 1: Public API - Get ticker
            logger.info("\n1️⃣ Testing Public API (Ticker)...")
            try:
                ticker = await exchange.get_ticker("BTC")
                if ticker:
                    logger.info(f"✅ Ticker Success: BTC = ₩{ticker['last']:,.0f}")
                    logger.info(f"   Bid: ₩{ticker['bid']:,.0f} | Ask: ₩{ticker['ask']:,.0f}")
                else:
                    logger.error("❌ Ticker failed - no data returned")
            except Exception as e:
                logger.error(f"❌ Ticker Error: {e}")
            
            # Test 2: Public API - Get orderbook
            logger.info("\n2️⃣ Testing Public API (Orderbook)...")
            try:
                orderbook = await exchange.get_orderbook("BTC", limit=5)
                if orderbook and orderbook.get('bids'):
                    best_bid = orderbook['bids'][0]
                    best_ask = orderbook['asks'][0]
                    logger.info(f"✅ Orderbook Success:")
                    logger.info(f"   Best Bid: ₩{best_bid[0]:,.0f} ({best_bid[1]:.4f} BTC)")
                    logger.info(f"   Best Ask: ₩{best_ask[0]:,.0f} ({best_ask[1]:.4f} BTC)")
                else:
                    logger.error("❌ Orderbook failed - no data returned")
            except Exception as e:
                logger.error(f"❌ Orderbook Error: {e}")
            
            # Test 3: Private API - Get balance
            logger.info("\n3️⃣ Testing Private API (Balance)...")
            try:
                balances = await exchange.get_balance()
                
                if isinstance(balances, dict):
                    logger.info("✅ Balance API Response Received")
                    
                    # Show all balances greater than 0
                    has_balance = False
                    for currency, balance in balances.items():
                        if balance.total > 0:
                            has_balance = True
                            logger.info(f"   {currency}: {balance.total:,.8f} "
                                      f"(Available: {balance.available:,.8f})")
                    
                    if not has_balance:
                        logger.info("   No balances found (this is OK if account is empty)")
                        logger.info("   ✅ Authentication successful!")
                else:
                    logger.error("❌ Balance failed - unexpected response format")
                    
            except Exception as e:
                error_msg = str(e)
                logger.error(f"❌ Balance Error: {error_msg}")
                
                # Provide specific guidance based on error
                if "Invalid Apikey" in error_msg:
                    logger.error("\n🔧 Fix: Check your Bithumb API credentials:")
                    logger.error("   1. Login to bithumb.com")
                    logger.error("   2. Go to 마이페이지 → API 관리")
                    logger.error("   3. Make sure API is enabled")
                    logger.error("   4. Copy the exact API key and secret")
                    logger.error("   5. Update .env file (no quotes, no spaces)")
                elif "5100" in error_msg:
                    logger.info("   Note: Error 5100 means 'no data' - this is OK for empty balance")
                    logger.info("   ✅ Authentication likely successful!")
            
            # Test 4: Get symbols
            logger.info("\n4️⃣ Testing Symbol List...")
            try:
                symbols = await exchange.get_symbols()
                if symbols:
                    logger.info(f"✅ Found {len(symbols)} trading pairs")
                    # Show first 5 symbols
                    sample = symbols[:5]
                    logger.info(f"   Sample: {', '.join([s['symbol'] for s in sample])}...")
                else:
                    logger.error("❌ No symbols returned")
            except Exception as e:
                logger.error(f"❌ Symbols Error: {e}")
            
            # Summary
            logger.info("\n" + "=" * 60)
            logger.info("📊 Test Summary:")
            logger.info("=" * 60)
            
            # Determine overall status
            if ticker and orderbook:
                if isinstance(balances, dict) or "5100" in str(e):
                    logger.info("✅ Bithumb v2.0 API is working correctly!")
                    logger.info("✅ Ready for trading!")
                    return True
                else:
                    logger.error("❌ Private API authentication failed")
                    logger.error("   Check your API credentials in .env file")
                    return False
            else:
                logger.error("❌ Public API not working")
                logger.error("   Check your internet connection")
                return False
                
    except Exception as e:
        logger.error(f"❌ Unexpected error: {e}")
        return False


async def main():
    """Run the test"""
    success = await test_bithumb_v2()
    
    if success:
        logger.info("\n✅ You can now run: ./start_trading.sh")
    else:
        logger.info("\n❌ Fix the issues above before trading")
        logger.info("📖 See API_SETUP_GUIDE.md for help")


if __name__ == "__main__":
    asyncio.run(main())