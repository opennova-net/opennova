#!/usr/bin/env bash
# Build the opennova shared library (Windows DLL) via MinGW cross-compilation
# and copy it to blender/lib/windows-x64/
set -eu
if [ -n "${BASH_VERSION:-}" ]; then set -o pipefail; fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-win64"
DEST_DIR="$ROOT_DIR/blender/lib/windows-x64"

echo "=== Building opennova.dll (MinGW cross-compile) ==="
echo "Build dir: $BUILD_DIR"
echo "Dest: $DEST_DIR"

mkdir -p "$BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc

cmake --build "$BUILD_DIR" --target opennova_shared --config Release -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "$DEST_DIR"
# MinGW produces libopennova.dll — copy as opennova.dll
cp "$BUILD_DIR/libopennova.dll" "$DEST_DIR/opennova.dll"

echo "=== Done: $DEST_DIR/opennova.dll ==="
ls -lh "$DEST_DIR/"
