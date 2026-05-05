#!/usr/bin/env bash
# Build native libraries for all platforms and package the Blender addon as a zip.
# Requires: cmake, mingw-w64 (for Windows cross-compile)
#
# Usage: scripts/package_addon.sh [--version VERSION]
set -eu
if [ -n "${BASH_VERSION:-}" ]; then set -o pipefail; fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BLENDER_DIR="$ROOT_DIR/blender"
PYOPENNOVA_DIR="$ROOT_DIR/pyopennova"
DIST_DIR="$ROOT_DIR/dist"
VERSION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            VERSION="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. Build Linux shared library
# ---------------------------------------------------------------------------
echo "=== Building libopennova.so (Linux) ==="
BUILD_LINUX="$ROOT_DIR/build-linux"
cmake -S "$ROOT_DIR" -B "$BUILD_LINUX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON

cmake --build "$BUILD_LINUX" --target opennova_shared --config Release \
    -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "$PYOPENNOVA_DIR/lib/linux-x64"
cp "$BUILD_LINUX/libopennova.so" "$PYOPENNOVA_DIR/lib/linux-x64/libopennova.so"
echo "  -> pyopennova/lib/linux-x64/libopennova.so"

# ---------------------------------------------------------------------------
# 2. Build Windows DLL via MinGW cross-compile
# ---------------------------------------------------------------------------
echo "=== Building opennova.dll (MinGW cross-compile) ==="
BUILD_WIN64="$ROOT_DIR/build-win64"
cmake -S "$ROOT_DIR" -B "$BUILD_WIN64" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++

cmake --build "$BUILD_WIN64" --target opennova_shared --config Release \
    -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "$PYOPENNOVA_DIR/lib/windows-x64"
cp "$BUILD_WIN64/libopennova.dll" "$PYOPENNOVA_DIR/lib/windows-x64/opennova.dll"
echo "  -> pyopennova/lib/windows-x64/opennova.dll"

# ---------------------------------------------------------------------------
# 3. Update manifest version if requested
# ---------------------------------------------------------------------------
MANIFEST="$BLENDER_DIR/blender_manifest.toml"
if [[ -n "$VERSION" ]]; then
    echo "=== Updating manifest version to $VERSION ==="
    sed -i "s/^version = .*/version = \"$VERSION\"/" "$MANIFEST"
fi

# ---------------------------------------------------------------------------
# 4. Create zip
# ---------------------------------------------------------------------------
mkdir -p "$DIST_DIR"

if [[ -z "$VERSION" ]]; then
    VERSION=$(grep -E '^version = ' "$MANIFEST" | sed -E 's/.*"(.*)".*/\1/')
fi
ZIP_NAME="opennova_blender-v${VERSION}.zip"

echo "=== Packaging $ZIP_NAME ==="
# Stage pyopennova inside the addon dir so the zipped addon is self-contained.
# Removed at the end so the working tree stays clean.
VENDORED_PYOPENNOVA="$BLENDER_DIR/pyopennova"
rm -rf "$VENDORED_PYOPENNOVA"
cp -r "$PYOPENNOVA_DIR" "$VENDORED_PYOPENNOVA"
trap 'rm -rf "$VENDORED_PYOPENNOVA"' EXIT

(cd "$BLENDER_DIR" && zip -r "$DIST_DIR/$ZIP_NAME" . \
    -x "__pycache__/*" "*.pyc" "__pycache__" "**/__pycache__/*")

echo "=== Done: dist/$ZIP_NAME ==="
ls -lh "$DIST_DIR/$ZIP_NAME"
