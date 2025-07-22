# 🚀 Kimchi Premium Trading Bot - Quick Start

## ⚡ Start Trading (3 Simple Commands)

### 1. Test Your Setup (Required First Time)
```bash
uv run python test_step_by_step.py
```
Make sure you see at least:
- ✅ 1 Korean exchange (Upbit or Bithumb) 
- ✅ 1 Global exchange (OKX or Gate)

### 2. Start Trading with Safety Check
```bash
./start_trading.sh
```
This will:
- Check your connections
- Confirm you want to trade
- Start the bot

### 3. Direct Trading (No Confirmation)
```bash
uv run python main.py
```

## 🛑 Stop Trading
Press `Ctrl+C` anytime to stop the bot safely.

## 💰 What The Bot Does

1. **Monitors** all coins for reverse premium ≤ -1.0%
2. **Buys** ₩10,000 worth on Korean exchange (spot)
3. **Shorts** same amount on global exchange (futures)
4. **Waits** for premium to reach ≥ +0.1%
5. **Exits** both positions for profit

## ⚠️ Important Notes

- **Real Money**: This bot trades with real money!
- **Small Amounts**: Starts with ₩10,000 per entry
- **Max Risk**: ₩30,000 per coin (3 entries max)
- **Funding Rate**: Only enters when funding ≥ 0%

## 🔧 If You Have Issues

### Upbit Error:
```bash
# Your IP is not whitelisted
# Go to upbit.com → 마이페이지 → Open API 관리 → Add your IP
```

### Bithumb Error:
```bash
# Check your API keys in .env file
# No spaces, no quotes
```

### Need Help?
Check `API_SETUP_GUIDE.md` for detailed setup instructions.

## 📊 Example Output When Trading

```
🚀 Starting Kimchi Premium Trading Bot...
⚡ Testing exchange connections...
✅ UPBIT: Connected
✅ BITHUMB: Connected
✅ OKX: Connected
✅ GATE: Connected

✅ Ready to trade!

🎯 Starting split entry strategy...
Found 150 common symbols
Split Entry Strategy started

🎯 ENTRY SIGNAL: BTC @ -1.25%
  Upbit: ₩10,000 market buy
  OKX: 0.00015 BTC short @ $65,250

✅ Position opened successfully
```

## 🎯 That's It!

Just run `./start_trading.sh` and the bot handles everything else!