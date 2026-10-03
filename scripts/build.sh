#!/usr/bin/env bash
# Builds the engine libraries and the ctest suite (Release), runs the tests,
# then builds the Godot GDExtension so the editor never loads a stale DLL
# missing classes that engine/ has since added.
#
# Usage: scripts/build.sh [--no-godot] [--jobs N] [--suite core|retail|all]
#                         [--no-test | -R REGEX] [--cmake-arg ARG]...
#   --no-godot       skip the Godot addon bootstrap and the GDExtension build
#                    (library-only iteration; what CI's engine test job runs)
#   --jobs N         build/test parallelism (default: the machine's CPU count)
#   --no-test        build only; run no ctest
#   -R REGEX         run only the suite's ctests matching REGEX; a partial run
#                    skips the suite attestation (scripts/ci/test_suites.py),
#                    which needs every selected test in the report
#   --cmake-arg ARG  pass ARG to the root configure (repeatable), e.g.
#                    -DBUILD_NOVAWORLD_HTTP=ON as CI's Linux job does
#
# The root tree configures once: later runs go straight to the build, which
# re-runs CMake by itself whenever a CMakeLists.txt changes. A run given
# --cmake-arg configures again to apply it.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
build_godot=1
suite=all
run_tests=1
test_regex=""
cmake_args=()
usage="usage: scripts/build.sh [--no-godot] [--jobs N] [--suite core|retail|all] [--no-test | -R REGEX] [--cmake-arg ARG]..."

while [[ $# -gt 0 ]]; do
    case "$1" in
        --suite)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            suite="$2"; shift 2 ;;
        --suite=*) suite="${1#--suite=}"; shift ;;
        --no-godot) build_godot=0; shift ;;
        --jobs)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            jobs="$2"; shift 2 ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        --no-test) run_tests=0; shift ;;
        -R)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            test_regex="$2"; shift 2 ;;
        --cmake-arg)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            cmake_args+=("$2"); shift 2 ;;
        --cmake-arg=*) cmake_args+=("${1#--cmake-arg=}"); shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done

case "$suite" in
    core) unset OPENNOVA_JO_DIR OPENNOVA_JO_ASSETS ;;
    retail) python "$root/scripts/ci/test_suites.py" --suite retail --require-roots ;;
    all) ;;
    *) echo "$usage" >&2; exit 2 ;;
esac

# Keep a CL=/MP16 (MSVC's extra-options variable) intact through Git Bash's
# POSIX-path rewrite of the environment (scripts/build_godot.sh says why).
export MSYS2_ENV_CONV_EXCL="CL${MSYS2_ENV_CONV_EXCL:+;$MSYS2_ENV_CONV_EXCL}"

# The Godot addons (GUT, imgui-godot) are project assets for the GDExtension
# flavour, not a dependency of the C++ targets: the Godot-free path skips the
# bootstrap.
if [[ "$build_godot" == "1" ]]; then
    "$root/scripts/bootstrap_godot.sh"
fi

echo "Building opennova libraries and tests..."
if [[ ! -f "$root/build/CMakeCache.txt" || ${#cmake_args[@]} -gt 0 ]]; then
    cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release "${cmake_args[@]}"
fi
cmake --build "$root/build" --config Release -j "$jobs"

selection=()
case "$suite" in
    core) selection=(-LE '^retail$') ;;
    retail) selection=(-L '^retail$') ;;
esac
if [[ "$run_tests" == "0" ]]; then
    echo "Skipping tests (--no-test)."
elif [[ -n "$test_regex" ]]; then
    echo "Running $suite tests matching '$test_regex' (no suite attestation)..."
    ctest --test-dir "$root/build" --output-on-failure -C Release --parallel "$jobs" \
      -R "$test_regex" "${selection[@]}"
else
    echo "Running $suite tests..."
    mkdir -p "$root/build/Testing"
    inventory="$root/build/Testing/$suite-inventory.json"
    report="$root/build/Testing/ctest-$suite.xml"
    python "$root/scripts/ci/test_suites.py" --suite "$suite" --build "$root/build" --inventory "$inventory"
    # Preserve full passing output so missing compatibility legs cannot hide
    # beyond CTest's default truncation limit.
    ctest --test-dir "$root/build" --output-on-failure -C Release --parallel "$jobs" \
      --test-output-size-passed 1048576 --test-output-size-failed 1048576 \
      --output-junit "$report" "${selection[@]}"
    python "$root/scripts/ci/test_suites.py" --suite "$suite" --inventory "$inventory" --report "$report"
fi

if [[ "$build_godot" == "1" ]]; then
    "$root/scripts/build_godot.sh" --jobs "$jobs"
fi

echo "Build and tests completed successfully."
