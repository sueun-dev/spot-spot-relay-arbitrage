"""
Logging configuration for the arbitrage bot
"""
import sys
from pathlib import Path
from loguru import logger
import yaml

# Load configuration
config_path = Path(__file__).parent.parent.parent / "config.yaml"
with open(config_path) as f:
    config = yaml.safe_load(f)

# Remove default logger
logger.remove()

# Configure logger based on config
log_config = config.get("logging", {})
log_level = log_config.get("level", "INFO")
log_format = "<green>{time:YYYY-MM-DD HH:mm:ss}</green> | <level>{level: <8}</level> | <cyan>{name}</cyan>:<cyan>{function}</cyan>:<cyan>{line}</cyan> - <level>{message}</level>"

# Console output
logger.add(
    sys.stdout,
    format=log_format,
    level=log_level,
    colorize=True,
    backtrace=True,
    diagnose=True
)

# File output for all logs
log_dir = Path("logs")
log_dir.mkdir(exist_ok=True)

logger.add(
    log_dir / "bot_{time}.log",
    format=log_format,
    level=log_level,
    rotation=log_config.get("rotation", "1 day"),
    retention=log_config.get("retention", "30 days"),
    compression="zip",
    backtrace=True,
    diagnose=True
)

# Separate file for errors
logger.add(
    log_dir / "errors_{time}.log",
    format=log_format,
    level="ERROR",
    rotation="1 week",
    retention="3 months",
    compression="zip",
    backtrace=True,
    diagnose=True
)

# Trade logger - separate instance for trade logging
trade_logger = logger.bind(name="trades")
trade_logger.add(
    log_dir / "trades_{time}.log",
    format="{time} | {level} | {message}",
    level="INFO",
    rotation="1 week",
    retention="1 year",
    compression="zip"
)


def log_trade(position_id: str, action: str, details: dict):
    """Log trade actions with structured data"""
    trade_logger.info(
        f"Trade {action}: {position_id}",
        extra={
            "position_id": position_id,
            "action": action,
            **details
        }
    )


def log_premium(symbol: str, premium: float, exchanges: dict):
    """Log premium data for analysis"""
    logger.debug(
        f"Premium {symbol}: {premium:.2f}%",
        extra={
            "symbol": symbol,
            "premium": premium,
            "korean_exchange": exchanges.get("korean"),
            "global_exchange": exchanges.get("global")
        }
    )


def log_risk_alert(alert_type: str, details: dict):
    """Log risk management alerts"""
    logger.warning(
        f"Risk Alert: {alert_type}",
        extra={
            "alert_type": alert_type,
            **details
        }
    )


# Export configured logger
__all__ = ["logger", "trade_logger", "log_trade", "log_premium", "log_risk_alert"]