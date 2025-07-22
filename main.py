"""
Kimchi Premium Arbitrage Bot - Main Entry Point
김치 프리미엄 차익거래 봇
"""
import asyncio
import argparse
import sys
import os
from pathlib import Path
from datetime import datetime
from dotenv import load_dotenv
import yaml

from src.exchanges.connector import ExchangeConnector
from src.strategies.split_entry_strategy import SplitEntryStrategy
from src.utils.logger import logger

load_dotenv()

sys.path.insert(0, str(Path(__file__).parent))


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Kimchi Premium Arbitrage Bot - Auto Trading"
    )
    
    parser.add_argument(
        "--config",
        type=str,
        default="config.yaml",
        help="Path to configuration file (default: config.yaml)"
    )
    
    return parser.parse_args()


async def run_trade(args):
    """Run trading mode with split entry strategy"""
    # Load configuration
    with open(args.config) as f:
        config = yaml.safe_load(f)
    
    logger.info("="*100)
    logger.info(f"{'🚀 Kimchi Premium Arbitrage Bot - Trading Mode 🚀':^100}")
    logger.info("="*100)
    logger.info(f"Started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    logger.info("Strategy: Split Entry (₩10,000 x 3)")
    logger.info("Entry: ≤ -1.0% | Exit: ≥ +0.1%")
    logger.info("-"*100)
    
    async with ExchangeConnector(config) as connector:
        # Test connections
        logger.info("\n⚡ Testing connections...")
        results = await connector.test_connections()
        connected = {k: v for k, v in results.items() if v}
        
        if len(connected) < 2:
            logger.error("❌ Need at least one Korean and one global exchange!")
            return
        
        logger.info(f"✅ Connected: {list(connected.keys())}")
        
        # Create and start strategy
        strategy = SplitEntryStrategy(connector, config)
        
        logger.info("\n🎯 Starting split entry strategy...")
        await strategy.start()
        
        try:
            # Keep running until interrupted
            while True:
                await asyncio.sleep(30)
                
                # Print status
                summary = strategy.get_positions_summary()
                logger.info(f"\n📊 Status Update - {datetime.now().strftime('%H:%M:%S')}")
                logger.info(f"   Positions: {summary['total_positions']}")
                logger.info(f"   Invested: ₩{summary['total_krw']:,}")
                
                if summary['positions']:
                    for symbol, pos in summary['positions'].items():
                        logger.info(f"   {symbol}: {pos['count']}x ₩10,000 "
                              f"(avg: {pos['avg_premium']:.2f}%) [{pos['exchange']}]")
                
        except KeyboardInterrupt:
            logger.info("\n\n🛑 Stopping strategy...")
        finally:
            await strategy.stop()
            logger.info("✅ Strategy stopped safely")


def main():
    args = parse_arguments()

    try:
        # Start trading
        logger.info("🚀 Starting Kimchi Premium Trading Bot...")
        logger.info("⚠️  REAL MONEY TRADING - Press Ctrl+C to stop")
        asyncio.run(run_trade(args))
    
    except Exception as e:
        logger.error(f"\n❌ Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()