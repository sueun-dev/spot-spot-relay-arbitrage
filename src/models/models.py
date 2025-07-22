"""
Data models for the Kimchi Premium Arbitrage Bot
"""
from dataclasses import dataclass, field
from datetime import datetime
from decimal import Decimal
from enum import Enum
from typing import Optional, Dict, List


class ExchangeType(Enum):
    KOREAN = "korean"
    GLOBAL = "global"


class OrderSide(Enum):
    BUY = "buy"
    SELL = "sell"


class PositionStatus(Enum):
    OPEN = "open"
    CLOSING = "closing"
    CLOSED = "closed"


class OrderStatus(Enum):
    PENDING = "pending"
    FILLED = "filled"
    PARTIAL = "partial"
    CANCELLED = "cancelled"
    REJECTED = "rejected"


class OrderType(Enum):
    MARKET = "market"
    LIMIT = "limit"


@dataclass
class PriceData:
    exchange: str
    symbol: str
    price: Decimal
    volume: Decimal
    timestamp: datetime
    bid: Decimal
    ask: Decimal
    spread: Decimal
    
    @property
    def mid_price(self) -> Decimal:
        return (self.bid + self.ask) / 2


@dataclass
class PremiumData:
    symbol: str
    korean_price: Decimal
    global_price: Decimal
    premium: Decimal  # Percentage
    timestamp: datetime
    korean_exchange: str
    global_exchange: str
    volume_krw: Decimal
    volume_usd: Decimal
    
    @property
    def is_reverse_premium(self) -> bool:
        return self.premium < 0
    
    @property
    def abs_premium(self) -> Decimal:
        return abs(self.premium)


@dataclass
class FundingRate:
    exchange: str
    symbol: str
    rate: Decimal  # Percentage
    next_funding_time: datetime
    interval_hours: int
    
    @property
    def is_positive(self) -> bool:
        return self.rate > 0
    
    @property
    def hourly_rate(self) -> Decimal:
        return self.rate / self.interval_hours


@dataclass
class SpotPosition:
    exchange: str
    side: str = "long"
    size: Decimal = Decimal("0")
    entry_price: Decimal = Decimal("0")
    current_price: Decimal = Decimal("0")
    unrealized_pnl: Decimal = Decimal("0")


@dataclass
class FuturesPosition:
    exchange: str
    side: str = "short"
    size: Decimal = Decimal("0")
    entry_price: Decimal = Decimal("0")
    current_price: Decimal = Decimal("0")
    unrealized_pnl: Decimal = Decimal("0")
    accumulated_funding: Decimal = Decimal("0")


@dataclass
class Position:
    id: str
    symbol: str
    entry_premium: Decimal
    current_premium: Decimal
    spot_position: SpotPosition
    futures_position: FuturesPosition
    entry_time: datetime
    total_pnl: Decimal = Decimal("0")
    status: PositionStatus = PositionStatus.OPEN
    
    @property
    def hedge_ratio(self) -> Decimal:
        spot_value = self.spot_position.size * self.spot_position.current_price
        futures_value = self.futures_position.size * self.futures_position.current_price
        return spot_value / futures_value if futures_value > 0 else Decimal("0")
    
    @property
    def premium_change(self) -> Decimal:
        return self.current_premium - self.entry_premium
    
    @property
    def holding_time(self) -> float:
        """Returns holding time in hours"""
        return (datetime.utcnow() - self.entry_time).total_seconds() / 3600


@dataclass
class TradeSignal:
    type: str  # "entry" or "exit"
    symbol: str
    premium: Decimal
    reason: str
    confidence: float  # 0-100
    estimated_profit: Decimal
    risk_score: float  # 0-100
    funding_rate: FundingRate
    timestamp: datetime = field(default_factory=datetime.utcnow)
    
    @property
    def should_execute(self) -> bool:
        return self.confidence >= 70 and self.risk_score <= 30


@dataclass
class OrderRequest:
    exchange: str
    symbol: str
    side: OrderSide
    size: Decimal
    order_type: OrderType
    price: Optional[Decimal] = None
    client_order_id: Optional[str] = None
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
    
    @property
    def is_filled(self) -> bool:
        return self.status == OrderStatus.FILLED
    
    @property
    def fill_rate(self) -> Decimal:
        return self.executed_size / self.size if self.size > 0 else Decimal("0")


@dataclass
class ExchangeBalance:
    exchange: str
    currency: str
    free: Decimal
    used: Decimal
    total: Decimal
    
    @property
    def utilization_rate(self) -> Decimal:
        return self.used / self.total if self.total > 0 else Decimal("0")


@dataclass
class RiskMetrics:
    total_exposure: Decimal
    hedge_ratio: Decimal
    funding_cost: Decimal
    liquidity_score: float  # 0-100
    exchange_risk: Dict[str, float]
    timestamp: datetime = field(default_factory=datetime.utcnow)
    
    @property
    def overall_risk_score(self) -> float:
        """Calculate overall risk score (0-100)"""
        # Weight different risk factors
        hedge_risk = abs(self.hedge_ratio - 1) * 100
        exposure_risk = min(self.total_exposure / Decimal("50000") * 50, 50)
        liquidity_risk = 100 - self.liquidity_score
        
        return min((hedge_risk * 0.3 + exposure_risk * 0.4 + liquidity_risk * 0.3), 100)


@dataclass
class MarketCondition:
    symbol: str
    volatility_24h: Decimal
    volume_24h_usd: Decimal
    funding_trend: str  # "increasing", "decreasing", "stable"
    premium_volatility: Decimal
    market_sentiment: str  # "bullish", "bearish", "neutral"
    timestamp: datetime = field(default_factory=datetime.utcnow)


@dataclass
class BacktestResult:
    total_trades: int
    winning_trades: int
    losing_trades: int
    total_pnl: Decimal
    max_drawdown: Decimal
    sharpe_ratio: float
    win_rate: float
    avg_trade_duration: float  # hours
    avg_profit_per_trade: Decimal
    
    @property
    def profit_factor(self) -> float:
        """Ratio of gross profits to gross losses"""
        return float(self.total_pnl / abs(self.max_drawdown)) if self.max_drawdown != 0 else 0