"""Configuration module for Kimchi Premium Arbitrage Bot.

This module defines all configuration parameters for the trading bot,
including strategy settings, exchange configurations, rate limits,
monitoring intervals, and risk management parameters.

Configuration Structure:
    - strategy: Trading strategy parameters
    - exchanges: List of Korean and global exchanges
    - rate_limit: API rate limiting (8 requests/second)
    - monitoring: Update and status intervals
    - risk: Position and exposure limits

Example:
    from config import config
    
    entry_amount = config["strategy"]["entry_amount"]
    korean_exchanges = config["exchanges"]["korean"]
"""

from __future__ import annotations

from typing import Any, Dict, List

# Trading Strategy Configuration
STRATEGY_CONFIG: Dict[str, Any] = {
    "entry_amount": 10000,       # KRW per entry (₩10,000)
    "num_entries": 3,            # Number of split entries
    "entry_threshold": -0.5,     # Entry when premium <= -0.5%
    "exit_threshold": 0.5,       # Exit when premium >= +0.5%
    "stop_loss": -3.0,          # Stop loss at -3.0%
    "min_volume_24h": 50000,    # Minimum 24h volume in USDT
}

# Exchange Configuration
EXCHANGE_CONFIG: Dict[str, List[str]] = {
    "korean": ["upbit", "bithumb"],
    "global": ["okx", "gate"],
}

# API Rate Limiting
RATE_LIMIT_PER_SECOND: int = 8  # All exchanges use 8 requests/second

# Monitoring Configuration
MONITORING_CONFIG: Dict[str, int] = {
    "update_interval": 1,        # Premium update interval (seconds)
    "status_interval": 30,       # Status log interval (seconds)
}

# Risk Management Configuration
RISK_CONFIG: Dict[str, int] = {
    "max_positions": 10,         # Maximum concurrent positions
    "max_exposure": 300000,      # Maximum total KRW exposure
}


def get_config() -> Dict[str, Any]:
    """Get complete configuration dictionary.
    
    Returns:
        Dictionary containing all configuration sections:
            - strategy: Trading strategy settings
            - exchanges: Exchange lists
            - rate_limit: Rate limiting value
            - monitoring: Monitoring intervals
            - risk: Risk management limits
    """
    return {
        "strategy": STRATEGY_CONFIG,
        "exchanges": EXCHANGE_CONFIG,
        "rate_limit": RATE_LIMIT_PER_SECOND,
        "monitoring": MONITORING_CONFIG,
        "risk": RISK_CONFIG,
    }


# Module-level config export for backward compatibility
config: Dict[str, Any] = get_config()