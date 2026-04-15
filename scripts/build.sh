#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

"$root/scripts/bootstrap_godot.sh"

echo "Building opennova libraries and tests..."
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --config Release -j "$jobs"

echo "Running tests..."
ctest --test-dir "$root/build" --output-on-failure -C Release

echo "Build and tests completed successfully."
