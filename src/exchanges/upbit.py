"""
Upbit Exchange Implementation - Native API
"""
import jwt
import uuid
import hashlib
import time
import json
from urllib.parse import urlencode
from decimal import Decimal
from datetime import datetime
from typing import Dict, List, Optional, Any

from .base import BaseExchange
from ..models import OrderRequest, OrderResponse, OrderStatus, OrderSide, ExchangeBalance
from ..utils.logger import logger


class UpbitExchange(BaseExchange):
    """Upbit exchange implementation using native API"""
    
    def __init__(self, api_key: str, secret_key: str, **kwargs):
        super().__init__(api_key, secret_key, **kwargs)
        self.base_url = "https://api.upbit.com"
        self.ws_url = "wss://api.upbit.com/websocket/v1"
        
    def get_base_url(self) -> str:
        return self.base_url
        
    def get_ws_url(self) -> str:
        return self.ws_url
        
    async def get_server_time(self) -> int:
        """Get server time"""
        # Upbit doesn't provide server time endpoint, use local time
        return int(time.time() * 1000)
        
    def sign_request(self, method: str, endpoint: str, params: Dict[str, Any]) -> Dict[str, str]:
        """Sign request using JWT for Upbit"""
        payload = {
            'access_key': self.api_key,
            'nonce': str(uuid.uuid4()),
        }
        
        if method == "GET" and params:
            query_string = urlencode(params)
            payload['query'] = query_string
            m = hashlib.sha512()
            m.update(query_string.encode())
            payload['query_hash'] = m.hexdigest()
            payload['query_hash_alg'] = 'SHA512'
        elif method in ["POST", "DELETE"] and params:
            payload['query'] = json.dumps(params)
            m = hashlib.sha512()
            m.update(payload['query'].encode())
            payload['query_hash'] = m.hexdigest()
            payload['query_hash_alg'] = 'SHA512'
            
        jwt_token = jwt.encode(payload, self.secret_key)
        
        return {
            "headers": {
                "Authorization": f"Bearer {jwt_token}"
            }
        }
        
    def format_symbol(self, base: str, quote: str) -> str:
        """Format symbol for Upbit (KRW-BTC)"""
        return f"{quote}-{base}"
        
    def _parse_order_status(self, status: str) -> OrderStatus:
        """Parse Upbit order status"""
        status_map = {
            'wait': OrderStatus.PENDING,
            'done': OrderStatus.FILLED,
            'cancel': OrderStatus.CANCELLED,
        }
        return status_map.get(status, OrderStatus.PENDING)
        
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on Upbit"""
        try:
            symbol = self.format_symbol(order.symbol, "KRW")
            
            params = {
                'market': symbol,
                'side': 'bid' if order.side == OrderSide.BUY else 'ask',
                'ord_type': 'market' if order.order_type.value == 'market' else 'limit',
            }
            
            if order.order_type.value == 'market':
                if order.side == OrderSide.BUY:
                    # For market buy, Upbit requires price (total KRW amount)
                    if order.total_krw:
                        params['price'] = str(int(order.total_krw))  # Must be integer KRW
                    else:
                        # Fallback: fetch current price and calculate
                        ticker = await self.get_ticker(order.symbol)
                        if ticker:
                            estimated_price = ticker['ask'] * 1.01  # Add 1% buffer
                            params['price'] = str(int(float(order.size) * estimated_price))
                        else:
                            raise ValueError(f"Cannot determine price for market buy of {order.symbol}")
                else:
                    # For market sell, use volume
                    params['volume'] = str(float(order.size))
            else:
                params['volume'] = str(float(order.size))
                params['price'] = str(float(order.price))
                
            response = await self.request("POST", "/v1/orders", params, signed=True)
            
            if response:
                return OrderResponse(
                    order_id=response['uuid'],
                    exchange="upbit",
                    symbol=order.symbol,
                    side=order.side,
                    size=order.size,
                    executed_size=Decimal(response.get('executed_volume', '0')),
                    price=Decimal(response.get('price', '0')),
                    status=self._parse_order_status(response['state']),
                    timestamp=datetime.fromisoformat(response['created_at'].replace('Z', '+00:00')),
                    fee=Decimal(response.get('paid_fee', '0'))
                )
                
        except Exception as e:
            logger.error(f"Failed to place order on Upbit: {e}")
            return None
            
    async def cancel_order(self, order_id: str, symbol: str) -> bool:
        """Cancel an order"""
        try:
            params = {'uuid': order_id}
            response = await self.request("DELETE", "/v1/order", params, signed=True)
            return response is not None
        except Exception as e:
            logger.error(f"Failed to cancel order on Upbit: {e}")
            return False
            
    async def get_order(self, order_id: str, symbol: str) -> Optional[OrderResponse]:
        """Get order status"""
        try:
            params = {'uuid': order_id}
            response = await self.request("GET", "/v1/order", params, signed=True)
            
            if response:
                return OrderResponse(
                    order_id=response['uuid'],
                    exchange="upbit",
                    symbol=symbol,
                    side=OrderSide.BUY if response['side'] == 'bid' else OrderSide.SELL,
                    size=Decimal(response['volume']),
                    executed_size=Decimal(response['executed_volume']),
                    price=Decimal(response.get('price', '0')),
                    status=self._parse_order_status(response['state']),
                    timestamp=datetime.fromisoformat(response['created_at'].replace('Z', '+00:00')),
                    fee=Decimal(response.get('paid_fee', '0'))
                )
                
        except Exception as e:
            logger.error(f"Failed to get order from Upbit: {e}")
            return None
            
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get account balance"""
        try:
            response = await self.request("GET", "/v1/accounts", signed=True)
            
            if response:
                balances = {}
                for item in response:
                    currency = item['currency']
                    balances[currency] = ExchangeBalance(
                        exchange="upbit",
                        currency=currency,
                        free=Decimal(item['balance']),
                        used=Decimal(item['locked']),
                        total=Decimal(item['balance']) + Decimal(item['locked'])
                    )
                return balances
                
        except Exception as e:
            logger.error(f"Failed to get balance from Upbit: {e}")
            return {}
            
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get ticker data"""
        try:
            market = self.format_symbol(symbol, "KRW")
            params = {'markets': market}
            response = await self.request("GET", "/v1/ticker", params)
            
            if response and len(response) > 0:
                ticker = response[0]
                return {
                    'symbol': symbol,
                    'last': float(ticker['trade_price']),
                    'bid': float(ticker['trade_price']) * 0.999,  # Approximate
                    'ask': float(ticker['trade_price']) * 1.001,  # Approximate
                    'volume': float(ticker['acc_trade_volume_24h']),
                    'timestamp': ticker['timestamp']
                }
                
        except Exception as e:
            logger.error(f"Failed to get ticker from Upbit: {e}")
            return None
            
    async def get_orderbook(self, symbol: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        try:
            market = self.format_symbol(symbol, "KRW")
            params = {'markets': market}
            response = await self.request("GET", "/v1/orderbook", params)
            
            if response and len(response) > 0:
                orderbook = response[0]
                # Upbit orderbook has bid_price/bid_size and ask_price/ask_size
                bids = []
                asks = []
                
                for unit in orderbook['orderbook_units'][:limit]:
                    if 'bid_price' in unit and 'bid_size' in unit:
                        bids.append([unit['bid_price'], unit['bid_size']])
                    if 'ask_price' in unit and 'ask_size' in unit:
                        asks.append([unit['ask_price'], unit['ask_size']])
                
                return {
                    'bids': bids,
                    'asks': asks,
                    'timestamp': orderbook['timestamp']
                }
            else:
                return None
                
        except Exception as e:
            logger.error(f"Failed to get orderbook from Upbit for {symbol}: {e}")
            return None
            
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Get all KRW trading pairs from Upbit"""
        try:
            endpoint = "/v1/market/all"
            response = await self.request("GET", endpoint, {})
            
            if not response:
                return []
                
            symbols = []
            for market in response:
                # Only get KRW markets
                if market['market'].startswith('KRW-'):
                    base = market['market'].split('-')[1]
                    symbols.append({
                        'symbol': base,
                        'base': base,
                        'quote': 'KRW',
                        'korean_name': market.get('korean_name', ''),
                        'english_name': market.get('english_name', ''),
                        'market_warning': market.get('market_warning', 'NONE'),
                        'min_size': Decimal('0.0001'),  # Default, needs to be fetched separately
                        'price_precision': 0,  # KRW is integer
                        'size_precision': 8
                    })
                    
            logger.info(f"Found {len(symbols)} KRW trading pairs on Upbit")
            return symbols
            
        except Exception as e:
            logger.error(f"Failed to get symbols from Upbit: {e}")
            return []