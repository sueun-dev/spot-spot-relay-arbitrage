"""
Simple USDT rate manager for kimchi premium calculation
"""
import logging
from decimal import Decimal
from typing import Dict, Optional
from datetime import datetime, timezone
logger = logging.getLogger(__name__)


class PremiumCalculator:
    """Manage USDT/KRW exchange rates for premium calculation"""
    
    def __init__(self):
        self.usdt_rates = {}  # exchange -> USDT/KRW rate
        self.last_update = {}
        
    async def update_usdt_rate(self, exchange: str, connector) -> Optional[Decimal]:
        """Update USDT/KRW rate for an exchange"""
        try:
            # Get USDT price in KRW
            ticker = await connector.get_ticker('USDT', exchange)
            if ticker:
                rate = Decimal(str(ticker['last']))
                self.usdt_rates[exchange] = rate
                self.last_update[exchange] = datetime.now(timezone.utc)
                logger.info(f"{exchange} USDT rate: ₩{rate:,.2f}")
                return rate
            else:
                logger.warning(f"Failed to get USDT rate from {exchange}")
                return None
        except Exception as e:
            logger.error(f"Error updating USDT rate for {exchange}: {e}")
            return None
            
    async def update_all_usdt_rates(self, connector, korean_exchanges: list):
        """Update USDT rates for all Korean exchanges"""
        for exchange in korean_exchanges:
            if exchange in connector.get_configured_exchanges():
                await self.update_usdt_rate(exchange, connector)
                
    def get_usdt_rate(self, exchange: str) -> Optional[Decimal]:
        """Get cached USDT rate for an exchange"""
        return self.usdt_rates.get(exchange)