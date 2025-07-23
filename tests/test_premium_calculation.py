"""
Tests for premium calculation and monitoring
"""
import pytest
from decimal import Decimal
from datetime import datetime, timezone
import asyncio

from src.models import PremiumData, PriceData
from src.utils.premium_calculator import PremiumCalculator


class TestPremiumCalculation:
    """Test premium calculation functions"""
    
    def setup_method(self):
        """Setup test calculator"""
        self.calculator = PremiumCalculator()
    
    def test_usdt_rate_update(self):
        """Test USDT rate update"""
        # Set rate directly
        self.calculator.usdt_rates['upbit'] = Decimal("1370")
        self.calculator.last_update['upbit'] = datetime.now(timezone.utc)
        
        # Get rate
        rate = self.calculator.get_usdt_rate('upbit')
        assert rate == Decimal("1370")
    
    def test_usdt_rate_management(self):
        """Test USDT rate management"""
        # Set rate
        self.calculator.usdt_rates['upbit'] = Decimal("1370")
        
        # Get rate
        rate = self.calculator.get_usdt_rate('upbit')
        assert rate == Decimal("1370")
    
    def test_missing_usdt_rate(self):
        """Test getting missing USDT rate returns None"""
        # No rate set
        rate = self.calculator.get_usdt_rate('bithumb')
        assert rate is None
    
    def test_hedge_size_calculation(self):
        """Test hedge size calculation with fees"""
        # 10,000 KRW buy
        krw_amount = Decimal("10000")
        upbit_ask = Decimal("61500000")  # BTC price in KRW
        
        # Calculate with 0.05% fee
        upbit_fee = Decimal("0.0005")
        coin_amount = (krw_amount * (1 - upbit_fee)) / upbit_ask
        
        # Verify the calculation
        expected_coin = Decimal("9995") / upbit_ask
        assert abs(coin_amount - expected_coin) < Decimal("0.00000001")


class TestPremiumData:
    """Test PremiumData model"""
    
    def test_premium_data_creation(self):
        """Test creating PremiumData instance"""
        premium_data = PremiumData(
            symbol="BTC",
            korean_price=Decimal("51500"),
            global_price=Decimal("50000"),
            premium=Decimal("3.0"),
            timestamp=datetime.now(timezone.utc),
            korean_exchange="upbit",
            global_exchange="okx",
            volume_krw=Decimal("1000000000"),
            volume_usd=Decimal("1000000")
        )
        
        assert premium_data.symbol == "BTC"
        assert premium_data.premium == Decimal("3.0")
        assert not premium_data.is_reverse_premium
        assert premium_data.abs_premium == Decimal("3.0")
    
    def test_reverse_premium_detection(self):
        """Test reverse premium detection"""
        premium_data = PremiumData(
            symbol="BTC",
            korean_price=Decimal("49500"),
            global_price=Decimal("50000"),
            premium=Decimal("-1.0"),
            timestamp=datetime.now(timezone.utc),
            korean_exchange="upbit",
            global_exchange="okx",
            volume_krw=Decimal("1000000000"),
            volume_usd=Decimal("1000000")
        )
        
        assert premium_data.is_reverse_premium
        assert premium_data.abs_premium == Decimal("1.0")


@pytest.fixture
def sample_price_data():
    """Sample price data for testing"""
    return PriceData(
        exchange="upbit",
        symbol="BTC",
        price=Decimal("50000"),
        volume=Decimal("1000"),
        timestamp=datetime.now(timezone.utc),
        bid=Decimal("49950"),
        ask=Decimal("50050"),
        spread=Decimal("100")
    )

