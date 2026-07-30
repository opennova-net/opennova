#!/usr/bin/env bash
# Builds the Godot GDExtension (godot/engine/) into godot/bin/.
#
# This is what registers the engine's C++ classes (NovaTerrain, RtxtStringFile,
# NovaResourceRoot, ...) with the editor. scripts/build.sh builds the libs/ +
# ctest suite but NOT this DLL, so the editor can run a stale extension and fail
# to parse any GDScript that references a newly added class. Run this after
# adding/changing engine/ sources, then fully restart the editor — GDExtension
# class registration does not reliably hot-reload (especially on Windows, where
# the running editor holds the DLL lock and the swap is deferred to a ~temp).
#
# Usage: scripts/build_godot.sh [Dev|DebugFull|Release]   (default: Dev)
#   Dev       -> libopennova.<platform>.template_debug.x86_64.<dll|so>  (editor)
#                RelWithDebInfo: optimized native code (/O2 + symbols). This is
#                the flavor every editor session and every game runtime it
#                launches (F5/F6) load; an
#                unoptimized build here costs ~1.5x whole-frame time in-game
#                (MSVC Debug is /Od /RTC1: ~5-10x on native sim work). The
#                artifact NAME stays template_debug — godot-cpp derives it from
#                GODOTCPP_TARGET, not the CMake config — so the editor picks it
#                up unchanged.
#   DebugFull -> same template_debug artifact, true Debug (/Od, runtime checks,
#                asserts live). Use when stepping through native code or chasing
#                memory corruption; do not judge performance on it.
#   Release   -> libopennova.<platform>.template_release.x86_64.<dll|so> (export)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
flavor="${1:-Dev}"

case "$flavor" in
    Dev) config="RelWithDebInfo" ;;
    DebugFull) config="Debug" ;;
    Release) config="Release" ;;
    *)
        echo "usage: scripts/build_godot.sh [Dev|DebugFull|Release]" >&2
        exit 2
        ;;
esac

echo "Building Godot GDExtension ($flavor -> CMake config $config)..."
# CMAKE_BUILD_TYPE drives single-config generators (Linux/macOS Makefiles or
# Ninja); --config drives multi-config generators (Visual Studio). Passing both
# keeps one code path for either.
cmake -S "$root/godot/engine" -B "$root/godot/engine/build" -DCMAKE_BUILD_TYPE="$config"
cmake --build "$root/godot/engine/build" --config "$config" -j "$jobs"

echo "GDExtension ($flavor) built into godot/bin/ — restart the Godot editor to load it."
