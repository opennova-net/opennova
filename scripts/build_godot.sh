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
# Usage: scripts/build_godot.sh [Debug|Release]   (default: Debug)
#   Debug   -> libopennova.<platform>.template_debug.x86_64.<dll|so>  (editor)
#   Release -> libopennova.<platform>.template_release.x86_64.<dll|so> (export)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
config="${1:-Debug}"

echo "Building Godot GDExtension ($config)..."
cmake -S "$root/godot/engine" -B "$root/godot/engine/build"
cmake --build "$root/godot/engine/build" --config "$config" -j "$jobs"

echo "GDExtension built into godot/bin/ — restart the Godot editor to load it."
