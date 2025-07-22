"""
Unified Exchange Connector - Factory for all exchange implementations
"""
import os
from typing import Dict, Optional, Any
import asyncio

from .base import BaseExchange
from .upbit import UpbitExchange
from .bithumb import BithumbExchange
from .okx import OKXExchange
from .gate import GateExchange
from ..models import OrderRequest, OrderResponse, ExchangeBalance
from ..utils.logger import logger


class ExchangeConnector:
    """Unified interface for all exchanges"""
    
    def __init__(self, config: dict):
        self.config = config
        self.exchanges: Dict[str, BaseExchange] = {}
        self._initialize_exchanges()
        
    def _initialize_exchanges(self):
        """Initialize all configured exchanges"""
        
        # Korean exchanges
        for exchange_config in self.config["exchanges"]["korean"]:
            if not exchange_config["enabled"]:
                continue
                
            name = exchange_config["name"]
            try:
                if name == "upbit":
                    api_key = os.getenv('UPBIT_ACCESS_KEY')
                    secret_key = os.getenv('UPBIT_SECRET_KEY')
                    if api_key and secret_key:
                        self.exchanges[name] = UpbitExchange(api_key, secret_key)
                        logger.info(f"Initialized Upbit exchange")
                        
                elif name == "bithumb":
                    api_key = os.getenv('BITHUMB_API_KEY')
                    secret_key = os.getenv('BITHUMB_SECRET_KEY')
                    if api_key and secret_key:
                        self.exchanges[name] = BithumbExchange(api_key, secret_key)
                        logger.info(f"Initialized Bithumb exchange")
                        
            except Exception as e:
                logger.error(f"Failed to initialize {name}: {e}")
                
        # Global futures exchanges
        for exchange_config in self.config["exchanges"]["global"]:
            if not exchange_config["enabled"]:
                continue
                
            name = exchange_config["name"]
            try:
                if name == "okx":
                    api_key = os.getenv('OKX_API_KEY')
                    secret_key = os.getenv('OKX_SECRET_KEY')
                    passphrase = os.getenv('OKX_PASSPHRASE')
                    if api_key and secret_key and passphrase:
                        self.exchanges[name] = OKXExchange(api_key, secret_key, passphrase)
                        logger.info(f"Initialized OKX exchange")
                        
                # Bybit removed - not used in current implementation
                        
                elif name == "gate":
                    api_key = os.getenv('GATE_API_KEY')
                    secret_key = os.getenv('GATE_SECRET_KEY')
                    if api_key and secret_key:
                        self.exchanges[name] = GateExchange(api_key, secret_key)
                        logger.info(f"Initialized Gate.io exchange")
                        
            except Exception as e:
                logger.error(f"Failed to initialize {name}: {e}")
                
        logger.info(f"Total exchanges initialized: {len(self.exchanges)}")
        
    async def __aenter__(self):
        """Async context manager entry"""
        for exchange in self.exchanges.values():
            await exchange.__aenter__()
        return self
        
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit"""
        for exchange in self.exchanges.values():
            await exchange.__aexit__(exc_type, exc_val, exc_tb)
            
    async def place_order(self, request: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on specified exchange"""
        exchange = self.exchanges.get(request.exchange)
        if not exchange:
            logger.error(f"Exchange {request.exchange} not configured")
            return None
            
        return await exchange.place_order(request)
        
    async def cancel_order(self, order_id: str, symbol: str, exchange_name: str) -> bool:
        """Cancel an order"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return False
            
        return await exchange.cancel_order(order_id, symbol)
        
    async def get_order(self, order_id: str, symbol: str, exchange_name: str) -> Optional[OrderResponse]:
        """Get order status"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return None
            
        return await exchange.get_order(order_id, symbol)
        
    async def fetch_balance(self, exchange_name: str) -> Dict[str, ExchangeBalance]:
        """Get account balance from exchange"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return {}
            
        return await exchange.get_balance()
        
    async def get_ticker(self, symbol: str, exchange_name: str) -> Optional[Dict[str, Any]]:
        """Get ticker data"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return None
            
        return await exchange.get_ticker(symbol)
        
    async def get_orderbook(self, symbol: str, exchange_name: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return None
            
        return await exchange.get_orderbook(symbol, limit)
        
    async def get_funding_rate(self, symbol: str, exchange_name: str) -> Optional[float]:
        """Get funding rate (futures only)"""
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange {exchange_name} not configured")
            return None
            
        # Only futures exchanges have funding rates
        if exchange_name not in ["okx", "gate"]:
            return None
            
        if hasattr(exchange, 'get_funding_rate'):
            rate = await exchange.get_funding_rate(symbol)
            return float(rate) if rate else None
            
        return None
        
    async def get_funding_fee(self, symbol: str, exchange_name: str, position_size: float) -> Optional[float]:
        """Calculate funding fee for a position"""
        funding_rate = await self.get_funding_rate(symbol, exchange_name)
        if funding_rate is None:
            return None
            
        # Funding fee = position size * funding rate
        return position_size * funding_rate
        
    def is_futures_exchange(self, exchange_name: str) -> bool:
        """Check if exchange is for futures trading"""
        return exchange_name in ["okx", "gate"]
        
    def get_configured_exchanges(self) -> list:
        """Get list of configured exchanges"""
        return list(self.exchanges.keys())
        
    async def test_connections(self) -> Dict[str, bool]:
        """Test all exchange connections"""
        results = {}
        
        for name, exchange in self.exchanges.items():
            try:
                # Try to get balance as a connection test
                balance = await exchange.get_balance()
                # Consider connection successful if we got a response (even empty balance)
                # For Bithumb, check if we got a dict response (even if empty)
                if name == 'bithumb':
                    results[name] = isinstance(balance, dict)
                else:
                    results[name] = balance is not None
                logger.info(f"{name} connection: {'OK' if results[name] else 'Failed'}")
            except Exception as e:
                logger.error(f"{name} connection test failed: {e}")
                results[name] = False
                
        return results