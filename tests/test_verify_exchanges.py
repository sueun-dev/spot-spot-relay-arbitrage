import logging

logger = logging.getLogger(__name__)

"""
Verify all 5 exchanges are properly configured and connected
Tests: Upbit, Bithumb, OKX, Gate.io (Bybit removed)
"""
import asyncio
import os
import sys
from pathlib import Path
from dotenv import load_dotenv

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from src.exchanges.connector import ExchangeConnector
from src.exchanges.upbit import UpbitExchange
from src.exchanges.bithumb import BithumbExchange
from src.exchanges.okx import OKXExchange
from src.exchanges.gate import GateExchange
import yaml

load_dotenv()


async def verify_exchange_apis():
    """Verify all exchange APIs are properly implemented"""
    logger.info("="*80)
    logger.info("🔍 Verifying Exchange API Implementations")
    logger.info("="*80)
    
    # Load config
    with open('config.yaml') as f:
        config = yaml.safe_load(f)
    
    exchanges_to_test = {
        'upbit': {
            'class': UpbitExchange,
            'required_env': ['UPBIT_ACCESS_KEY', 'UPBIT_SECRET_KEY'],
            'type': 'korean'
        },
        'bithumb': {
            'class': BithumbExchange,
            'required_env': ['BITHUMB_API_KEY', 'BITHUMB_SECRET_KEY'],
            'type': 'korean'
        },
        'okx': {
            'class': OKXExchange,
            'required_env': ['OKX_API_KEY', 'OKX_SECRET_KEY', 'OKX_PASSPHRASE'],
            'type': 'global'
        },
        'gate': {
            'class': GateExchange,
            'required_env': ['GATE_API_KEY', 'GATE_SECRET_KEY'],
            'type': 'global'
        }
    }
    
    results = {}
    
    for name, info in exchanges_to_test.items():
        logger.info(f"\n{'='*60}")
        logger.info(f"Testing {name.upper()} ({info['type']} exchange)")
        logger.info(f"{'='*60}")
        
        # Check environment variables
        env_check = all(os.getenv(env) for env in info['required_env'])
        if not env_check:
            logger.error(f"❌ Missing environment variables: {info['required_env']}")
            results[name] = {'status': 'FAIL', 'reason': 'Missing credentials'}
            continue
        
        # Test required methods
        try:
            # Initialize exchange
            if name == 'okx':
                exchange = info['class'](
                    os.getenv('OKX_API_KEY'),
                    os.getenv('OKX_SECRET_KEY'),
                    os.getenv('OKX_PASSPHRASE')
                )
            else:
                exchange = info['class'](
                    os.getenv(info['required_env'][0]),
                    os.getenv(info['required_env'][1])
                )
            
            async with exchange:
                # Test 1: Required methods exist
                required_methods = [
                    'get_base_url', 'get_symbols',
                    'sign_request', 'format_symbol', 'place_order',
                    'get_order', 'get_balance', 'get_ticker', 'get_orderbook'
                ]
                
                for method in required_methods:
                    if not hasattr(exchange, method):
                        logger.error(f"❌ Missing method: {method}")
                        results[name] = {'status': 'FAIL', 'reason': f'Missing {method}'}
                        break
                else:
                    logger.info("✅ All required methods implemented")
                
                # Test 2: API connectivity
                try:
                    balance = await exchange.get_balance()
                    if isinstance(balance, dict):
                        logger.info("✅ API connection successful")
                        results[name] = {'status': 'PASS', 'connected': True}
                    else:
                        logger.warning("⚠️  API returned unexpected format")
                        results[name] = {'status': 'WARN', 'reason': 'Unexpected response'}
                except Exception as e:
                    error_msg = str(e)
                    if "no_authorization_ip" in error_msg:
                        logger.warning("⚠️  IP not whitelisted (Upbit)")
                        results[name] = {'status': 'WARN', 'reason': 'IP whitelist needed'}
                    elif "Invalid Apikey" in error_msg:
                        logger.error("❌ Invalid API credentials")
                        results[name] = {'status': 'FAIL', 'reason': 'Invalid credentials'}
                    else:
                        logger.error(f"❌ Connection error: {error_msg[:100]}")
                        results[name] = {'status': 'FAIL', 'reason': error_msg[:100]}
                
        except Exception as e:
            logger.error(f"❌ Failed to initialize: {e}")
            results[name] = {'status': 'FAIL', 'reason': str(e)[:100]}
    
    # Summary
    logger.info("\n" + "="*80)
    logger.info("📊 VERIFICATION SUMMARY")
    logger.info("="*80)
    
    korean_ok = 0
    global_ok = 0
    
    for name, result in results.items():
        exchange_type = exchanges_to_test[name]['type']
        status_icon = "✅" if result['status'] == 'PASS' else "⚠️" if result['status'] == 'WARN' else "❌"
        
        logger.info(f"{status_icon} {name.upper()}: {result['status']}")
        if 'reason' in result:
            logger.info(f"   → {result['reason']}")
        
        if result['status'] in ['PASS', 'WARN']:
            if exchange_type == 'korean':
                korean_ok += 1
            else:
                global_ok += 1
    
    logger.info(f"\n📍 Korean Exchanges: {korean_ok}/2 working")
    logger.info(f"🌍 Global Exchanges: {global_ok}/2 working")
    
    if korean_ok >= 1 and global_ok >= 1:
        logger.info("\n✅ MINIMUM REQUIREMENTS MET - Can trade!")
        logger.info(f"   {korean_ok} Korean + {global_ok} Global exchanges available")
    else:
        logger.error("\n❌ INSUFFICIENT EXCHANGES")
        logger.error("   Need at least 1 Korean + 1 Global exchange")
    
    return results


async def test_connector_integration():
    """Test ExchangeConnector integration"""
    logger.info("\n" + "="*80)
    logger.info("🔗 Testing ExchangeConnector Integration")
    logger.info("="*80)
    
    with open('config.yaml') as f:
        config = yaml.safe_load(f)
    
    async with ExchangeConnector(config) as connector:
        # Check initialized exchanges
        logger.info(f"\nInitialized exchanges: {connector.get_configured_exchanges()}")
        
        # Test connections
        results = await connector.test_connections()
        
        for exchange, connected in results.items():
            if connected:
                logger.info(f"✅ {exchange}: Connected via connector")
            else:
                logger.error(f"❌ {exchange}: Failed via connector")
        
        return results


async def main():
    """Run all verification tests"""
    # Test individual exchange implementations
    api_results = await verify_exchange_apis()
    
    # Test connector integration
    connector_results = await test_connector_integration()
    
    logger.info("\n" + "="*80)
    logger.info("✅ VERIFICATION COMPLETE")
    logger.info("="*80)
    
    # Final status
    working_exchanges = [ex for ex, res in api_results.items() 
                        if res['status'] in ['PASS', 'WARN']]
    
    if len(working_exchanges) >= 2:
        logger.info(f"\n✅ Bot is ready with {len(working_exchanges)} exchanges:")
        for ex in working_exchanges:
            logger.info(f"   - {ex.upper()}")
        logger.info("\n🚀 You can start trading with: python main.py")
    else:
        logger.error("\n❌ Not enough exchanges configured")
        logger.error("   Fix the issues above before trading")


if __name__ == "__main__":
    asyncio.run(main())