"""
Base Exchange Class - Abstract interface for all exchanges
"""
from abc import ABC, abstractmethod
from decimal import Decimal
from typing import Dict, List, Optional, Any, Tuple
from datetime import datetime
import aiohttp
import asyncio
import hmac
import hashlib
import time
import json

from ..models import OrderRequest, OrderResponse, OrderStatus, OrderSide, ExchangeBalance
from ..utils.logger import logger


class BaseExchange(ABC):
    """Abstract base class for all exchange implementations"""
    
    def __init__(self, api_key: str, secret_key: str, **kwargs):
        self.api_key = api_key
        self.secret_key = secret_key
        self.session: Optional[aiohttp.ClientSession] = None
        self.rate_limit_remaining = 100
        self.last_request_time = 0
        
    async def __aenter__(self):
        """Async context manager entry"""
        self.session = aiohttp.ClientSession()
        return self
        
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit"""
        if self.session:
            await self.session.close()
            
    @abstractmethod
    def get_base_url(self) -> str:
        """Get the base URL for the exchange API"""
        pass
        
    @abstractmethod
    def get_ws_url(self) -> str:
        """Get WebSocket URL for the exchange"""
        pass
        
    @abstractmethod
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Get all tradable symbols from the exchange
        
        Returns:
            List of dicts with symbol info including:
            - symbol: Trading pair symbol
            - base: Base currency
            - quote: Quote currency
            - min_size: Minimum order size
            - price_precision: Price decimal places
            - size_precision: Size decimal places
        """
        pass
        
    @abstractmethod
    async def get_server_time(self) -> int:
        """Get server timestamp"""
        pass
        
    @abstractmethod
    def sign_request(self, method: str, endpoint: str, params: Dict[str, Any]) -> Dict[str, str]:
        """Sign request according to exchange requirements"""
        pass
        
    @abstractmethod
    def format_symbol(self, base: str, quote: str) -> str:
        """Format trading pair according to exchange requirements"""
        pass
        
    @abstractmethod
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on the exchange"""
        pass
        
    @abstractmethod
    async def cancel_order(self, order_id: str, symbol: str) -> bool:
        """Cancel an order"""
        pass
        
    @abstractmethod
    async def get_order(self, order_id: str, symbol: str) -> Optional[OrderResponse]:
        """Get order status"""
        pass
        
    @abstractmethod
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get account balance"""
        pass
        
    @abstractmethod
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get current ticker data"""
        pass
        
    @abstractmethod
    async def get_orderbook(self, symbol: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        pass
    
    async def test_connection(self) -> bool:
        """Test if the exchange connection is working"""
        try:
            # Try to get server time or balance as a connection test
            await self.get_server_time()
            return True
        except Exception as e:
            logger.error(f"Connection test failed for {self.__class__.__name__}: {e}")
            return False
        
    async def request(
        self,
        method: str,
        endpoint: str,
        params: Optional[Dict[str, Any]] = None,
        signed: bool = False,
        retry_count: int = 3
    ) -> Optional[Dict[str, Any]]:
        """Make HTTP request to exchange API with retry logic"""
        if not self.session:
            self.session = aiohttp.ClientSession()
            
        url = f"{self.get_base_url()}{endpoint}"
        
        for attempt in range(retry_count):
            try:
                # Rate limiting
                await self._check_rate_limit()
                
                # Prepare request
                headers = self._get_headers()
                
                if signed:
                    signature_data = self.sign_request(method, endpoint, params or {})
                    headers.update(signature_data.get("headers", {}))
                    if "params" in signature_data:
                        params = signature_data["params"]
                        
                if method == "GET":
                    async with self.session.get(url, params=params, headers=headers) as response:
                        result = await self._handle_response(response)
                        
                        # If rate limited, wait and retry
                        if response.status == 429 and attempt < retry_count - 1:
                            wait_time = min(2 ** attempt, 60)  # Exponential backoff, max 60s
                            logger.warning(f"Rate limited, waiting {wait_time}s before retry...")
                            await asyncio.sleep(wait_time)
                            continue
                            
                        return result
                        
                elif method == "POST":
                    # Check if this is Bithumb and needs form data
                    if 'bithumb' in self.__class__.__name__.lower():
                        async with self.session.post(url, data=params, headers=headers) as response:
                            result = await self._handle_response(response)
                            
                            # If rate limited, wait and retry
                            if response.status == 429 and attempt < retry_count - 1:
                                wait_time = min(2 ** attempt, 60)
                                logger.warning(f"Rate limited, waiting {wait_time}s before retry...")
                                await asyncio.sleep(wait_time)
                                continue
                                
                            return result
                    else:
                        async with self.session.post(url, json=params, headers=headers) as response:
                            result = await self._handle_response(response)
                            
                            # If rate limited, wait and retry
                            if response.status == 429 and attempt < retry_count - 1:
                                wait_time = min(2 ** attempt, 60)
                                logger.warning(f"Rate limited, waiting {wait_time}s before retry...")
                                await asyncio.sleep(wait_time)
                                continue
                                
                            return result
                        
                elif method == "DELETE":
                    async with self.session.delete(url, params=params, headers=headers) as response:
                        return await self._handle_response(response)
                        
            except aiohttp.ClientError as e:
                if attempt < retry_count - 1:
                    wait_time = min(2 ** attempt, 30)
                    logger.warning(f"Request error: {e}, retrying in {wait_time}s...")
                    await asyncio.sleep(wait_time)
                    continue
                else:
                    logger.error(f"Request failed after {retry_count} attempts: {e}")
                    return None
            except Exception as e:
                logger.error(f"Unexpected error: {e}")
                return None
                
        return None
            
    async def _handle_response(self, response: aiohttp.ClientResponse) -> Optional[Dict[str, Any]]:
        """Handle API response"""
        try:
            text = await response.text()
            
            if response.status >= 400:
                logger.error(f"API error: {response.status} - {text}")
                return None
            
            # Try to parse JSON
            try:
                data = json.loads(text) if text else None
            except json.JSONDecodeError:
                logger.error(f"Invalid JSON response: {text[:200]}")
                return None
                
            return data
        except Exception as e:
            logger.error(f"Failed to parse response: {e}")
            return None
            
    async def _check_rate_limit(self):
        """Enhanced rate limiting with per-exchange limits"""
        current_time = time.time()
        time_since_last = current_time - self.last_request_time
        
        # Exchange-specific rate limits (requests per second)
        rate_limits = {
            'upbit': 1.5,     # ~90 requests per minute
            'bithumb': 1.3,   # ~80 requests per minute  
            'okx': 5.0,       # 300 requests per minute
            'gate': 3.3,      # 200 requests per minute
        }
        
        # Get exchange name from class name
        exchange_name = self.__class__.__name__.replace('Exchange', '').lower()
        requests_per_second = rate_limits.get(exchange_name, 2.0)  # Default: 2 req/sec
        min_interval = 1.0 / requests_per_second
        
        # Wait if necessary
        if time_since_last < min_interval:
            await asyncio.sleep(min_interval - time_since_last)
            
        self.last_request_time = time.time()
        
    def _get_headers(self) -> Dict[str, str]:
        """Get common headers"""
        return {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "KimchiPremiumBot/1.0"
        }
        
    def _generate_nonce(self) -> str:
        """Generate nonce for request signing"""
        return str(int(time.time() * 1000))
        
    def _hmac_sha256(self, secret: str, message: str) -> str:
        """Generate HMAC SHA256 signature"""
        return hmac.new(
            secret.encode('utf-8'),
            message.encode('utf-8'),
            hashlib.sha256
        ).hexdigest()
        
    def _hmac_sha512(self, secret: str, message: str) -> str:
        """Generate HMAC SHA512 signature"""
        return hmac.new(
            secret.encode('utf-8'),
            message.encode('utf-8'),
            hashlib.sha512
        ).hexdigest()