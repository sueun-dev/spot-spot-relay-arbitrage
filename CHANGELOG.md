# Changelog

## [1.1.0] - 2025-07-20

### 🔧 Critical Fixes & Improvements

#### Fixed
- **Critical**: Fixed Upbit market buy order bug (was multiplying by arbitrary 50000000)
- Added `total_krw` field to OrderRequest for proper Upbit market orders
- Corrected hedge ratio calculation with proper fee consideration
- Improved order size rounding with exchange-specific precision

#### Added
- Direct monitoring functionality in main.py (removed separate monitor.py)
- Exchange fee constants (0.05% for all exchanges)
- Minimum order value checks (₩5,000 for Upbit, $1 for others)
- Symbol info caching for precise order size rounding
- Real-time premium monitoring with instant alerts
- Detailed logging showing fees, hedge ratios, and execution details

#### Improved
- **Hedge Calculation**: Now precisely calculates post-fee amounts
  - Upbit: Deducts 0.05% fee from purchased amount
  - Global: Adds 0.05% to ensure equal USD value hedge
- **Order Execution**: Parallel execution with instant rollback on failure
- **BBO Implementation**: Uses ask/bid prices for immediate execution
- **Real-time Detection**: 0.1s monitoring interval for instant opportunity detection

### 📊 Technical Details
- Exact ₩10,000 market buy on Upbit
- Precise USD value calculation after fees
- Symbol-specific decimal precision (BTC: 8, ETH: 5, etc.)
- Instant arbitrage detection and execution

## [1.0.0] - 2025-07-20

### 🎯 Major Refactoring

#### Removed
- Removed unused modules: `bot.py`, `websocket_handler.py`, entire `modules/` directory
- Removed unused exchanges: `bithumb.py`, `bybit.py`
- Removed unused utilities: `helpers.py`
- Removed redundant monitor scripts (consolidated into single `monitor.py` with real-time monitoring)
- Removed all test files from root directory (moved to `tests/`)
- Removed unused configuration sections

#### Added
- Clean `main.py` with command-line interface
- Comprehensive `README.md` with usage instructions
- Proper test suite in `tests/` directory
- `.env.example` for easy setup

#### Changed
- Simplified `config.yaml` to match actual implementation
- Updated `pyproject.toml` to include only necessary dependencies
- Fixed Gate.io symbol fetching (now returns 602 symbols)
- Improved error handling and logging

### 📁 Final Structure
```
kimp_arb_bot/
├── main.py                 # CLI entry point
├── monitor.py              # Real-time monitoring (all coins)
├── config.yaml            # Simplified configuration
├── README.md              # Comprehensive documentation
├── src/
│   ├── exchanges/         # Exchange implementations
│   ├── strategies/        # Split entry strategy
│   └── utils/            # Utilities
└── tests/                # All test files
```

### 🚀 Usage
```bash
# Monitor premiums (real-time for all coins)
uv run python main.py monitor

# Run trading bot
uv run python main.py trade

# Run tests
uv run pytest tests/
```