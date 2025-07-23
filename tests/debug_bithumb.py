import logging

logger = logging.getLogger(__name__)

"""
Bithumb API Debug Script
Detailed debugging for Bithumb v2.0 API connection issues
"""
import asyncio
import os
import sys
from pathlib import Path
from dotenv import load_dotenv
import hmac
import hashlib
import time
import base64
import json
import aiohttp
from urllib.parse import urlencode

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent))


load_dotenv()


async def test_bithumb_raw():
    """Test Bithumb API with raw requests to debug issues"""
    
    api_key = os.getenv('BITHUMB_API_KEY')
    secret_key = os.getenv('BITHUMB_SECRET_KEY')
    
    if not api_key or not secret_key:
        logger.error("❌ BITHUMB_API_KEY and BITHUMB_SECRET_KEY must be set in .env file")
        return
        
    logger.info("=" * 80)
    logger.info("🔍 Bithumb API Debug")
    logger.info("=" * 80)
    
    # Show API key info (masked)
    logger.info(f"\n📋 API Credentials Check:")
    logger.info(f"   API Key: {api_key[:8]}...{api_key[-4:]} (length: {len(api_key)})")
    logger.info(f"   Secret Key: {'*' * 8}...{'*' * 4} (length: {len(secret_key)})")
    
    # Test 1: Public API (no auth needed)
    logger.info("\n1️⃣ Testing Public API (No Auth Required)...")
    async with aiohttp.ClientSession() as session:
        try:
            url = "https://api.bithumb.com/public/ticker/BTC_KRW"
            async with session.get(url) as response:
                text = await response.text()
                logger.info(f"   Status: {response.status}")
                
                if response.status == 200:
                    data = json.loads(text)
                    if data.get('status') == '0000':
                        price = data['data']['closing_price']
                        logger.info(f"   ✅ Public API OK - BTC: ₩{int(price):,}")
                    else:
                        logger.error(f"   ❌ Error: {data}")
                else:
                    logger.error(f"   ❌ HTTP {response.status}: {text}")
                    
        except Exception as e:
            logger.error(f"   ❌ Public API Error: {e}")
    
    # Test 2: Private API with detailed debugging
    logger.info("\n2️⃣ Testing Private API (Balance)...")
    
    endpoint = "/info/balance"
    params = {"currency": "ALL"}
    
    # Create signature
    nonce = str(int(time.time() * 1000))
    
    # Build query string
    query_string = urlencode(sorted(params.items()))
    
    # Create signature data
    signature_data = endpoint + chr(0) + query_string + chr(0) + nonce
    
    logger.info(f"\n📝 Signature Details:")
    logger.info(f"   Endpoint: {endpoint}")
    logger.info(f"   Params: {params}")
    logger.info(f"   Nonce: {nonce}")
    logger.info(f"   Query String: {query_string}")
    logger.info(f"   Signature Data: {repr(signature_data)}")
    
    # Create HMAC signature
    signature = hmac.new(
        secret_key.encode('utf-8'),
        signature_data.encode('utf-8'),
        hashlib.sha512
    ).hexdigest()
    
    # Base64 encode
    api_sign = base64.b64encode(signature.encode('utf-8')).decode('utf-8')
    
    logger.info(f"   HMAC Hex: {signature[:32]}...")
    logger.info(f"   Base64 Sign: {api_sign[:32]}...")
    
    # Prepare headers
    headers = {
        "api-client-type": "2",
        "api-key": api_key,
        "api-sign": api_sign,
        "api-nonce": nonce,
        "Content-Type": "application/x-www-form-urlencoded"
    }
    
    logger.info(f"\n📤 Request Details:")
    logger.info(f"   URL: https://api.bithumb.com{endpoint}")
    logger.info(f"   Method: POST")
    logger.info(f"   Headers:")
    for k, v in headers.items():
        if k == "api-sign":
            logger.info(f"     {k}: {v[:20]}...")
        elif k == "api-key":
            logger.info(f"     {k}: {v[:8]}...{v[-4:]}")
        else:
            logger.info(f"     {k}: {v}")
    
    # Make request
    async with aiohttp.ClientSession() as session:
        try:
            url = f"https://api.bithumb.com{endpoint}"
            
            # Prepare form data
            form_data = params.copy()
            form_data['endpoint'] = endpoint
            
            async with session.post(url, data=form_data, headers=headers) as response:
                text = await response.text()
                logger.info(f"\n📥 Response:")
                logger.info(f"   Status: {response.status}")
                logger.info(f"   Headers: {dict(response.headers)}")
                
                try:
                    data = json.loads(text)
                    logger.info(f"   Body: {json.dumps(data, indent=2)}")
                    
                    if data.get('status') == '0000':
                        logger.info(f"\n✅ Authentication Successful!")
                    elif data.get('status') == '5100':
                        logger.info(f"\n✅ Authentication OK (No data - empty balance)")
                    else:
                        logger.error(f"\n❌ Error Code: {data.get('status')}")
                        logger.error(f"   Message: {data.get('message', 'Unknown error')}")
                        
                        # Provide specific guidance
                        if data.get('status') == '5300':
                            logger.error("\n🔧 Fix: Invalid API Key")
                            logger.error("   1. Check API key in .env file")
                            logger.error("   2. Make sure there are no spaces or quotes")
                            logger.error("   3. Verify key is active on bithumb.com")
                        elif data.get('status') == '5302':
                            logger.error("\n🔧 Fix: Invalid Signature")
                            logger.error("   1. Check secret key in .env file")
                            logger.error("   2. Make sure you're using the correct secret")
                        elif data.get('status') == '5500':
                            logger.error("\n🔧 Fix: Invalid Nonce")
                            logger.error("   This is a timing issue, try again")
                            
                except json.JSONDecodeError:
                    logger.error(f"   Raw Response: {text[:200]}...")
                    
        except Exception as e:
            logger.error(f"   ❌ Request Error: {e}")
    
    # Test 3: Try alternative endpoints
    logger.info("\n3️⃣ Testing Alternative Endpoints...")
    
    # Try account info
    endpoint2 = "/info/account"
    params2 = {"order_currency": "BTC", "payment_currency": "KRW"}
    
    nonce2 = str(int(time.time() * 1000))
    query_string2 = urlencode(sorted(params2.items()))
    signature_data2 = endpoint2 + chr(0) + query_string2 + chr(0) + nonce2
    
    signature2 = hmac.new(
        secret_key.encode('utf-8'),
        signature_data2.encode('utf-8'),
        hashlib.sha512
    ).hexdigest()
    
    api_sign2 = base64.b64encode(signature2.encode('utf-8')).decode('utf-8')
    
    headers2 = {
        "api-client-type": "2",
        "api-key": api_key,
        "api-sign": api_sign2,
        "api-nonce": nonce2,
        "Content-Type": "application/x-www-form-urlencoded"
    }
    
    async with aiohttp.ClientSession() as session:
        try:
            url = f"https://api.bithumb.com{endpoint2}"
            form_data2 = params2.copy()
            form_data2['endpoint'] = endpoint2
            
            async with session.post(url, data=form_data2, headers=headers2) as response:
                text = await response.text()
                data = json.loads(text) if text else {}
                
                logger.info(f"   Account Info Status: {data.get('status')}")
                if data.get('status') == '0000':
                    logger.info(f"   ✅ Account endpoint works!")
                    
        except Exception as e:
            logger.error(f"   Account Info Error: {e}")
    
    logger.info("\n" + "="*80)
    logger.info("📊 Debug Summary")
    logger.info("="*80)
    
    logger.info("\n🔍 Common Issues:")
    logger.info("1. API Key Issues:")
    logger.info("   - Make sure API key is active on bithumb.com")
    logger.info("   - Check for spaces or quotes in .env file")
    logger.info("   - API key should be 32 characters")
    logger.info("\n2. Secret Key Issues:")
    logger.info("   - Secret key should be 32 characters")
    logger.info("   - No spaces or special characters")
    logger.info("\n3. API Permissions:")
    logger.info("   - Login to bithumb.com")
    logger.info("   - Go to 마이페이지 → API 관리")
    logger.info("   - Make sure '정보 조회' is enabled")
    logger.info("\n4. IP Restrictions:")
    logger.info("   - Check if API has IP restrictions")
    logger.info("   - Your current IP might need to be whitelisted")


if __name__ == "__main__":
    asyncio.run(test_bithumb_raw())