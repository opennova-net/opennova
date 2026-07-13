#!/usr/bin/env bash
# Runs the GDScript test suite via GUT, headless.
#
# Expects $GODOT_BIN to point at Godot 4.6.1. Falls back to the binary
# at .godot-bin/ if one isn't set.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$root/scripts/bootstrap_godot.sh"

if [[ -z "${GODOT_BIN:-}" ]]; then
  for candidate in \
    "$root/.godot-bin/Godot_v4.6.1-stable_win64_console.exe" \
    "$root/.godot-bin/Godot_v4.6.1-stable_win64.exe" \
    "$root/.godot-bin/Godot_v4.6.1-stable_linux.x86_64" \
    "$root/.godot-bin/Godot_v4.6.1-stable_macos.universal"
  do
    if [[ -x "$candidate" ]]; then
      GODOT_BIN="$candidate"
      break
    fi
  done
fi

if [[ -z "${GODOT_BIN:-}" || ! -x "$GODOT_BIN" ]]; then
  echo "error: set GODOT_BIN to a Godot 4.6.1 binary (or drop one in .godot-bin/)" >&2
  exit 1
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

if [[ "${OPENNOVA_GODOT_PROBE_ONLY:-0}" != "1" ]]; then
set +e
"$GODOT_BIN" --headless --path "$root/godot" \
  -s addons/gut/gut_cmdln.gd \
  -gdir=res://tests \
  -ginclude_subdirs \
  -gprefix= \
  -gsuffix=_test.gd \
  -gexit 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

# GUT exits 0 even when scripts fail to parse (e.g. missing GDExtension
# classes), because unparseable scripts are silently dropped from the
# collector rather than counted as failures. Catch that here.
if grep -qE 'SCRIPT ERROR: Parse Error' "$log"; then
  echo "error: GDScript parse errors detected during test collection" >&2
  exit 1
fi

# A *_test.gd that references an unregistered GDExtension class (e.g. a stale
# DLL on a fresh worktree) often loads WITHOUT a "Parse Error" line, fails GUT's
# top-level "extends GutTest" check, and is dropped from collection with only a
# warning -- so the suite stays green while silently skipping the test. Helper
# inner classes inside an accepted GutTest script are allowed; GUT reports them
# separately, and the outer script still runs. Every *_test.gd we ship extends
# GutTest, so any top-level script drop is a real defect, not an intentional
# non-test file. (See third_party/gut test_collector.gd add_script.)
if grep -qE 'Ignoring script .*does not extend GutTest' "$log"; then
  echo "error: a test script was dropped from collection (parse error or unregistered class):" >&2
  grep -E 'Ignoring script .*does not extend GutTest' "$log" >&2
  exit 1
fi

if [[ "$status" -ne 0 ]]; then
  exit "$status"
fi
fi

# GUT exercises NovaLaunchFlags.parse with injected arrays. These separate
# processes pin both real invocation forms: public options passed directly to
# an exported game and ONED's private handoff after Godot's separator.
run_launch_probe() {
  local mode="$1"
  shift
  echo "=== Runtime launch-argument probe: $mode ==="
  set +e
  # Git Bash/MSYS otherwise rewrites retail-style /D and /EXP tokens as
  # Windows paths. Other arguments (notably --path) keep normal conversion.
  OPENNOVA_RUNTIME_LAUNCH_ARGS_MODE="$mode" \
  MSYS2_ARG_CONV_EXCL='/D;/EXP;/game' \
  "$GODOT_BIN" --headless --path "$root/godot" \
    -s res://tests/runtime_launch_args_probe.gd "$@" 2>&1 | tee "$log"
  local probe_status=${PIPESTATUS[0]}
  set -e

  if [[ "$probe_status" -ne 0 ]]; then
    echo "error: $mode runtime launch-argument probe exited with status $probe_status" >&2
    exit "$probe_status"
  fi
  if grep -qE 'SCRIPT ERROR: Parse Error|Failed to load script' "$log"; then
    echo "error: $mode runtime launch-argument probe logged a script load failure" >&2
    exit 1
  fi
  if ! grep -qF "OPENNOVA_RUNTIME_LAUNCH_ARGS_PROBE: OK ($mode)" "$log"; then
    echo "error: $mode runtime launch-argument probe did not report success" >&2
    exit 1
  fi
}

run_launch_probe direct /D /game jodemo /EXP revx02
run_launch_probe oned -- /D /game jodemo /EXP revx02 \
  --oned-resource-root "C:/OpenNova Loose Root"

exit 0
