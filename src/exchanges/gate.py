"""
Gate.io Exchange Implementation - Native API for Perpetual Futures
"""
import hmac
import hashlib
import time
import json
from datetime import datetime
from decimal import Decimal
from typing import Dict, List, Optional, Any
from urllib.parse import urlencode

from .base import BaseExchange
from ..models import OrderRequest, OrderResponse, OrderStatus, OrderSide, ExchangeBalance
from ..utils.logger import logger


class GateExchange(BaseExchange):
    """Gate.io exchange implementation using native API"""
    
    def __init__(self, api_key: str, secret_key: str, **kwargs):
        super().__init__(api_key, secret_key, **kwargs)
        self.base_url = "https://api.gateio.ws"
        self.ws_url = "wss://fx-ws.gateio.ws/v4/ws/usdt"
        
    def get_base_url(self) -> str:
        return self.base_url
        
    def get_ws_url(self) -> str:
        return self.ws_url
        
    async def get_server_time(self) -> int:
        """Get server time"""
        response = await self.request("GET", "/api/v4/spot/time")
        if response and 'server_time' in response:
            return int(response['server_time']) * 1000
        return int(time.time() * 1000)
        
    def sign_request(self, method: str, endpoint: str, params: Dict[str, Any]) -> Dict[str, str]:
        """Sign request for Gate.io API"""
        timestamp = str(int(time.time()))
        
        # Create signature string
        query_string = ""
        body_string = ""
        
        if method == "GET" and params:
            query_string = urlencode(sorted(params.items()))
            
        if method in ["POST", "PATCH", "PUT", "DELETE"]:
            if params:
                body_string = json.dumps(params)
                body_hash = hashlib.sha512(body_string.encode('utf-8')).hexdigest()
            else:
                body_hash = hashlib.sha512(b'').hexdigest()
        else:
            body_hash = hashlib.sha512(b'').hexdigest()
            
        # Build signing string
        signing_string = f"{method}\n{endpoint}\n{query_string}\n{body_hash}\n{timestamp}"
        
        # Create signature
        signature = hmac.new(
            self.secret_key.encode('utf-8'),
            signing_string.encode('utf-8'),
            hashlib.sha512
        ).hexdigest()
        
        return {
            "headers": {
                "KEY": self.api_key,
                "SIGN": signature,
                "Timestamp": timestamp,
                "Content-Type": "application/json"
            }
        }
        
    def format_symbol(self, base: str, quote: str) -> str:
        """Format symbol for Gate.io futures (BTC_USDT)"""
        return f"{base}_{quote}"
        
    def _parse_order_status(self, status: str) -> OrderStatus:
        """Parse Gate.io order status"""
        status_map = {
            'open': OrderStatus.PENDING,
            'closed': OrderStatus.FILLED,
            'cancelled': OrderStatus.CANCELLED,
        }
        return status_map.get(status, OrderStatus.PENDING)
        
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place a futures order on Gate.io"""
        try:
            endpoint = "/api/v4/futures/usdt/orders"
            
            # Calculate contract size (Gate.io uses contracts, not USD)
            # This is simplified - actual implementation would need contract specifications
            contract_size = int(float(order.size) / 100)  # Assuming $100 per contract
            
            params = {
                'contract': self.format_symbol(order.symbol, "USDT"),
                'size': -contract_size if order.side == OrderSide.SELL else contract_size,  # Negative for short
                'price': '0' if order.order_type.value == 'market' else str(float(order.price)),
                'tif': 'ioc' if order.order_type.value == 'market' else 'gtc',
                'reduce_only': False,
                'auto_size': 'close_short' if order.side == OrderSide.BUY else 'close_long'
            }
            
            response = await self.request("POST", endpoint, params, signed=True)
            
            if response and 'id' in response:
                return OrderResponse(
                    order_id=str(response['id']),
                    exchange="gate",
                    symbol=order.symbol,
                    side=order.side,
                    size=order.size,
                    executed_size=Decimal('0'),
                    price=Decimal(response.get('price', '0')),
                    status=self._parse_order_status(response.get('status', 'open')),
                    timestamp=datetime.fromtimestamp(response.get('create_time', time.time())),
                    fee=Decimal('0')
                )
                
        except Exception as e:
            logger.error(f"Failed to place order on Gate.io: {e}")
            return None
            
    async def cancel_order(self, order_id: str, symbol: str) -> bool:
        """Cancel an order"""
        try:
            endpoint = f"/api/v4/futures/usdt/orders/{order_id}"
            params = {'contract': self.format_symbol(symbol, "USDT")}
            
            response = await self.request("DELETE", endpoint, params, signed=True)
            return response is not None
            
        except Exception as e:
            logger.error(f"Failed to cancel order on Gate.io: {e}")
            return False
            
    async def get_order(self, order_id: str, symbol: str) -> Optional[OrderResponse]:
        """Get order status"""
        try:
            endpoint = f"/api/v4/futures/usdt/orders/{order_id}"
            params = {'contract': self.format_symbol(symbol, "USDT")}
            
            response = await self.request("GET", endpoint, params, signed=True)
            
            if response:
                # Determine side from size (negative = short/sell)
                size = float(response['size'])
                side = OrderSide.SELL if size < 0 else OrderSide.BUY
                
                return OrderResponse(
                    order_id=str(response['id']),
                    exchange="gate",
                    symbol=symbol,
                    side=side,
                    size=Decimal(str(abs(size * 100))),  # Convert back to USD
                    executed_size=Decimal(str(abs(size - response.get('left', 0)) * 100)),
                    price=Decimal(response.get('fill_price', response.get('price', '0'))),
                    status=self._parse_order_status(response['status']),
                    timestamp=datetime.fromtimestamp(response['create_time']),
                    fee=Decimal(response.get('fee', '0'))
                )
                
        except Exception as e:
            logger.error(f"Failed to get order from Gate.io: {e}")
            return None
            
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get futures account balance"""
        try:
            endpoint = "/api/v4/futures/usdt/accounts"
            
            response = await self.request("GET", endpoint, signed=True)
            
            if response:
                balances = {}
                
                # Gate.io returns single account info for USDT futures
                balances['USDT'] = ExchangeBalance(
                    exchange="gate",
                    currency='USDT',
                    free=Decimal(response.get('available', '0')),
                    used=Decimal(response.get('margin', '0')),
                    total=Decimal(response.get('total', '0'))
                )
                
                return balances
                
        except Exception as e:
            logger.error(f"Failed to get balance from Gate.io: {e}")
            return {}
            
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get ticker data"""
        try:
            endpoint = "/api/v4/futures/usdt/tickers"
            params = {'contract': self.format_symbol(symbol, "USDT")}
            
            response = await self.request("GET", endpoint, params)
            
            if response and len(response) > 0:
                data = response[0]
                return {
                    'symbol': symbol,
                    'last': float(data['last']),
                    'bid': float(data.get('highest_bid', data['last'])),
                    'ask': float(data.get('lowest_ask', data['last'])),
                    'volume': float(data['volume_24h']),
                    'timestamp': int(time.time() * 1000)
                }
                
        except Exception as e:
            logger.error(f"Failed to get ticker from Gate.io: {e}")
            return None
            
    async def get_orderbook(self, symbol: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        try:
            endpoint = "/api/v4/futures/usdt/order_book"
            params = {
                'contract': self.format_symbol(symbol, "USDT"),
                'limit': str(limit)
            }
            
            response = await self.request("GET", endpoint, params)
            
            if response:
                return {
                    'bids': [[float(bid['p']), float(bid['s'])] for bid in response.get('bids', [])],
                    'asks': [[float(ask['p']), float(ask['s'])] for ask in response.get('asks', [])],
                    'timestamp': int(response.get('current', time.time() * 1000))
                }
                
        except Exception as e:
            logger.error(f"Failed to get orderbook from Gate.io: {e}")
            return None
            
    async def get_funding_rate(self, symbol: str) -> Optional[Decimal]:
        """Get current funding rate"""
        try:
            endpoint = "/api/v4/futures/usdt/contracts/" + self.format_symbol(symbol, "USDT")
            
            response = await self.request("GET", endpoint)
            
            if response and isinstance(response, dict):
                # Gate.io returns funding rate as a string percentage
                funding_rate = response.get('funding_rate', '0')
                if funding_rate:
                    return Decimal(str(funding_rate))
            
            return Decimal('0')
                
        except Exception as e:
            logger.error(f"Failed to get funding rate from Gate.io: {e}")
            return None
            
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Get all USDT perpetual contracts from Gate.io"""
        try:
            endpoint = "/api/v4/futures/usdt/contracts"
            response = await self.request("GET", endpoint, {})
            
            if not response:
                return []
                
            symbols = []
            for contract in response:
                if contract['name'].endswith('_USDT'):
                    base = contract['name'].split('_')[0]
                    symbols.append({
                        'symbol': base,
                        'base': base,
                        'quote': 'USDT',
                        'contract_type': 'perpetual',
                        'contract_size': Decimal(contract.get('quanto_multiplier', '1')),
                        'min_size': Decimal(str(contract.get('order_size_min', 1))),
                        'mark_price': Decimal(contract.get('mark_price', '0')),
                        'funding_rate': Decimal(contract.get('funding_rate', '0')),
                        'price_precision': contract.get('mark_price_round', '0.0001'),
                        'size_precision': contract.get('order_size_round', '1'),  # Use default if None
                        'status': 'trading' if not contract.get('in_delisting') else 'delisting'
                    })
                    
            logger.info(f"Found {len(symbols)} USDT perpetual contracts on Gate.io")
            return symbols
            
        except Exception as e:
            logger.error(f"Failed to get symbols from Gate.io: {e}")
            return []