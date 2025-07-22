# 📁 Kimchi Premium Bot - Project Structure

```
kimp_arb_bot/
├── .env                           # API credentials (DO NOT COMMIT)
├── .gitignore                     # Git ignore rules
├── README.md                      # Project documentation
├── requirements.txt               # Python dependencies
├── config.yaml                    # Bot configuration
├── main.py                        # Main entry point
│
├── src/                           # Source code
│   ├── __init__.py
│   ├── exchanges/                 # Exchange implementations
│   │   ├── __init__.py
│   │   ├── base.py               # Base exchange class
│   │   ├── connector.py          # Exchange connector factory
│   │   ├── upbit.py              # Upbit implementation
│   │   ├── bithumb.py            # Bithumb implementation
│   │   ├── okx.py                # OKX implementation
│   │   └── gate.py               # Gate.io implementation
│   │
│   ├── strategies/               # Trading strategies
│   │   ├── __init__.py
│   │   └── split_entry_strategy.py  # Split entry strategy
│   │
│   ├── models/                   # Data models
│   │   ├── __init__.py
│   │   └── models.py             # Order, balance models
│   │
│   └── utils/                    # Utilities
│       ├── __init__.py
│       ├── logger.py             # Logging configuration
│       └── premium_calculator.py # Premium calculation logic
│
├── tests/                        # All test files
│   ├── __init__.py
│   ├── test_exchanges.py         # Exchange tests
│   ├── test_split_entry.py       # Strategy tests
│   ├── test_premium_calculation.py # Premium calc tests
│   ├── test_simple.py            # Simple tests
│   ├── test_all_exchanges.py     # Comprehensive exchange test
│   ├── test_bithumb_v2.py        # Bithumb v2.0 API test
│   ├── test_connections.py       # Connection tests
│   ├── test_step_by_step.py      # Step by step test
│   └── debug_bithumb.py          # Bithumb debugging tool
│
├── scripts/                      # Utility scripts
│   ├── start_trading.sh          # Trading start script
│   ├── azure_vm_connect.sh       # Azure VM connection
│   └── connect_azure.sh          # Azure connection helper
│
└── docs/                         # Documentation
    ├── API_SETUP_GUIDE.md        # API setup instructions
    ├── AZURE_VM_GUIDE.md         # Azure VM guide
    ├── TRADING_COMMANDS.md       # Trading commands
    └── FIXES_COMPLETED.md        # Completed fixes
```

## 🗑️ Removed/Unused Files
- `monitor.py` - Monitoring functionality (removed)
- `backtest.py` - Backtesting functionality (removed)
- Bybit exchange implementation (not used)
- Duplicate test files in root directory

## 📌 Key Files

### Configuration
- `.env` - API keys and secrets
- `config.yaml` - Trading parameters and settings

### Core Implementation
- `main.py` - Entry point for trading
- `src/exchanges/connector.py` - Manages all exchanges
- `src/strategies/split_entry_strategy.py` - Trading logic

### Testing
- `tests/test_all_exchanges.py` - Test all connections
- `tests/test_bithumb_v2.py` - Test Bithumb v2.0 API

### Documentation
- `README.md` - Project overview
- `docs/API_SETUP_GUIDE.md` - How to set up APIs
- `docs/TRADING_COMMANDS.md` - Quick start guide