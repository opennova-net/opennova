#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

echo "Building opennova libraries and tests..."
cmake -S "$root" -B "$root/build"
cmake --build "$root/build" -j "$jobs"

echo "Running tests..."
ctest --test-dir "$root/build" -j "$jobs" --output-on-failure

echo "Build and tests completed successfully."
