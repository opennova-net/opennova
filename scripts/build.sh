#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

"$root/scripts/bootstrap_godot.sh"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        if [[ -d "$root/third_party/modsuperoed" ]]; then
            echo "Preparing ModSuperOED test artifacts..."
            git -C "$root/third_party/modsuperoed" lfs install --local
            git -C "$root/third_party/modsuperoed" lfs pull
            git -C "$root/third_party/modsuperoed" lfs fsck --objects
            cmake -S "$root" -B "$root/build-modsuperoed" -A Win32
            cmake --build "$root/build-modsuperoed" --config Release \
                --target modsuperoed_injector modsuperoed_hook -j "$jobs"
        fi
        ;;
esac

echo "Building opennova libraries and tests..."
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIB=ON \
    -DOPENNOVA_ENABLE_PYTHON_TESTS=ON
cmake --build "$root/build" --config Release -j "$jobs"

echo "Running tests..."
ctest --test-dir "$root/build" --output-on-failure -C Release

echo "Build and tests completed successfully."
