#!/usr/bin/env bash
# Build and run the C test suite
set -eu
if [ -n "${BASH_VERSION:-}" ]; then set -o pipefail; fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-test"

echo "=== Configuring tests ==="
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "=== Building tests ==="
cmake --build "$BUILD_DIR" --config Release -j"$(nproc 2>/dev/null || echo 4)"

echo "=== Running tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure -C Release
