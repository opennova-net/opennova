#!/usr/bin/env bash
# Runs the GDScript test suite via GUT, headless.
#
# Expects $GODOT_BIN to point at Godot 4.6.1. Falls back to the binary
# at .godot-bin/ if one isn't set.
#
# Usage: scripts/test_godot.sh [--keep-user-dir]
#   --keep-user-dir  leave the run's isolated user:// (.godot-test-user) in
#                    place to inspect what the suite wrote
#
# The suite runs against an ISOLATED user:// (see below). Tests that persist
# settings or drop scratch files therefore cannot reach the developer's real
# Godot user directory -- a run must never change which resource directory ONED
# or the game opens next time, nor leave debris behind.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
keep_user_dir=0
for arg in "$@"; do
  case "$arg" in
    --keep-user-dir) keep_user_dir=1 ;;
    *) echo "usage: scripts/test_godot.sh [--keep-user-dir]" >&2; exit 2 ;;
  esac
done

"$root/scripts/bootstrap_godot.sh"

source "$root/scripts/godot_bin.sh"
resolve_godot_bin "$root"

if [[ -z "${GODOT_BIN:-}" || ! -x "$GODOT_BIN" ]]; then
  echo "error: set GODOT_BIN to a Godot 4.6.1 binary (or drop one in .godot-bin/)" >&2
  exit 1
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# --- isolate user:// ----------------------------------------------------------
# Godot derives user:// from the platform data dir ($APPDATA on Windows,
# $XDG_DATA_HOME/$HOME elsewhere). Point those at a scratch dir for the run so a
# test physically cannot write the developer's real config.
#
# This is not hypothetical: the suite had leaked settings and scratch files into
# the real user dir for a long time -- persisted resource roots, an MCP
# enabled/port pair that then failed a later run's defaults assertion, plus
# mission_authoring_rt_*.bms, render-fixture-* trees, test_perf_overlay_*.cfg,
# test_sbf_*.sbf and more. Per-test snapshot/restore only ever covered the files
# somebody remembered; this covers all of them, including files added later.
#
# Pass --keep-user-dir to inspect what a run wrote.
user_dir="$root/.godot-test-user"
rm -rf "$user_dir"
mkdir -p "$user_dir"
export APPDATA="$user_dir"
export XDG_DATA_HOME="$user_dir"
export HOME="$user_dir"
if [[ "$keep_user_dir" == "0" ]]; then
  trap 'rm -f "$log"; rm -rf "$user_dir"' EXIT
fi

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

exit "$status"
