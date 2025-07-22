# API Setup Guide - Complete Instructions

## 🚨 IMPORTANT: API Key Issues and Solutions

### 1. Upbit API Error: "no_authorization_ip"

**Problem**: Your IP address is not whitelisted in Upbit API settings.

**Solution**:
1. Log in to Upbit website
2. Go to **마이페이지** (My Page) → **Open API 관리** (Open API Management)
3. Find your API key and click **수정** (Edit)
4. In **허용 IP** (Allowed IP) section:
   - Add your current IP address
   - You can find your IP at: https://whatismyipaddress.com
   - Format: `123.456.789.012`
5. Save changes and wait 1-2 minutes for it to take effect

**Alternative**: If you're using a dynamic IP, you can temporarily set it to allow all IPs:
- Enter `0.0.0.0/0` (NOT RECOMMENDED for production)
- Only use this for testing, then restrict to your actual IP

### 2. Bithumb API Error: "Invalid Apikey"

**Problem**: API key format or configuration issue.

**Solution**:
1. Log in to Bithumb website
2. Go to **회원정보** → **API 관리**
3. Create a new API key with these permissions:
   - **조회** (View): ✅ Required
   - **출금** (Withdrawal): ❌ Not needed
   - **거래** (Trading): ✅ Required for trading bot
4. **IMPORTANT**: Copy both:
   - API Key (Connect Key)
   - Secret Key
5. Make sure there are no extra spaces when copying

### 3. Rate Limiting (429 Errors)

**Already Fixed**: The code now includes:
- Exchange-specific rate limits
- Batch processing (20 symbols at a time)
- Exponential backoff retry logic
- Proper delays between requests

## 📋 Complete API Setup Checklist

### Upbit Setup
```bash
# 1. Create API Keys at https://upbit.com
# 2. Required permissions:
#    - 자산 조회 (Asset View): ✅
#    - 주문 조회 (Order View): ✅
#    - 주문하기 (Place Order): ✅ (for trading)
#    - 출금하기 (Withdrawal): ❌ (not needed)

# 3. CRITICAL: Add your IP address to whitelist!

# 4. Add to .env file:
UPBIT_ACCESS_KEY=your_access_key_here
UPBIT_SECRET_KEY=your_secret_key_here
```

### Bithumb Setup
```bash
# 1. Create API Keys at https://bithumb.com
# 2. Required permissions:
#    - INFO (정보 조회): ✅
#    - TRADE (거래): ✅ (for trading)

# 3. Add to .env file:
BITHUMB_API_KEY=your_api_key_here
BITHUMB_SECRET_KEY=your_secret_key_here
```

### OKX Setup
```bash
# 1. Create API Keys at https://www.okx.com
# 2. Required permissions:
#    - Read: ✅
#    - Trade: ✅ (for trading)
# 3. Set Passphrase (remember it!)

# 4. Add to .env file:
OKX_API_KEY=your_api_key_here
OKX_SECRET_KEY=your_secret_key_here
OKX_PASSPHRASE=your_passphrase_here
```

### Gate.io Setup
```bash
# 1. Create API Keys at https://www.gate.io
# 2. Required permissions:
#    - Spot/Margin Trade: ❌ (not needed)
#    - Futures Trade: ✅
#    - Read: ✅

# 3. Add to .env file:
GATE_API_KEY=your_api_key_here
GATE_SECRET_KEY=your_secret_key_here
```

## 🧪 Testing Your Setup

### Step 1: Test Connections Only
```bash
# This will test all API connections without trading
uv run python test_connections.py
```

Expected output:
```
✅ UPBIT: Connected
✅ BITHUMB: Connected
✅ OKX: Connected
✅ GATE: Connected
```

### Step 2: Monitor Mode (Safe)
```bash
# Monitor only BTC to minimize API calls
uv run python main.py monitor --symbols BTC

# If successful, try more symbols
uv run python main.py monitor --symbols BTC ETH XRP
```

### Step 3: Full Monitoring
```bash
# Monitor all symbols (will make more API calls)
uv run python main.py monitor
```

## 🛠️ Troubleshooting

### If Upbit still shows IP error:
1. Double-check your IP is correctly added
2. Try using a VPN to get a stable IP
3. Consider using a cloud server with static IP

### If Bithumb shows API key error:
1. Regenerate API keys
2. Make sure you're using the correct key format
3. Check there are no spaces or line breaks in your .env file

### If you see rate limit errors:
1. The code now handles this automatically
2. If persistent, reduce batch_size in the code
3. Increase delays between requests

### Environment File Check:
```bash
# Make sure your .env file has correct format (no quotes needed):
UPBIT_ACCESS_KEY=abcd1234efgh5678
UPBIT_SECRET_KEY=ijkl9012mnop3456

# NOT like this (wrong):
UPBIT_ACCESS_KEY="abcd1234efgh5678"
UPBIT_SECRET_KEY='ijkl9012mnop3456'
```

## 📱 Mobile App API Setup

### Upbit Mobile:
1. Open Upbit app
2. Go to 더보기 (More) → Open API 관리
3. Create API key
4. **IMPORTANT**: Set IP whitelist

### Bithumb Mobile:
1. Open Bithumb app
2. Go to 더보기 → API 인증 관리
3. Create API key
4. Copy both keys carefully

## 🔒 Security Best Practices

1. **Never share your API keys**
2. **Use IP whitelisting** (especially for Upbit)
3. **Don't enable withdrawal permissions**
4. **Use read-only keys for testing**
5. **Store keys in .env file, never in code**
6. **Add .env to .gitignore**

## 🚀 Quick Start After Setup

```bash
# 1. Test connections
uv run python test_connections.py

# 2. If all green, start monitoring
uv run python main.py monitor --symbols BTC ETH

# 3. Once comfortable, monitor all
uv run python main.py monitor

# 4. For trading (be careful!)
uv run python main.py trade
```

## 📞 Need Help?

Common issues:
- **IP not whitelisted**: Most common Upbit issue
- **Wrong API format**: Check for extra spaces
- **Permissions**: Make sure trading is enabled if needed
- **Rate limits**: Code now handles automatically

Remember: Start with test_connections.py to verify everything works!