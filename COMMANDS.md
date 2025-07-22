# Kimchi Premium Arbitrage Bot - Command Reference

## 🆕 Quick Fix Guide (MUST READ!)

### Common Issues Fixed:
1. **Upbit IP Error** → Add your IP to whitelist (see API_SETUP_GUIDE.md)
2. **Bithumb API Error** → Check API keys for spaces
3. **Rate Limit (429)** → Now handled automatically with retry logic
4. **Gate.io Funding** → Parser fixed

## 🧪 Test Commands (Run These First!)

### 1. Minimal Test (Recommended First Step)
```bash
# Test with minimal API calls to avoid errors
uv run python test_step_by_step.py
```

### 2. Full Connection Test
```bash
# Test all exchange connections and basic functionality
uv run python test_connections.py
```

### 3. Single Symbol Test
```bash
# Test with just BTC to verify everything works
uv run python test_step_by_step.py full
```

This command will:
- Test API connectivity for all configured exchanges
- Retrieve and display symbol counts
- Test USDT price retrieval for Korean exchanges
- Test orderbook data for BTC
- Test funding rates for futures exchanges
- Show a summary of which exchanges are ready

### 2. Quick Health Check
```bash
# Check if Python environment is set up correctly
uv run python -c "import src.exchanges.connector; print('✅ Environment OK')"
```

## 📊 Monitoring Commands

### 1. Basic Monitoring (All Coins)
```bash
# Monitor all common coins between Korean and global exchanges
uv run python main.py monitor
```

### 2. Monitor Specific Coins
```bash
# Monitor only specific coins
uv run python main.py monitor --symbols BTC ETH XRP SOL

# Monitor major coins
uv run python main.py monitor --symbols BTC ETH XRP SOL DOGE AVAX MATIC
```

### 3. Fast Monitoring (1 second refresh)
```bash
# Ultra-fast monitoring for catching opportunities
uv run python main.py monitor --interval 1
```

### 4. Custom Refresh Interval
```bash
# Set custom refresh interval (in seconds)
uv run python main.py monitor --interval 5    # 5 seconds
uv run python main.py monitor --interval 30   # 30 seconds
uv run python main.py monitor --interval 60   # 1 minute (default)
```

## 🚀 Production Commands (After Testing)

### Safe Start Sequence:
```bash
# 1. First, always test connections
uv run python test_step_by_step.py

# 2. Monitor single symbol
uv run python main.py monitor --symbols BTC

# 3. Monitor multiple major coins
uv run python main.py monitor --symbols BTC ETH XRP SOL

# 4. Monitor all coins (if no errors)
uv run python main.py monitor

# 5. OPTIONAL: Start trading (REAL MONEY!)
uv run python main.py trade
```

## 🤖 Trading Commands

### 1. Start Auto-Trading Bot
```bash
# Start the auto-trading bot with split entry strategy
uv run python main.py trade
```

**⚠️ WARNING**: This will execute REAL trades! Make sure:
- API keys have trading permissions
- You have sufficient balance
- You understand the risks
- Test monitoring mode first!

### 2. Trading with Custom Config
```bash
# Use a custom configuration file
uv run python main.py trade --config custom_config.yaml
```

## 🛠️ Setup Commands

### 1. Install Dependencies
```bash
# Install all required dependencies
uv sync
```

### 2. Environment Setup
```bash
# Copy example environment file
cp .env.example .env

# Edit the .env file and add your API keys
nano .env  # or use your preferred editor
```

### 3. Install UV (if not installed)
```bash
# Install UV package manager
curl -LsSf https://astral.sh/uv/install.sh | sh
```

## 🎯 Advanced Usage

### 1. Monitor with Specific Korean Exchange Only
```bash
# If you only have Upbit configured
BITHUMB_API_KEY="" uv run python main.py monitor

# If you only have Bithumb configured  
UPBIT_ACCESS_KEY="" uv run python main.py monitor
```

### 2. Debug Mode
```bash
# Run with debug logging
LOGURU_LEVEL=DEBUG uv run python main.py monitor
```

