from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal
from enum import Enum
from typing import Optional


class OrderSide(Enum):
    BUY = "buy"
    SELL = "sell"


class OrderStatus(Enum):
    PENDING = "pending"
    FILLED = "filled"
    PARTIAL = "partial"
    CANCELLED = "cancelled"


class OrderType(Enum):
    MARKET = "market"
    LIMIT = "limit"


@dataclass
class OrderRequest:
    exchange: str
    symbol: str
    side: OrderSide
    size: Decimal
    order_type: OrderType
    price: Optional[Decimal] = None
    total_krw: Optional[Decimal] = None  # For Upbit market buy orders


@dataclass
class OrderResponse:
    order_id: str
    exchange: str
    symbol: str
    side: OrderSide
    size: Decimal
    executed_size: Decimal
    price: Decimal
    status: OrderStatus
    timestamp: datetime
    fee: Optional[Decimal] = None


@dataclass
class ExchangeBalance:
    currency: str
    total: Decimal
    available: Decimal
    locked: Decimal