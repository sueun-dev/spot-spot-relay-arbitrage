import asyncio
import logging

from dotenv import load_dotenv

from config import config
from src.exchanges.connector import ExchangeConnector
from src.strategies.split_entry_strategy import SplitEntryStrategy

logger = logging.getLogger(__name__)

STATUS_UPDATE_INTERVAL = 30  # 30 seconds
LOG_FORMAT = '%(asctime)s | %(levelname)-8s | %(name)s:%(funcName)s:%(lineno)d - %(message)s'
LOG_DATE_FORMAT = '%Y-%m-%d %H:%M:%S'


def _setup_logging() -> None:
    """Configure logging for the application."""
    logging.basicConfig(
        level=logging.INFO,
        format=LOG_FORMAT,
        datefmt=LOG_DATE_FORMAT
    )

async def _initialize_trading_components(
    config: dict
) -> tuple[ExchangeConnector, SplitEntryStrategy]:
    """Initialize exchange connector and trading strategy.
    
    Args:
        config: Configuration dictionary for trading.
        
    Returns:
        Tuple of (exchange_connector, trading_strategy).
    """
    connector = ExchangeConnector()
    strategy = SplitEntryStrategy(connector, config)
    return connector, strategy

async def _run_status_monitoring(strategy: SplitEntryStrategy) -> None:
    """Run continuous status monitoring loop.
    
    Args:
        strategy: Trading strategy instance to monitor.
        
    Raises:
        KeyboardInterrupt: When user interrupts with Ctrl+C.
    """
    while True:
        await asyncio.sleep(STATUS_UPDATE_INTERVAL)
        _log_trading_status(strategy)

def _log_trading_status(strategy: SplitEntryStrategy) -> None:
    """Log current trading status.
    
    Args:
        strategy: Trading strategy instance.
    """
    summary = strategy.get_positions_summary()
    total_positions = summary.get('total_positions', 0)
    total_krw = summary.get('total_krw', 0)
    
    logger.info(
        f"Trading Status: {total_positions} active positions, "
        f"₩{total_krw:,} total invested"
    )

def main() -> None:
    """Main entry point for the application.
    
    Sets up the environment and runs the trading bot.
    Exits with code 1 on error, 0 on success.
    """
    # Setup environment
    _setup_logging()
    load_dotenv()
    
    # Create event loop and run
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    
    # Initialize components
    connector, strategy = loop.run_until_complete(
        _initialize_trading_components(config)
    )
    
    try:
        loop.run_until_complete(strategy.start())
        loop.run_until_complete(_run_status_monitoring(strategy))
    except KeyboardInterrupt:
        logger.info("\n⚠️ Shutting down...")
    finally:
        loop.run_until_complete(strategy.stop())
        loop.run_until_complete(connector.close_all())
        loop.close()


if __name__ == "__main__":
    main()