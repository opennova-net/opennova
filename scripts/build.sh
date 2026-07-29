#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

"$root/scripts/bootstrap_godot.sh"

echo "Building opennova libraries and tests..."
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON \
    -DOPENNOVA_ENABLE_PYTHON_TESTS=ON
cmake --build "$root/build" --config Release -j "$jobs"

echo "Running tests..."
ctest --test-dir "$root/build" --output-on-failure -C Release --parallel "$jobs"

# Build the Godot GDExtension too, so the editor doesn't load a stale DLL
# missing classes that engine/ has since added. Skippable via BUILD_GODOT=0
# for lib-only iteration.
if [[ "${BUILD_GODOT:-1}" != "0" ]]; then
    "$root/scripts/build_godot.sh"
fi

echo "Build and tests completed successfully."
