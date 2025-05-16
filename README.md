# OmLaxmiQuant

A low-latency, high-performance trading system for algorithmic trading.

## System Architecture

The system consists of three main components:

### Ida

Core trading system written in C++ for maximum performance:

- **Exchange**: A simulated exchange with matching engine, order server, and market data publisher
- **Trading**: Client implementations with market data consumer, order gateway, and trading strategies
- **Common**: Shared utilities for threading, networking, logging, and memory management

### Pingala

Python-based analytics and backtesting framework:

- Data analysis
- Strategy research
- Performance measurement
- Visualization

### Sushumna

Frontend and monitoring dashboard.

## Building and Running

### Prerequisites

- C++17 compatible compiler
- CMake 3.15+
- Boost libraries
- Python 3.8+ (for Pingala)

### Build Instructions

```bash
cd ida
mkdir -p cmake-build-release
cd cmake-build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Running the System

```bash
# Start the exchange
./exchange_main

# In another terminal, start the trading clients
cd scripts
./run_clients.sh
```

## Configuration

Trading strategies and system parameters can be configured in `ida/config/StrategyConfig.json`.

## Binance Integration

See `ida/docs/Binance_Integration_Merged.md` for detailed implementation plan for connecting to Binance exchange.

## License

All rights reserved.