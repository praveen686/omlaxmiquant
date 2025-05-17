#!/bin/bash

# Script to run Binance integration tests
# This script builds and runs the Binance tests

echo "Building Binance tests..."
cd "$(dirname "$0")/.." || exit 1
bash scripts/no_clean_build.sh || exit 1

echo "----------------------------------------------"
echo "Running Binance Market Data Test"
echo "----------------------------------------------"
./cmake-build-release/testing/binance/test_market_data --test
echo ""

echo "----------------------------------------------"
echo "Running Binance Order Gateway Test"
echo "----------------------------------------------"
./cmake-build-release/testing/binance/test_order_gateway --test
echo ""

echo "----------------------------------------------"
echo "Running Binance API Test"
echo "----------------------------------------------"
./cmake-build-release/testing/binance/test_binance_api --test
echo ""

echo "All tests completed!"