"""
Premium Calculator with Real-time USDT rates
"""
from decimal import Decimal
from typing import Dict, Optional, Tuple
from datetime import datetime
from ..utils.logger import logger


class PremiumCalculator:
    """Calculate kimchi premium using real-time USDT rates"""
    
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
                self.last_update[exchange] = datetime.utcnow()
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
        
    def calculate_premium(self, korean_price: Decimal, korean_exchange: str,
                         global_price: Decimal, global_exchange: str) -> Tuple[Optional[Decimal], Dict]:
        """
        Calculate premium using real-time USDT rates
        
        Returns:
            (premium_percentage, details_dict)
        """
        # Get USDT rate for Korean exchange
        usdt_rate = self.usdt_rates.get(korean_exchange)
        if not usdt_rate:
            logger.warning(f"No USDT rate for {korean_exchange}, using fallback 1365")
            usdt_rate = Decimal('1365')  # Fallback
            
        # Convert Korean price to USD using USDT rate
        korean_price_usd = korean_price / usdt_rate
        
        # Calculate premium
        if global_price > 0:
            premium = ((korean_price_usd - global_price) / global_price) * 100
        else:
            premium = Decimal('0')
            
        details = {
            'korean_price_krw': korean_price,
            'korean_price_usd': korean_price_usd,
            'global_price_usd': global_price,
            'usdt_rate': usdt_rate,
            'premium': premium,
            'korean_exchange': korean_exchange,
            'global_exchange': global_exchange
        }
        
        return premium, details
        
    def calculate_arbitrage_profit(self, premium: Decimal, position_size_usd: Decimal,
                                  entry_premium: Decimal = Decimal('-1.0'),
                                  exit_premium: Decimal = Decimal('2.0')) -> Dict:
        """
        Calculate potential arbitrage profit
        
        Args:
            premium: Current premium percentage
            position_size_usd: Position size in USD
            entry_premium: Entry threshold (default -1%)
            exit_premium: Exit threshold (default 2%)
            
        Returns:
            Dict with profit calculations
        """
        if premium > entry_premium:
            # Not an entry opportunity
            return {
                'is_opportunity': False,
                'current_premium': premium,
                'reason': 'Premium too high for entry'
            }
            
        # Calculate profit potential
        # Profit = (Exit Premium - Entry Premium) * Position Size
        premium_spread = exit_premium - premium  # Note: premium is negative
        gross_profit = position_size_usd * premium_spread / 100
        
        # Estimate fees (0.1% maker + 0.1% taker) * 2 (entry + exit)
        total_fees = position_size_usd * Decimal('0.004')  # 0.4% total
        
        net_profit = gross_profit - total_fees
        
        return {
            'is_opportunity': True,
            'current_premium': premium,
            'entry_premium': premium,
            'exit_premium': exit_premium,
            'premium_spread': premium_spread,
            'position_size': position_size_usd,
            'gross_profit': gross_profit,
            'total_fees': total_fees,
            'net_profit': net_profit,
            'roi_percentage': (net_profit / position_size_usd) * 100
        }