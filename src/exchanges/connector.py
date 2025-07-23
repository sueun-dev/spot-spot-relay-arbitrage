import logging
import os
from typing import Any, Dict, Optional

from ..models import ExchangeBalance, OrderRequest, OrderResponse
from .base import BaseExchange
from .bithumb import BithumbExchange
from .gate import GateExchange
from .okx import OKXExchange
from .upbit import UpbitExchange

logger = logging.getLogger(__name__)

# Constants
GLOBAL_EXCHANGES = ["okx", "gate"]


class ExchangeConnector:
    """Factory class for managing multiple cryptocurrency exchange connections.
    
    This class provides a unified interface for interacting with multiple
    exchanges simultaneously, handling initialization, and routing operations
    to the appropriate exchange implementation.
    
    Attributes:
        exchanges: Dictionary mapping exchange names to initialized instances.
    """
    
    def __init__(self) -> None:
        """Initialize exchange connector.
        
        Automatically detects and initializes exchanges based on
        environment variables.
        """
        self.exchanges: Dict[str, BaseExchange] = {}
        
        self._initialize_korean_exchanges()
        self._initialize_global_exchanges()
        
        logger.info(f"Total exchanges initialized: {len(self.exchanges)}")
    
    def _initialize_korean_exchanges(self) -> None:
        """Initialize Korean exchange connections."""
        self._initialize_upbit()
        self._initialize_bithumb()
    
    def _initialize_global_exchanges(self) -> None:
        """Initialize global exchange connections."""
        self._initialize_okx()
        self._initialize_gate()
    
    def _initialize_upbit(self) -> None:
        """Initialize Upbit exchange connection."""
        api_key = os.getenv('UPBIT_ACCESS_KEY')
        secret_key = os.getenv('UPBIT_SECRET_KEY')
        
        if not api_key or not secret_key:
            logger.warning("Upbit API credentials not found in environment")
            return
        
        try:
            self.exchanges["upbit"] = UpbitExchange(api_key, secret_key)
        except Exception as e:
            logger.error(f"Failed to initialize Upbit: {e}")
    
    def _initialize_bithumb(self) -> None:
        """Initialize Bithumb exchange connection."""
        api_key = os.getenv('BITHUMB_API_KEY')
        secret_key = os.getenv('BITHUMB_SECRET_KEY')
        
        if not api_key or not secret_key:
            logger.warning("Bithumb API credentials not found in environment")
            return
        
        try:
            self.exchanges["bithumb"] = BithumbExchange(api_key, secret_key)
        except Exception as e:
            logger.error(f"Failed to initialize Bithumb: {e}")
    
    def _initialize_okx(self) -> None:
        """Initialize OKX exchange connection."""
        api_key = os.getenv('OKX_API_KEY')
        secret_key = os.getenv('OKX_SECRET_KEY')
        passphrase = os.getenv('OKX_PASSPHRASE')
        
        if not api_key or not secret_key or not passphrase:
            logger.warning("OKX API credentials not found in environment")
            return
        
        try:
            self.exchanges["okx"] = OKXExchange(api_key, secret_key, passphrase)
        except Exception as e:
            logger.error(f"Failed to initialize OKX: {e}")
    
    def _initialize_gate(self) -> None:
        """Initialize Gate.io exchange connection."""
        api_key = os.getenv('GATE_API_KEY')
        secret_key = os.getenv('GATE_SECRET_KEY')
        
        if not api_key or not secret_key:
            logger.warning("Gate.io API credentials not found in environment")
            return
        
        try:
            self.exchanges["gate"] = GateExchange(api_key, secret_key)
        except Exception as e:
            logger.error(f"Failed to initialize Gate.io: {e}")
            
    async def place_order(self, request: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on the specified exchange.
        
        Args:
            request: Order request object containing:
                - exchange: Target exchange name
                - symbol: Trading pair symbol
                - side: Buy or sell
                - size: Order size
                - price: Order price (for limit orders)
                
        Returns:
            Order response object if successful, None if exchange not found
            or order placement failed.
        """
        exchange = self.exchanges.get(request.exchange)
        if not exchange:
            logger.error(f"Exchange '{request.exchange}' not configured")
            return None
            
        return await exchange.place_order(request)
        
    async def get_order(
        self, 
        order_id: str, 
        symbol: str, 
        exchange_name: str
    ) -> Optional[OrderResponse]:
        """Get order status and details from the specified exchange.
        
        Args:
            order_id: Exchange-specific order identifier.
            symbol: Trading pair symbol.
            exchange_name: Name of the exchange.
            
        Returns:
            Order response object if found, None otherwise.
        """
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange '{exchange_name}' not configured")
            return None
            
        return await exchange.get_order(order_id, symbol)
        
    async def get_ticker(
        self, 
        symbol: str, 
        exchange_name: str
    ) -> Optional[Dict[str, Any]]:
        """Get current ticker data for a symbol from the specified exchange.
        
        Args:
            symbol: Trading pair symbol.
            exchange_name: Name of the exchange.
            
        Returns:
            Ticker data dictionary containing:
                - last: Last traded price
                - bid: Best bid price
                - ask: Best ask price
                - volume: 24h volume
            Returns None if exchange not found or request failed.
        """
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange '{exchange_name}' not configured")
            return None
            
        return await exchange.get_ticker(symbol)
        
    async def get_orderbook(
        self, 
        symbol: str, 
        exchange_name: str, 
        limit: int = 10
    ) -> Optional[Dict[str, Any]]:
        """Get order book data for a symbol from the specified exchange.
        
        Args:
            symbol: Trading pair symbol.
            exchange_name: Name of the exchange.
            limit: Number of price levels to return (default: 10).
            
        Returns:
            Order book dictionary containing:
                - bids: List of [price, size] bid levels
                - asks: List of [price, size] ask levels
                - timestamp: Order book timestamp
            Returns None if exchange not found or request failed.
        """
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange '{exchange_name}' not configured")
            return None
            
        return await exchange.get_orderbook(symbol, limit)
        
    async def get_funding_rate(
        self, 
        symbol: str, 
        exchange_name: str
    ) -> Optional[float]:
        """Get funding rate for futures contracts.
        
        Args:
            symbol: Trading pair symbol.
            exchange_name: Name of the exchange.
            
        Returns:
            Funding rate as a float if available, None otherwise.
            Only available for futures exchanges (OKX, Gate.io).
        """
        if exchange_name not in GLOBAL_EXCHANGES:
            return None
            
        exchange = self.exchanges.get(exchange_name)
        if not exchange:
            logger.error(f"Exchange '{exchange_name}' not configured")
            return None
            
        # OKX and Gate both have get_funding_rate method
        rate = await exchange.get_funding_rate(symbol)
        return float(rate) if rate is not None else None
        
    def get_configured_exchanges(self) -> list:
        """Get list of successfully configured exchange names.
        
        Returns:
            List of exchange names that were successfully initialized.
        """
        return list(self.exchanges.keys())
    
    async def close_all(self) -> None:
        """Close all exchange connections."""
        for exchange_name, exchange in self.exchanges.items():
            try:
                await exchange.close()
                logger.info(f"Closed connection to {exchange_name}")
            except Exception as e:
                logger.error(f"Error closing {exchange_name}: {e}")
    
