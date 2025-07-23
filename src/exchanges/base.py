import asyncio
import json
import logging
import time
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Optional

import aiohttp

from ..models import ExchangeBalance, OrderRequest, OrderResponse

logger = logging.getLogger(__name__)

DEFAULT_RATE_LIMIT = 8.0  # Requests per second
HTTP_ERROR_THRESHOLD = 400


class BaseExchange(ABC):
    """Abstract base class for cryptocurrency exchange implementations.
    
    This class provides the common interface and functionality that all
    exchange implementations must follow. It handles HTTP requests, rate
    limiting, and response parsing.
    
    Attributes:
        api_key: Exchange API key for authentication.
        secret_key: Exchange secret key for request signing.
        session: Aiohttp client session for making requests.
        last_request_time: Timestamp of last API request for rate limiting.
    """
    
    def __init__(self, api_key: str, secret_key: str) -> None:
        """Initialize the exchange with API credentials.
        
        Args:
            api_key: Exchange API key.
            secret_key: Exchange secret key.
        """
        self.api_key = api_key
        self.secret_key = secret_key
        self.session: Optional[aiohttp.ClientSession] = None
        self.last_request_time: float = 0.0
        
    @abstractmethod
    def get_base_url(self) -> str:
        """Return the base URL for the exchange API.
        
        Returns:
            Base URL string (e.g., "https://api.exchange.com").
        """
        
        
    @abstractmethod
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Fetch all tradable symbols from the exchange.
        
        Returns:
            List of symbol information dictionaries containing:
                - symbol: Trading pair symbol (e.g., "BTC-USDT")
                - base: Base currency (e.g., "BTC")
                - quote: Quote currency (e.g., "USDT")
                - min_size: Minimum order size
                - price_precision: Price decimal places
                - size_precision: Size decimal places
        """
        
        
    @abstractmethod
    def sign_request(
        self, 
        method: str, 
        endpoint: str, 
        params: Dict[str, Any]
    ) -> Dict[str, str]:
        """Sign request according to exchange requirements.
        
        Args:
            method: HTTP method (GET, POST, etc.).
            endpoint: API endpoint path.
            params: Request parameters.
            
        Returns:
            Dictionary containing signature headers and/or modified params.
        """
        
    @abstractmethod
    def format_symbol(self, base: str, quote: str) -> str:
        """Format trading pair according to exchange requirements.
        
        Args:
            base: Base currency (e.g., "BTC").
            quote: Quote currency (e.g., "USDT").
            
        Returns:
            Formatted symbol string (e.g., "BTCUSDT" or "BTC-USDT").
        """
        
    @abstractmethod
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on the exchange.
        
        Args:
            order: Order request object with order details.
            
        Returns:
            Order response object if successful, None otherwise.
        """
        
        
    @abstractmethod
    async def get_order(
        self, 
        order_id: str, 
        symbol: str
    ) -> Optional[OrderResponse]:
        """Get order status and details.
        
        Args:
            order_id: Exchange order ID.
            symbol: Trading symbol.
            
        Returns:
            Order response object if found, None otherwise.
        """
        
    @abstractmethod
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get account balance for all currencies.
        
        Returns:
            Dictionary mapping currency symbols to balance objects.
        """
        
    @abstractmethod
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get current ticker data for a symbol.
        
        Args:
            symbol: Trading symbol.
            
        Returns:
            Ticker data dictionary if successful, None otherwise.
        """
        
    @abstractmethod
    async def get_orderbook(
        self, 
        symbol: str, 
        limit: int = 10
    ) -> Optional[Dict[str, Any]]:
        """Get order book data for a symbol.
        
        Args:
            symbol: Trading symbol.
            limit: Number of price levels to return (default: 10).
            
        Returns:
            Order book data dictionary if successful, None otherwise.
        """
    
    async def request(
        self,
        method: str,
        endpoint: str,
        params: Optional[Dict[str, Any]] = None,
        signed: bool = False
    ) -> Optional[Dict[str, Any]]:
        """Make HTTP request to exchange API.
        
        This method handles rate limiting, request signing, and error handling
        for all API requests.
        
        Args:
            method: HTTP method (GET, POST, DELETE).
            endpoint: API endpoint path.
            params: Request parameters.
            signed: Whether request requires authentication.
            
        Returns:
            Parsed JSON response if successful, None otherwise.
        """
        if not self.session:
            timeout = aiohttp.ClientTimeout(total=30)
            self.session = aiohttp.ClientSession(timeout=timeout)
        
        url = f"{self.get_base_url()}{endpoint}"
        
        try:
            await self._apply_rate_limit()
            
            # Build headers
            headers = {
                "Content-Type": "application/json",
                "Accept": "application/json"
            }
            
            if signed:
                signature_data = self.sign_request(method, endpoint, params or {})
                headers.update(signature_data.get("headers", {}))
            
            # Get appropriate request method
            if method not in ["GET", "POST", "DELETE"]:
                raise ValueError(f"Unsupported HTTP method: {method}")
            
            request_method = getattr(self.session, method.lower())
            kwargs = self._build_request_kwargs(method, params, headers)
            
            async with request_method(url, **kwargs) as response:
                return await self._parse_response(response)
                
        except Exception as e:
            logger.error(f"Unexpected error for {endpoint}: {e}", exc_info=True)
            return None
    
    async def _apply_rate_limit(self) -> None:
        """Apply rate limiting to prevent exceeding API limits.
        
        Enforces a maximum of 8 requests per second by sleeping if necessary.
        """
        current_time = time.time()
        time_since_last = current_time - self.last_request_time
        min_interval = 1.0 / DEFAULT_RATE_LIMIT
        
        if time_since_last < min_interval:
            sleep_time = min_interval - time_since_last
            await asyncio.sleep(sleep_time)
            
        self.last_request_time = time.time()
    
    def _build_request_kwargs(
        self,
        method: str,
        params: Optional[Dict[str, Any]],
        headers: Dict[str, str]
    ) -> Dict[str, Any]:
        """Build keyword arguments for aiohttp request.
        
        Args:
            method: HTTP method.
            params: Request parameters.
            headers: Request headers.
            
        Returns:
            Keyword arguments dictionary.
        """
        kwargs = {"headers": headers}
        
        if method == "GET":
            kwargs["params"] = params
        elif method == "POST":
            kwargs["json"] = params
        elif method == "DELETE":
            kwargs["params"] = params
            
        return kwargs
    
    async def _parse_response(
        self,
        response: aiohttp.ClientResponse
    ) -> Optional[Dict[str, Any]]:
        """Parse HTTP response.
        
        Args:
            response: Aiohttp response object.
            
        Returns:
            Parsed JSON data if successful, None otherwise.
        """
        try:
            text = await response.text()
            
            if response.status >= HTTP_ERROR_THRESHOLD:
                logger.error(f"API error {response.status}: {text[:200]}")
                return None
            
            return json.loads(text) if text else None

        except json.JSONDecodeError as e:
            logger.error(f"Invalid JSON response: {e}")
            return None
        except Exception as e:
            logger.error(f"Failed to parse response: {e}")
            return None
    
    async def close(self) -> None:
        """Close the HTTP session."""
        if self.session:
            await self.session.close()
            self.session = None