### 3. Using the Interactive Start Script
```bash
# Run the interactive menu
./start.sh
```

## 📝 Configuration

### Exchange Priority
The bot automatically selects:
- **Korean Exchange**: The one with the lowest ask price (best for buying)
- **Global Exchange**: The one with the highest bid price (best for shorting)

### Default Settings
- **Entry Threshold**: -1.0% (reverse premium)
- **Exit Threshold**: +0.1% (positive premium)
- **Entry Amount**: ₩10,000 per trade
- **Max per Coin**: ₩30,000 (3 entries)
- **Entry Interval**: 60 seconds between entries

## 🚨 Safety Commands

### 1. Dry Run (Monitor Only)
```bash
# Just monitor without trading
uv run python main.py monitor
```

### 2. Check Configuration
```bash
# Validate configuration file
uv run python -c "import yaml; yaml.safe_load(open('config.yaml')); print('✅ Config valid')"
```

### 3. Check Environment Variables
```bash
# Check if all required environment variables are set
uv run python -c "
import os
from dotenv import load_dotenv
load_dotenv()
required = ['UPBIT_ACCESS_KEY', 'UPBIT_SECRET_KEY', 'OKX_API_KEY', 'OKX_SECRET_KEY', 'OKX_PASSPHRASE', 'GATE_API_KEY', 'GATE_SECRET_KEY']
optional = ['BITHUMB_API_KEY', 'BITHUMB_SECRET_KEY']
print('Required:')
for key in required:
    print(f'  {key}: {\"✅ Set\" if os.getenv(key) else \"❌ Missing\"}')
print('\\nOptional:')
for key in optional:
    print(f'  {key}: {\"✅ Set\" if os.getenv(key) else \"⚠️ Not set\"}')
"
```

## 📊 Example Output

### Monitoring Output
```
Symbol   Premium   KR Ex  Global Ex  Funding   Korean Ask     Global Bid         Status
BTC       -1.25%   upbit        okx  0.0100%  ₩130,500,000   $65,250.00   🎯 Entry opportunity!
ETH       -0.85%   bithumb     gate -0.0050%    ₩5,200,000    $3,850.50   👀 Reverse premium
XRP        0.15%   upbit        okx  0.0080%        ₩1,050       $0.7845   
```

### Connection Test Output
```
✅ UPBIT: Connected
✅ BITHUMB: Connected
✅ OKX: Connected
✅ GATE: Connected

📊 Summary: 4/4 exchanges connected
✅ Ready for arbitrage trading!
```

## ⚡ Quick Start Sequence

1. **First Time Setup**:
   ```bash
   # Clone repo
   git clone <repo-url>
   cd kimp_arb_bot
   
   # Install UV
   curl -LsSf https://astral.sh/uv/install.sh | sh
   
   # Install dependencies
   uv sync
   
   # Setup environment
   cp .env.example .env
   # Edit .env with your API keys
   
   # Test connections
   uv run python test_connections.py
   ```

2. **Daily Usage**:
   ```bash
   # Option 1: Use interactive menu
   ./start.sh
   
   # Option 2: Direct commands
   uv run python main.py monitor              # Monitor all
   uv run python main.py monitor --symbols BTC ETH  # Monitor specific
   uv run python main.py trade                # Start trading
   ```

## 🛡️ Safety Tips

1. **Always test connections first** before starting the trading bot
2. **Start with monitoring** to understand the premiums
3. **Use small amounts** when testing real trades
4. **Monitor funding rates** - negative funding can eat profits
5. **Check exchange status** - some exchanges may be under maintenance
6. **Keep logs** - The bot logs all activities for review

## 📞 Troubleshooting

If you encounter issues:

1. **Connection Failed**: Check API keys and network connection
2. **No Symbols Found**: Ensure at least one Korean and one global exchange are connected
3. **Invalid Credentials**: Verify API keys are correct and have required permissions
4. **Module Not Found**: Run `uv sync` to install dependencies