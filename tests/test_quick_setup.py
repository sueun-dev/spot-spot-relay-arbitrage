"""
Quick Setup Test - Consolidated test for initial setup verification
Combines functionality from test_connections, test_simple, and test_step_by_step
"""
import asyncio
import os
import sys
from pathlib import Path
from dotenv import load_dotenv
import yaml

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from src.exchanges.connector import ExchangeConnector
from src.utils.logger import logger

load_dotenv()


async def test_quick_setup():
    """Quick test to verify basic setup"""
    logger.info("="*60)
    logger.info("🚀 Kimchi Premium Bot - Quick Setup Test")
    logger.info("="*60)
    
    # Step 1: Check environment variables
    logger.info("\n1️⃣ Checking Environment Variables...")
    
    env_vars = {
        'Korean Exchanges': {
            'Upbit': ['UPBIT_ACCESS_KEY', 'UPBIT_SECRET_KEY'],
            'Bithumb': ['BITHUMB_API_KEY', 'BITHUMB_SECRET_KEY']
        },
        'Global Exchanges': {
            'OKX': ['OKX_API_KEY', 'OKX_SECRET_KEY', 'OKX_PASSPHRASE'],
            'Gate.io': ['GATE_API_KEY', 'GATE_SECRET_KEY']
        }
    }
    
    configured_exchanges = []
    
    for category, exchanges in env_vars.items():
        logger.info(f"\n{category}:")
        for exchange, vars in exchanges.items():
            all_set = all(os.getenv(var) for var in vars)
            if all_set:
                logger.info(f"  ✅ {exchange}: Configured")
                configured_exchanges.append(exchange.lower().replace('.', ''))
            else:
                logger.warning(f"  ❌ {exchange}: Missing credentials")
    
    # Step 2: Test connections
    logger.info("\n2️⃣ Testing Exchange Connections...")
    
    try:
        with open('config.yaml') as f:
            config = yaml.safe_load(f)
        
        async with ExchangeConnector(config) as connector:
            results = await connector.test_connections()
            
            korean_connected = []
            global_connected = []
            
            for exchange, connected in results.items():
                if connected:
                    logger.info(f"  ✅ {exchange.upper()}: Connected")
                    if exchange in ['upbit', 'bithumb']:
                        korean_connected.append(exchange)
                    else:
                        global_connected.append(exchange)
                else:
                    logger.error(f"  ❌ {exchange.upper()}: Connection Failed")
                    if exchange == 'upbit':
                        logger.info("     → Check IP whitelist at upbit.com")
                    elif exchange == 'bithumb':
                        logger.info("     → Check API key format (32 chars)")
    
    except Exception as e:
        logger.error(f"Error testing connections: {e}")
        return False
    
    # Step 3: Summary
    logger.info("\n"+"="*60)
    logger.info("📊 SETUP SUMMARY")
    logger.info("="*60)
    
    can_trade = len(korean_connected) >= 1 and len(global_connected) >= 1
    
    if can_trade:
        logger.info("\n✅ READY TO TRADE!")
        logger.info(f"   Korean: {', '.join(korean_connected)}")
        logger.info(f"   Global: {', '.join(global_connected)}")
        logger.info("\nStart trading with: python main.py")
        return True
    else:
        logger.error("\n❌ NOT READY TO TRADE")
        if not korean_connected:
            logger.error("   Need at least 1 Korean exchange")
        if not global_connected:
            logger.error("   Need at least 1 Global exchange")
        logger.info("\nFix the issues above and try again")
        return False


async def main():
    """Run the quick setup test"""
    success = await test_quick_setup()
    
    if not success:
        logger.info("\n📖 See docs/API_SETUP_GUIDE.md for help")
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())