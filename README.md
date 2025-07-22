# 🚀 Kimchi Premium Arbitrage Bot

Automated cryptocurrency arbitrage bot that exploits price differences between Korean and global exchanges.

## 📊 Strategy

**Split Entry Strategy**: Enters positions in ₩10,000 increments (max 3 entries per symbol)
- **Entry Signal**: Reverse premium ≤ -1.0%
- **Exit Signal**: Premium ≥ +0.1%
- **Risk Management**: Maximum ₩30,000 per symbol

## 🏦 Supported Exchanges

### Korean Exchanges (Spot)
- **Upbit** - Korea's largest exchange
- **Bithumb** - v2.0 API implementation

### Global Exchanges (Futures)
- **OKX** - Perpetual futures
- **Gate.io** - USDT perpetual contracts

## 🛠️ Setup

### 1. Install Dependencies
```bash
# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install packages
pip install -r requirements.txt
```

### 2. Configure API Keys
Copy `.env.example` to `.env` and add your API credentials:
```bash
cp .env.example .env
```

Edit `.env` with your API keys:
```
UPBIT_ACCESS_KEY=your_key_here
UPBIT_SECRET_KEY=your_secret_here
BITHUMB_API_KEY=your_key_here
BITHUMB_SECRET_KEY=your_secret_here
OKX_API_KEY=your_key_here
OKX_SECRET_KEY=your_secret_here
OKX_PASSPHRASE=your_passphrase_here
GATE_API_KEY=your_key_here
GATE_SECRET_KEY=your_secret_here
```

### 3. Configure Trading Parameters
Edit `config.yaml` to adjust:
- Entry/exit thresholds
- Position sizes
- Risk limits

## 🚀 Usage

### Test Connections
```bash
# Test all exchanges
python tests/test_all_exchanges.py

# Verify exchange setup
python tests/test_verify_exchanges.py
```

### Start Trading
```bash
# With connection test
python main.py --test

# Direct start
python main.py
```

### Using Scripts
```bash
# Easy start with confirmation
./scripts/start_trading.sh
```

## 📁 Project Structure

```
kimp_arb_bot/
├── src/
│   ├── exchanges/      # Exchange implementations
│   ├── strategies/     # Trading strategies
│   ├── models/         # Data models
│   └── utils/          # Utilities
├── tests/              # All test files
├── scripts/            # Utility scripts
├── docs/               # Documentation
└── main.py             # Entry point
```

## ⚠️ Important Notes

1. **Real Money Trading**: This bot trades with real money. Start with small amounts.
2. **IP Whitelist**: Upbit requires IP whitelisting in their settings.
3. **API Permissions**: Only enable trading permissions, NOT withdrawal.
4. **Risk Management**: Always monitor positions and set appropriate limits.

## 🧪 Testing

Run the test suite:
```bash
# Run all tests
pytest tests/

# Test specific exchange
python tests/test_bithumb_v2.py
```

## 📖 Documentation

- [API Setup Guide](docs/API_SETUP_GUIDE.md) - Detailed API setup instructions
- [Trading Commands](docs/TRADING_COMMANDS.md) - Quick command reference
- [Azure VM Guide](docs/AZURE_VM_GUIDE.md) - Deploy on Azure VM
- [Project Structure](PROJECT_STRUCTURE.md) - File organization

## 🛟 Troubleshooting

### Upbit "no_authorization_ip" Error
Add your IP address to the whitelist in Upbit settings.

### Bithumb "Invalid Apikey" Error
Check that your API key is correctly formatted (32 characters, no quotes).

### Rate Limiting (429 Errors)
The bot implements automatic rate limiting and retry logic.

## 📄 License

This project is for educational purposes. Use at your own risk.