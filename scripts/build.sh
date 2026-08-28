#!/usr/bin/env bash
# Builds the engine libraries and the ctest suite (Release), runs the tests,
# then builds the Godot GDExtension so the editor never loads a stale DLL
# missing classes that engine/ has since added.
#
# Usage: scripts/build.sh [--no-godot] [--jobs N]
#   --no-godot  skip the Godot addon bootstrap and the GDExtension build
#               (library-only iteration; what CI's engine test job runs)
#   --jobs N    build/test parallelism (default: the machine's CPU count)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
build_godot=1
usage="usage: scripts/build.sh [--no-godot] [--jobs N]"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-godot) build_godot=0; shift ;;
        --jobs)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            jobs="$2"; shift 2 ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done

# The Godot addons (GUT, imgui-godot) are project assets for the GDExtension
# flavour, not a dependency of the C++ targets: the Godot-free path skips the
# bootstrap.
if [[ "$build_godot" == "1" ]]; then
    "$root/scripts/bootstrap_godot.sh"
fi

echo "Building opennova libraries and tests..."
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --config Release -j "$jobs"

echo "Running tests..."
# The JUnit report is what scripts/ci/retail_gates_ran.py reads to prove the
# asset-gated tests ran (rather than skipped) once the reference data is mounted.
ctest --test-dir "$root/build" --output-on-failure -C Release --parallel "$jobs" \
  --output-junit "$root/build/Testing/ctest.xml"

if [[ "$build_godot" == "1" ]]; then
    "$root/scripts/build_godot.sh" --jobs "$jobs"
fi

echo "Build and tests completed successfully."
