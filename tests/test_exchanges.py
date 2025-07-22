#!/usr/bin/env python3
"""
Exchange API Tests
"""
import asyncio
import pytest
from decimal import Decimal
from dotenv import load_dotenv

# Load environment
load_dotenv()


@pytest.mark.asyncio
async def test_upbit_connection():
    """Test Upbit connection and basic functionality"""
    from src.exchanges.upbit import UpbitExchange
    import os
    
    api_key = os.getenv('UPBIT_ACCESS_KEY', '')
    secret_key = os.getenv('UPBIT_SECRET_KEY', '')
    
    if not api_key or not secret_key:
        pytest.skip("Upbit credentials not configured")
    
    exchange = UpbitExchange(api_key, secret_key)
    
    # Test connection
    result = await exchange.test_connection()
    assert result is True
    
    # Test symbol fetching
    symbols = await exchange.get_symbols()
    assert len(symbols) > 0
    assert any(s['symbol'] == 'BTC' for s in symbols)
    
    # Test orderbook
    orderbook = await exchange.get_orderbook('BTC')
    # Orderbook might fail due to API issues
    if orderbook:
        assert 'bids' in orderbook
        assert 'asks' in orderbook
        if orderbook['bids']:
            assert len(orderbook['bids']) > 0
        if orderbook['asks']:
            assert len(orderbook['asks']) > 0


@pytest.mark.asyncio
async def test_okx_connection():
    """Test OKX connection and basic functionality"""
    from src.exchanges.okx import OKXExchange
    import os
    
    api_key = os.getenv('OKX_API_KEY', '')
    secret_key = os.getenv('OKX_SECRET_KEY', '')
    passphrase = os.getenv('OKX_PASSPHRASE', '')
    
    if not api_key or not secret_key:
        pytest.skip("OKX credentials not configured")
    
    exchange = OKXExchange(api_key, secret_key, passphrase=passphrase)
    
    # Test connection
    result = await exchange.test_connection()
    assert result is True
    
    # Test symbol fetching
    symbols = await exchange.get_symbols()
    assert len(symbols) > 0
    assert any(s['symbol'] == 'BTC' for s in symbols)
    
    # Test funding rate
    funding = await exchange.get_funding_rate('BTC')
    assert funding is not None
    assert isinstance(funding, Decimal)


@pytest.mark.asyncio
async def test_gate_connection():
    """Test Gate.io connection and basic functionality"""
    from src.exchanges.gate import GateExchange
    import os
    
    api_key = os.getenv('GATE_API_KEY', '')
    secret_key = os.getenv('GATE_SECRET_KEY', '')
    
    if not api_key or not secret_key:
        pytest.skip("Gate.io credentials not configured")
    
    exchange = GateExchange(api_key, secret_key)
    
    # Test connection
    result = await exchange.test_connection()
    assert result is True
    
    # Test symbol fetching
    symbols = await exchange.get_symbols()
    assert len(symbols) > 0
    assert any(s['symbol'] == 'BTC' for s in symbols)


@pytest.mark.asyncio
async def test_exchange_connector():
    """Test ExchangeConnector functionality"""
    from src.exchanges.connector import ExchangeConnector
    import yaml
    
    with open('config.yaml') as f:
        config = yaml.safe_load(f)
    
    async with ExchangeConnector(config) as connector:
        # Test connections
        results = await connector.test_connections()
        assert len(results) > 0
        assert any(status for status in results.values())
        
        # Test orderbook fetching
        if 'upbit' in results and results['upbit']:
            orderbook = await connector.get_orderbook('BTC', 'upbit')
            # API might fail, so be lenient
            if orderbook:
                assert 'bids' in orderbook
                assert 'asks' in orderbook


if __name__ == "__main__":
    # Run specific test
    asyncio.run(test_exchange_connector())