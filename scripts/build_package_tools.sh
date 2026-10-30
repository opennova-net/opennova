#!/usr/bin/env bash
# Build the union of the Windows packages' CLI dependencies once, with the
# static MSVC runtime. Consumers receive a flat directory of executables.
# Usage: build_package_tools.sh [--out DIR] --tools|--blender|--godot ... [-D<VAR>=<value>...]
# CMake -D arguments carry CI's ONE stamp; omitted, CMake uses its git defaults.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build/package-tools"
build="$root/build/apps-package"
apps=()
cmake_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) [[ $# -ge 2 ]] || exit 2; out="$2"; shift 2 ;;
        --tools) apps+=(3di extract lan_probe parity serve update_tool wire); shift ;;
        --blender) apps+=(3di); shift ;;
        --godot) apps+=(update revupdat update_tool); shift ;;
        -D*) cmake_args+=("$1"); shift ;;
        *) echo "unknown package-tools argument: $1" >&2; exit 2 ;;
    esac
done
[[ ${#apps[@]} -gt 0 ]] || { echo "select --tools, --blender or --godot" >&2; exit 2; }
mapfile -t apps < <(printf '%s\n' "${apps[@]}" | sort -u)

# No package carries opennova-game yet (OPENNOVA_GAME, the SDL3 app, ADR 0077 d9).
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPENNOVA_DEVTOOLS=OFF \
    -DOPENNOVA_EDITOR=OFF -DOPENNOVA_GAME=OFF "${cmake_args[@]}"
targets=()
for app in "${apps[@]}"; do targets+=("opennova_$app"); done
cmake --build "$build" --config Release --target "${targets[@]}"

mkdir -p "$out"
for app in "${apps[@]}"; do
    case "$app" in
        update|revupdat) name="$app" ;;
        *) name="opennova-${app//_/-}" ;;
    esac
    exe="$build/apps/$app/Release/$name.exe"
    [[ -f "$exe" ]] || exe="$build/apps/$app/$name.exe"
    [[ -f "$exe" ]] || { echo "package tool was not built: $name.exe" >&2; exit 1; }
    cp "$exe" "$out/$name.exe"
done
