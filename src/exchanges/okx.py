"""
OKX Exchange Implementation - Native API for Perpetual Futures
"""
import hmac
import base64
import time
import json
from datetime import datetime, timezone
from decimal import Decimal
from typing import Dict, List, Optional, Any

import logging
from .base import BaseExchange
from ..models import OrderRequest, OrderResponse, OrderStatus, OrderSide, ExchangeBalance
logger = logging.getLogger(__name__)


class OKXExchange(BaseExchange):
    """OKX exchange implementation using native API"""
    
    def __init__(self, api_key: str, secret_key: str, passphrase: str):
        super().__init__(api_key, secret_key)
        self.passphrase = passphrase
        self.base_url = "https://www.okx.com"
        # For perpetual contracts
        self.inst_type = "SWAP"
        
    def get_base_url(self) -> str:
        return self.base_url
        
    def sign_request(self, method: str, endpoint: str, params: Dict[str, Any]) -> Dict[str, str]:
        """Sign request for OKX API"""
        timestamp = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'
        
        # Create signature string
        if method == "GET" and params:
            query_string = '&'.join([f"{k}={v}" for k, v in sorted(params.items())])
            message = f"{timestamp}{method}{endpoint}?{query_string}"
        elif method in ["POST", "DELETE"] and params:
            body = json.dumps(params)
            message = f"{timestamp}{method}{endpoint}{body}"
        else:
            message = f"{timestamp}{method}{endpoint}"
            
        # Create signature
        mac = hmac.new(
            bytes(self.secret_key, encoding='utf8'),
            bytes(message, encoding='utf-8'),
            digestmod='sha256'
        )
        signature = base64.b64encode(mac.digest()).decode()
        
        return {
            "headers": {
                "OK-ACCESS-KEY": self.api_key,
                "OK-ACCESS-SIGN": signature,
                "OK-ACCESS-TIMESTAMP": timestamp,
                "OK-ACCESS-PASSPHRASE": self.passphrase,
                "Content-Type": "application/json"
            }
        }
        
    def format_symbol(self, base: str, quote: str) -> str:
        """Format symbol for OKX perpetual (BTC-USDT-SWAP)"""
        return f"{base}-{quote}-SWAP"
        
    def _parse_order_status(self, status: str) -> OrderStatus:
        """Parse OKX order status"""
        status_map = {
            'live': OrderStatus.PENDING,
            'partially_filled': OrderStatus.PARTIAL,
            'filled': OrderStatus.FILLED,
            'canceled': OrderStatus.CANCELLED,
            'cancelled': OrderStatus.CANCELLED,
        }
        return status_map.get(status, OrderStatus.PENDING)
        
    async def place_order(self, order: OrderRequest) -> Optional[OrderResponse]:
        """Place a futures order on OKX"""
        try:
            endpoint = "/api/v5/trade/order"
            
            params = {
                'instId': self.format_symbol(order.symbol, "USDT"),
                'tdMode': 'cross',  # Cross margin mode
                'side': 'buy' if order.side == OrderSide.BUY else 'sell',
                'posSide': 'short',  # Always short for arbitrage
                'ordType': 'market' if order.order_type.value == 'market' else 'limit',
                'sz': str(float(order.size))
            }
            
            if order.order_type.value == 'limit' and order.price:
                params['px'] = str(float(order.price))
                
            response = await self.request("POST", endpoint, params, signed=True)
            
            if response and response.get('code') == '0':
                data = response['data'][0]
                return OrderResponse(
                    order_id=data['ordId'],
                    exchange="okx",
                    symbol=order.symbol,
                    side=order.side,
                    size=order.size,
                    executed_size=Decimal('0'),  # Need separate call for fill info
                    price=Decimal(data.get('px', '0')),
                    status=OrderStatus.PENDING,
                    timestamp=datetime.now(timezone.utc),
                    fee=Decimal('0')
                )
                
        except Exception as e:
            logger.error(f"Failed to place order on OKX: {e}")
            return None
            
    async def get_order(self, order_id: str, symbol: str) -> Optional[OrderResponse]:
        """Get order status"""
        try:
            endpoint = "/api/v5/trade/order"
            params = {
                'instId': self.format_symbol(symbol, "USDT"),
                'ordId': order_id
            }
            
            response = await self.request("GET", endpoint, params, signed=True)
            
            if response and response.get('code') == '0' and response['data']:
                data = response['data'][0]
                
                return OrderResponse(
                    order_id=data['ordId'],
                    exchange="okx",
                    symbol=symbol,
                    side=OrderSide.BUY if data['side'] == 'buy' else OrderSide.SELL,
                    size=Decimal(data['sz']),
                    executed_size=Decimal(data['fillSz']),
                    price=Decimal(data.get('avgPx', data.get('px', '0'))),
                    status=self._parse_order_status(data['state']),
                    timestamp=datetime.fromtimestamp(int(data['cTime']) / 1000),
                    fee=Decimal(data.get('fee', '0'))
                )
                
        except Exception as e:
            logger.error(f"Failed to get order from OKX: {e}")
            return None
            
    async def get_balance(self) -> Dict[str, ExchangeBalance]:
        """Get account balance"""
        try:
            endpoint = "/api/v5/account/balance"
            response = await self.request("GET", endpoint, signed=True)
            
            if response and response.get('code') == '0':
                balances = {}
                
                for account in response['data']:
                    # Check if account has details
                    if 'details' in account:
                        for detail in account['details']:
                            currency = detail['ccy']
                            
                            # For futures, we care about USDT balance
                            if currency == 'USDT':
                                balances[currency] = ExchangeBalance(
                                    currency=currency,
                                    total=Decimal(detail.get('bal', '0')),
                                    available=Decimal(detail.get('availBal', '0')),
                                    locked=Decimal(detail.get('frozenBal', '0'))
                                )
                    # Handle different response format
                    elif 'bal' in account:
                        currency = account.get('ccy', 'USDT')
                        if currency == 'USDT':
                            balances[currency] = ExchangeBalance(
                                currency=currency,
                                total=Decimal(account.get('bal', '0')),
                                available=Decimal(account.get('availBal', '0')),
                                locked=Decimal(account.get('frozenBal', '0'))
                            )
                            
                return balances
                
        except Exception as e:
            logger.error(f"Failed to get balance from OKX: {e}")
            return {}
            
    async def get_ticker(self, symbol: str) -> Optional[Dict[str, Any]]:
        """Get ticker data for perpetual contract"""
        try:
            endpoint = "/api/v5/market/ticker"
            params = {'instId': self.format_symbol(symbol, "USDT")}
            
            response = await self.request("GET", endpoint, params)
            
            if response and response.get('code') == '0' and response['data']:
                data = response['data'][0]
                return {
                    'symbol': symbol,
                    'last': float(data['last']),
                    'bid': float(data['bidPx']),
                    'ask': float(data['askPx']),
                    'volume': float(data['vol24h']),
                    'timestamp': int(data['ts'])
                }
                
        except Exception as e:
            logger.error(f"Failed to get ticker from OKX: {e}")
            return None
            
    async def get_orderbook(self, symbol: str, limit: int = 10) -> Optional[Dict[str, Any]]:
        """Get order book"""
        try:
            endpoint = "/api/v5/market/books"
            params = {
                'instId': self.format_symbol(symbol, "USDT"),
                'sz': str(limit)
            }
            
            response = await self.request("GET", endpoint, params)
            
            if response and response.get('code') == '0' and response['data']:
                data = response['data'][0]
                return {
                    'bids': [[float(bid[0]), float(bid[1])] for bid in data['bids']],
                    'asks': [[float(ask[0]), float(ask[1])] for ask in data['asks']],
                    'timestamp': int(data['ts'])
                }
                
        except Exception as e:
            logger.error(f"Failed to get orderbook from OKX: {e}")
            return None
            
    async def get_funding_rate(self, symbol: str) -> Optional[Decimal]:
        """Get current funding rate"""
        try:
            endpoint = "/api/v5/public/funding-rate"
            params = {'instId': self.format_symbol(symbol, "USDT")}
            
            response = await self.request("GET", endpoint, params)
            
            if response and response.get('code') == '0' and response['data']:
                return Decimal(response['data'][0]['fundingRate'])
                
        except Exception as e:
            logger.error(f"Failed to get funding rate from OKX: {e}")
            return None
            
    async def get_symbols(self) -> List[Dict[str, Any]]:
        """Get all USDT perpetual contracts from OKX"""
        try:
            endpoint = "/api/v5/public/instruments"
            params = {
                'instType': 'SWAP'  # Perpetual contracts
            }
            
            response = await self.request("GET", endpoint, params)
            
            if not response or response.get('code') != '0':
                return []
                
            symbols = []
            for inst in response['data']:
                if inst['instId'].endswith('-USDT-SWAP'):
                    base = inst['instId'].split('-')[0]
                    symbols.append({
                        'symbol': base,
                        'base': base,
                        'quote': 'USDT',
                        'contract_type': 'perpetual',
                        'contract_value': Decimal(inst['ctVal']),
                        'min_size': Decimal(inst['minSz']),
                        'tick_size': Decimal(inst['tickSz']),
                        'lot_size': Decimal(inst['lotSz']),
                        'price_precision': len(inst['tickSz'].split('.')[-1]) if '.' in inst['tickSz'] else 0,
                        'size_precision': len(inst['lotSz'].split('.')[-1]) if '.' in inst['lotSz'] else 0,
                        'status': inst['state']
                    })
                    
            logger.info(f"Found {len(symbols)} USDT perpetual contracts on OKX")
            return symbols
            
        except Exception as e:
            logger.error(f"Failed to get symbols from OKX: {e}")
            return []