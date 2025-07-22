"""
Bithumb Exchange Implementation - Native API
"""
import hmac
import hashlib
import time
import base64
import json
from urllib.parse import urlencode
from decimal import Decimal
from datetime import datetime
from typing import Dict, List, Optional, Any

from .base import BaseExchange
from ..models import OrderRequest, OrderResponse, OrderStatus, OrderSide, ExchangeBalance
from ..utils.logger import logger


class BithumbExchange(BaseExchange):
    """Bithumb exchange implementation using native API"""
    
    def __init__(self, api_key: str, secret_key: str, **kwargs):
        super().__init__(api_key, secret_key, **kwargs)
        self.base_url = "https://api.bithumb.com"
        self.ws_url = "wss://pubwss.bithumb.com/pub/ws"
        
    def get_base_url(self) -> str:
        return self.base_url
        
    def get_ws_url(self) -> str:
        return self.ws_url
        
    async def get_server_time(self) -> int:
        """Get server time"""
        # Bithumb doesn't provide server time endpoint, use local time
        return int(time.time() * 1000)
        
    def sign_request(self, method: str, endpoint: str, params: Dict[str, Any]) -> Dict[str, str]:
        """Sign request using Bithumb's v2.0 API signature method"""
        nonce = str(int(time.time() * 1000))
        
        # For Bithumb v2.0 API
        # Build the string to sign
        if params:
            # Sort parameters by key
            sorted_params = sorted(params.items())
            query_string = urlencode(sorted_params)
        else:
            query_string = ""
            
        # Create signature string according to v2.0 spec
        signature_data = endpoint + chr(0) + query_string + chr(0) + nonce
        
        # Create HMAC-SHA512 signature
        signature = hmac.new(
            self.secret_key.encode('utf-8'),
            signature_data.encode('utf-8'),
            hashlib.sha512
        ).hexdigest()
        
        # Base64 encode the hex signature
        api_sign = base64.b64encode(signature.encode('utf-8')).decode('utf-8')
        
        headers = {
            "api-client-type": "2",
            "api-key": self.api_key,
            "api-sign": api_sign,
            "api-nonce": nonce
        }
        
        # Add form data for POST requests
        if method == "POST":
            # Include parameters in the body for POST
            form_data = params.copy() if params else {}
            form_data['endpoint'] = endpoint
            return {
                "headers": headers,
                "params": form_data
            }
        else:
            return {
                "headers": headers,
                "params": params
            }
        
    def format_symbol(self, base: str, quote: str) -> str:
        """Format symbol for Bithumb (BTC_KRW)"""
        return f"{base}_{quote}"
        
    def _parse_order_status(self, status: str) -> OrderStatus:
        """Parse Bithumb order status"""
        status_map = {
            'pending': OrderStatus.PENDING,
            'completed': OrderStatus.FILLED,
            'cancel': OrderStatus.CANCELLED,
        }
        return status_map.get(status.lower(), OrderStatus.PENDING)
        
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Get all tradable symbols from Bithumb"""
        try:
            response = await self.request("GET", "/public/ticker/ALL_KRW")
            
            if not response or response.get('status') != '0000':
                logger.error(f"Failed to get symbols from Bithumb: {response}")
                return []
                
            symbols = []
            data = response.get('data', {})
            
            for symbol, info in data.items():
                if symbol == 'date':
                    continue
                    
                symbols.append({
                    'symbol': symbol,
                    'base': symbol,
                    'quote': 'KRW',
                    'min_size': Decimal('0.0001'),  # Default values
                    'price_precision': 0,  # KRW is integer
                    'size_precision': 4
                })
                
            return symbols
            
        except Exception as e:
            logger.error(f"Error getting symbols from Bithumb: {e}")
            return []
            
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place an order on Bithumb"""
        try:
            endpoint = "/trade/place"
            
            params = {
                'order_currency': order.symbol,
                'payment_currency': 'KRW',
                'type': 'bid' if order.side == OrderSide.BUY else 'ask',
            }
            
            if order.order_type.value == 'market':
                params['type'] = 'market_' + params['type']
                if order.side == OrderSide.BUY:
                    # For market buy, Bithumb requires units (amount to buy)
                    params['units'] = str(float(order.size))
                else:
                    # For market sell
                    params['units'] = str(float(order.size))
            else:
                # Limit order
                params['units'] = str(float(order.size))
                params['price'] = str(int(float(order.price)))  # Price must be integer KRW
                
            response = await self.request("POST", endpoint, params, signed=True)
            
            if response and response.get('status') == '0000':
                data = response.get('data', {})
                return OrderResponse(
                    order_id=data.get('order_id', ''),
                    exchange="bithumb",
                    symbol=order.symbol,
                    side=order.side,
                    size=order.size,
                    executed_size=Decimal('0'),  # Will be updated when checking order status
                    price=Decimal(params.get('price', '0')),
                    status=OrderStatus.PENDING,
                    timestamp=datetime.now(),
                    fee=Decimal('0')
                )
            else:
                logger.error(f"Order placement failed: {response}")
                return None
                
        except Exception as e:
            logger.error(f"Failed to place order on Bithumb: {e}")
            return None
            
    async def cancel_order(self, order_id: str, symbol: str) -> bool:
        """Cancel an order"""
        try:
            endpoint = "/trade/cancel"
            params = {
                'order_id': order_id,
                'type': 'bid',  # Will be overridden if needed
                'order_currency': symbol,
                'payment_currency': 'KRW'
            }
            
            response = await self.request("POST", endpoint, params, signed=True)
            return response and response.get('status') == '0000'
            
        except Exception as e:
            logger.error(f"Failed to cancel order on Bithumb: {e}")
            return False
            
    async def get_order(self, order_id: str, symbol: str) -> Optional[OrderResponse]:
        """Get order status"""
        try:
            endpoint = "/info/order_detail"
            params = {
                'order_id': order_id,
                'order_currency': symbol,
                'payment_currency': 'KRW'
            }
            
            response = await self.request("POST", endpoint, params, signed=True)
            
            if response and response.get('status') == '0000':
                data = response.get('data', {})
                
                # Determine order side
                order_type = data.get('type', '').lower()
                side = OrderSide.BUY if 'bid' in order_type else OrderSide.SELL
                
                return OrderResponse(
                    order_id=order_id,
                    exchange="bithumb",
                    symbol=symbol,
                    side=side,
                    size=Decimal(data.get('units', '0')),
                    executed_size=Decimal(data.get('units_traded', '0')),
                    price=Decimal(data.get('price', '0')),
                    status=self._parse_order_status(data.get('order_status', '')),
                    timestamp=datetime.fromtimestamp(int(data.get('order_date', 0)) / 1000000),
                    fee=Decimal(data.get('fee', '0'))
                )
                
        except Exception as e:
            logger.error(f"Failed to get order from Bithumb: {e}")
            return None
            
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get account balance"""
        try:
            endpoint = "/info/balance"
            params = {
                'currency': 'ALL'
            }
            
            response = await self.request("POST", endpoint, params, signed=True)
            
            if not response or response.get('status') != '0000':
                logger.error(f"Failed to get balance from Bithumb: {response}")
                return {}
                
            balances = {}
            data = response.get('data', {})
            
            # Bithumb returns balances with currency names as keys
            for key, value in data.items():
                # Skip non-currency fields
                if key in ['total_btc', 'total_krw']:
                    continue
                    
                # Extract currency from key (e.g., 'total_btc' -> 'btc')
                if key.startswith('total_') and isinstance(value, (str, float, int)):
                    currency = key.replace('total_', '').upper()
                    total = Decimal(str(value))
                    
                    # Get available balance
                    available_key = f'available_{currency.lower()}'
                    available = Decimal(str(data.get(available_key, '0')))
                    
                    if total > 0:
                        balances[currency] = ExchangeBalance(
                            currency=currency,
                            total=total,
                            available=available,
                            locked=total - available
                        )
                        
            return balances
            
        except Exception as e:
            logger.error(f"Error getting balance from Bithumb: {e}")
            return {}
            
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get current ticker data"""
        try:
            endpoint = f"/public/ticker/{symbol}_KRW"
            response = await self.request("GET", endpoint)
            
            if response and response.get('status') == '0000':
                data = response.get('data', {})
                return {
                    'symbol': symbol,
                    'last': Decimal(data.get('closing_price', '0')),
                    'bid': Decimal(data.get('buy_price', '0')),
                    'ask': Decimal(data.get('sell_price', '0')),
                    'volume': Decimal(data.get('units_traded_24H', '0')),
                    'timestamp': int(data.get('date', 0))
                }
                
        except Exception as e:
            logger.error(f"Error getting ticker from Bithumb: {e}")
            return None
            
    async def get_orderbook(self, symbol: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        try:
            endpoint = f"/public/orderbook/{symbol}_KRW"
            params = {'count': min(limit, 30)}  # Bithumb max is 30
            
            response = await self.request("GET", endpoint, params)
            
            if response and response.get('status') == '0000':
                data = response.get('data', {})
                
                bids = []
                asks = []
                
                # Parse bids
                for bid in data.get('bids', [])[:limit]:
                    bids.append([
                        Decimal(bid.get('price', '0')),
                        Decimal(bid.get('quantity', '0'))
                    ])
                    
                # Parse asks
                for ask in data.get('asks', [])[:limit]:
                    asks.append([
                        Decimal(ask.get('price', '0')),
                        Decimal(ask.get('quantity', '0'))
                    ])
                    
                return {
                    'symbol': symbol,
                    'bids': bids,
                    'asks': asks,
                    'timestamp': int(data.get('timestamp', 0))
                }
                
        except Exception as e:
            logger.error(f"Error getting orderbook from Bithumb: {e}")
            return None
            
    async def _handle_response(self, response) -> Optional[Dict[str, Any]]:
        """Handle Bithumb-specific response format"""
        try:
            text = await response.text()
            
            if response.status >= 400:
                logger.error(f"Bithumb API error: {response.status} - {text}")
                return None
                
            data = json.loads(text) if text else None
            
            # Check Bithumb-specific error codes
            if data and data.get('status') != '0000' and data.get('status') != '5100':
                logger.error(f"Bithumb error: {data.get('status')} - {data.get('message', 'Unknown error')}")
                return None
                
            return data
            
        except Exception as e:
            logger.error(f"Failed to parse Bithumb response: {e}")
            return None