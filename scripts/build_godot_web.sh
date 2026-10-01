#!/usr/bin/env bash
# Builds the web GDExtension (godot/src/ as a wasm32 threads side module) into
# godot/bin/libopennova.web.<target>.wasm32.wasm, the name opennova.gdextension
# lists for the Web export preset's dlink threads template.
#
# The side module links against Godot's own web template at load time, so it
# must come from the Emscripten that built those templates: 4.0.20 for Godot
# 4.6 (godotengine/build-containers, branch 4.6, Dockerfile.web). The script
# refuses any other emcc. Without that emcc on PATH, run it in the pinned image
# (whose CMake 3.22 predates the sqlite fetch's 3.24 options, hence the pip):
#
#   docker run --rm -v "$PWD:/src" -w /src emscripten/emsdk:4.0.20 bash -c \
#       "pip install -q 'cmake>=3.28,<4' ninja && scripts/build_godot_web.sh [--release]"
#
# Usage: scripts/build_godot_web.sh [--release] [--jobs N] [--build-dir DIR]
#   (default)  -> template_debug (the debug web export loads it)
#   --release  -> template_release (the published site's export loads it)
set -euo pipefail

emscripten_version="4.0.20"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="$(nproc 2>/dev/null || echo 4)"
target="template_debug"
build_dir=""
usage="usage: scripts/build_godot_web.sh [--release] [--jobs N] [--build-dir DIR]"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) target="template_release"; shift ;;
        --jobs)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            jobs="$2"; shift 2 ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        --build-dir)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            build_dir="$2"; shift 2 ;;
        --build-dir=*) build_dir="${1#--build-dir=}"; shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done
# Outside godot/, so the editor never scans the build tree.
[[ -n "$build_dir" ]] || build_dir="$root/build-web-$target"

command -v emcc >/dev/null || {
    echo "emcc not found; run this inside emscripten/emsdk:$emscripten_version (see the header)" >&2
    exit 1
}
have="$(emcc --version | head -n 1 | sed -E 's/.* ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
if [[ "$have" != "$emscripten_version" ]]; then
    echo "emcc $have; the Godot 4.6 web templates need exactly $emscripten_version" >&2
    exit 1
fi

# Optimized either way: the target picks Godot's debug features, and DWARF
# would multiply the download a browser fetches.
echo "Building web GDExtension ($target) in $build_dir..."
generator=()
command -v ninja >/dev/null && generator=(-G Ninja)
emcmake cmake -S "$root/godot/src" -B "$build_dir" "${generator[@]}" \
    -DGODOTCPP_TARGET="$target" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target opennova -j "$jobs"

echo "Built godot/bin/libopennova.web.$target.wasm32.wasm"
