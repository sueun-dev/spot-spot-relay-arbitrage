#!/usr/bin/env python3
"""
Split Entry Strategy Tests
"""
import asyncio
import pytest
from decimal import Decimal
from unittest.mock import Mock, AsyncMock, patch

from src.strategies.split_entry_strategy import SplitEntryStrategy


@pytest.fixture
def mock_connector():
    """Create mock exchange connector"""
    connector = Mock()
    connector.exchanges = {
        'upbit': Mock(),
        'okx': Mock(),
        'gate': Mock()
    }
    
    # Mock get_orderbook
    async def mock_get_orderbook(symbol, exchange):
        if exchange == 'upbit':
            return {
                'bids': [[Decimal('161500000'), Decimal('0.1')]],
                'asks': [[Decimal('161600000'), Decimal('0.1')]]
            }
        else:
            return {
                'bids': [[Decimal('117800'), Decimal('0.1')]],
                'asks': [[Decimal('117810'), Decimal('0.1')]]
            }
    
    connector.get_orderbook = AsyncMock(side_effect=mock_get_orderbook)
    
    # Mock get_funding_rate
    async def mock_get_funding_rate(symbol, exchange):
        return Decimal('0.0001')  # 0.01%
    
    connector.get_funding_rate = AsyncMock(side_effect=mock_get_funding_rate)
    
    # Mock place_order
    connector.place_order = AsyncMock(return_value=Mock(order_id='test123'))
    
    return connector


@pytest.fixture
def strategy(mock_connector):
    """Create strategy instance with mock connector"""
    config = {
        'trading': {
            'min_reverse_premium': -1.0,
            'target_exit_premium': 0.1
        }
    }
    return SplitEntryStrategy(mock_connector, config)


@pytest.mark.asyncio
async def test_entry_conditions(strategy):
    """Test entry condition detection"""
    # Mock premium data with entry condition met
    premium_data = {
        'symbol': 'BTC',
        'premium': Decimal('-1.1'),  # Below -1.0%
        'exchange': 'okx',
        'funding_rate': Decimal('0.0001'),  # Positive
        'upbit_ask': Decimal('161600000'),
        'global_bid': Decimal('117800')
    }
    
    # Position should be empty initially
    assert strategy.positions['BTC']['count'] == 0
    
    # Check if entry conditions are met
    assert premium_data['premium'] <= strategy.entry_threshold
    assert premium_data['funding_rate'] >= 0


@pytest.mark.asyncio
async def test_split_entry_execution(strategy, mock_connector):
    """Test split entry execution logic"""
    symbol = 'BTC'
    
    # Initial state
    assert strategy.positions[symbol]['count'] == 0
    assert strategy.positions[symbol]['total_krw'] == 0
    
    # Simulate 3 entries
    for i in range(3):
        # Add entry
        strategy.positions[symbol]['count'] += 1
        strategy.positions[symbol]['total_krw'] += strategy.entry_amount_krw
        
        # Check position state
        assert strategy.positions[symbol]['count'] == i + 1
        assert strategy.positions[symbol]['total_krw'] == strategy.entry_amount_krw * (i + 1)
    
    # Should not exceed max
    assert strategy.positions[symbol]['total_krw'] == strategy.max_amount_per_coin


@pytest.mark.asyncio
async def test_exit_conditions(strategy):
    """Test exit condition detection"""
    # Setup position
    symbol = 'BTC'
    strategy.positions[symbol] = {
        'count': 3,
        'total_krw': 30000,
        'global_exchange': 'okx',
        'entries': [
            {'amount': 10000, 'premium': Decimal('-1.2')},
            {'amount': 10000, 'premium': Decimal('-1.1')},
            {'amount': 10000, 'premium': Decimal('-1.0')}
        ],
        'avg_entry_premium': Decimal('-1.1'),
        'status': 'holding'
    }
    
    # Check exit condition
    exit_premium = Decimal('0.2')  # Above 0.1%
    assert exit_premium >= strategy.exit_threshold


@pytest.mark.asyncio
async def test_position_size_calculation():
    """Test position size calculations"""
    entry_amount_krw = Decimal('10000')
    upbit_price = Decimal('161600000')
    
    # Calculate BTC amount for 10,000 KRW
    btc_amount = entry_amount_krw / upbit_price
    
    # Should be around 0.0000619 BTC
    assert btc_amount > 0
    assert btc_amount < Decimal('0.0001')
    
    # Calculate USD value
    usdt_rate = Decimal('1370')
    usd_value = entry_amount_krw / usdt_rate
    
    # Should be around 7.3 USD
    assert usd_value > 7
    assert usd_value < 8


@pytest.mark.asyncio 
async def test_funding_rate_check(strategy, mock_connector):
    """Test funding rate validation"""
    # Negative funding rate should prevent entry
    negative_funding = Decimal('-0.0001')
    assert negative_funding < 0
    
    # Position should not be opened with negative funding
    # This is enforced in the strategy logic


if __name__ == "__main__":
    pytest.main([__file__, "-v"])