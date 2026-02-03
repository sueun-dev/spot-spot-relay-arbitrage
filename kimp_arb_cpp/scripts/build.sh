#!/bin/bash
# Build script for KIMP Arbitrage Bot

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Build type
BUILD_TYPE="${1:-Release}"
echo "Building in $BUILD_TYPE mode..."

# Create build directory
mkdir -p build
cd build

# Check if conan is available
if command -v conan &> /dev/null; then
    echo "Installing dependencies with Conan..."
    conan install .. --output-folder=. --build=missing -s build_type=$BUILD_TYPE
    cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
else
    echo "Conan not found, trying system packages..."
    cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE
fi

# Build
echo "Building..."
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "Build complete!"
echo "Executable: $PROJECT_DIR/build/kimp_bot"
