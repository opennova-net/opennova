#!/usr/bin/env bash
# Builds the Godot GDExtension (godot/src/) into godot/bin/.
#
# This is what registers the engine's C++ classes (Terrain, RtxtStringFile,
# ResourceRoot, ...) with the editor. scripts/build.sh runs it after the engine/
# build + ctest unless given --no-godot; an engine-only build leaves a stale
# extension that fails to parse GDScript naming a new class. Run this after
# adding/changing engine/ sources, then fully restart the editor — GDExtension
# class registration does not reliably hot-reload (especially on Windows, where
# the running editor holds the DLL lock and the swap is deferred to a ~temp).
#
# Usage: scripts/build_godot.sh [Dev|DebugFull|Release] [--jobs N]   (default: Dev)
#   --jobs N  -> build parallelism (default: the machine's CPU count)
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
#   Release   -> same template_debug artifact, plain Release (/O2, no symbols).
#                GODOTCPP_TARGET is never passed here, so no flavor of this
#                script produces the template_release-named DLL an
#                --export-release exe loads; scripts/package_godot_windows.ps1
#                (-ExportMode release) is the only producer of that one.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
flavor="Dev"
usage="usage: scripts/build_godot.sh [Dev|DebugFull|Release] [--jobs N]"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            jobs="$2"; shift 2 ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        Dev|DebugFull|Release) flavor="$1"; shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done

case "$flavor" in
    Dev) config="RelWithDebInfo" ;;
    DebugFull) config="Debug" ;;
    Release) config="Release" ;;
esac

# MSVC reads extra compiler options from CL (e.g. CL=/MP16). Git Bash rewrites an
# environment value that looks like a POSIX path when it starts a Windows program,
# so /MP16 would reach cl.exe as "C:/Program Files/Git/MP16" and fail every
# compile (CMake cannot even identify the compiler). Exclude CL from that
# conversion: MSYS2_ENV_CONV_EXCL is a ';' list of variable names, and means
# nothing outside MSYS, so other hosts are unaffected.
export MSYS2_ENV_CONV_EXCL="CL${MSYS2_ENV_CONV_EXCL:+;$MSYS2_ENV_CONV_EXCL}"

echo "Building Godot GDExtension ($flavor -> CMake config $config)..."
# CMAKE_BUILD_TYPE drives single-config generators (Linux/macOS Makefiles or
# Ninja); --config drives multi-config generators (Visual Studio). Passing both
# keeps one code path for either.
cmake -S "$root/godot/src" -B "$root/godot/src/build" -DCMAKE_BUILD_TYPE="$config"
cmake --build "$root/godot/src/build" --config "$config" -j "$jobs"

echo "GDExtension ($flavor) built into godot/bin/ — restart the Godot editor to load it."
