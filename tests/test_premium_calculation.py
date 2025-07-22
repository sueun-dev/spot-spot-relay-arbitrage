"""
Tests for premium calculation and monitoring
"""
import pytest
from decimal import Decimal
from datetime import datetime
import asyncio

from src.models import PremiumData, PriceData
from src.utils.premium_calculator import PremiumCalculator


class TestPremiumCalculation:
    """Test premium calculation functions"""
    
    def setup_method(self):
        """Setup test calculator"""
        self.calculator = PremiumCalculator()
    
    def test_calculate_premium_positive(self):
        """Test positive premium calculation (kimchi premium)"""
        # Set USDT rate
        self.calculator.usdt_rates['upbit'] = Decimal("1000")
        
        korean_price = Decimal("103000")  # KRW
        global_price = Decimal("100")     # USD
        
        premium, details = self.calculator.calculate_premium(
            korean_price, 'upbit', global_price, 'okx'
        )
        assert premium == Decimal("3.0")
    
    def test_calculate_premium_negative(self):
        """Test negative premium calculation (reverse premium)"""
        # Set USDT rate
        self.calculator.usdt_rates['upbit'] = Decimal("1000")
        
        korean_price = Decimal("99000")   # KRW
        global_price = Decimal("100")     # USD
        
        premium, details = self.calculator.calculate_premium(
            korean_price, 'upbit', global_price, 'okx'
        )
        assert premium == Decimal("-1.0")
    
    def test_calculate_premium_zero_global_price(self):
        """Test premium calculation with zero global price"""
        # Set USDT rate
        self.calculator.usdt_rates['upbit'] = Decimal("1000")
        
        korean_price = Decimal("100000")
        global_price = Decimal("0")
        
        premium, details = self.calculator.calculate_premium(
            korean_price, 'upbit', global_price, 'okx'
        )
        assert premium == Decimal("0")
    
    def test_usdt_rate_management(self):
        """Test USDT rate management"""
        # Set rate
        self.calculator.usdt_rates['upbit'] = Decimal("1370")
        
        # Get rate
        rate = self.calculator.get_usdt_rate('upbit')
        assert rate == Decimal("1370")
    
    def test_premium_with_missing_usdt_rate(self):
        """Test premium calculation with missing USDT rate - uses fallback"""
        korean_price = Decimal("136500")  # KRW (100 USD * 1365 fallback rate)
        global_price = Decimal("100")     # USD
        
        # No USDT rate set - should use fallback
        premium, details = self.calculator.calculate_premium(
            korean_price, 'upbit', global_price, 'okx'
        )
        # With fallback rate of 1365, should be 0% premium
        assert premium == Decimal("0")
        assert details['usdt_rate'] == Decimal('1365')  # Fallback rate
    
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
            timestamp=datetime.utcnow(),
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
            timestamp=datetime.utcnow(),
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
        timestamp=datetime.utcnow(),
        bid=Decimal("49950"),
        ask=Decimal("50050"),
        spread=Decimal("100")
    )


def test_price_data_mid_price(sample_price_data):
    """Test PriceData mid price calculation"""
    assert sample_price_data.mid_price == Decimal("50000")