#!/usr/bin/env bash
# Runs the GDScript test suite via GUT, headless unless --windowed is selected.
#
# Expects $GODOT_BIN to point at Godot 4.6.1. Falls back to the binary
# at .godot-bin/ if one isn't set.
#
# Usage: scripts/test_godot.sh [--keep-user-dir] [--suite core|retail|all] [--windowed]
#   --keep-user-dir  leave the run's isolated user:// (.godot-test-user) in
#                    place to inspect what the suite wrote
#   --windowed       run only the suite's graphics scripts (tests/windowed/ for
#                    core, tests/retail/windowed/ for retail) under Forward+;
#                    a pending test there (no RenderingDevice) fails the run
#
# The suite runs against an ISOLATED user:// (see below). Tests that persist
# settings or drop scratch files therefore cannot reach the developer's real
# Godot user directory -- a run must never change the game's player preferences, nor leave debris behind.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
keep_user_dir=0
suite=all
windowed=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep-user-dir) keep_user_dir=1; shift ;;
    --windowed) windowed=1; shift ;;
    --suite) [[ $# -ge 2 ]] || exit 2; suite="$2"; shift 2 ;;
    --suite=*) suite="${1#--suite=}"; shift ;;
    *) echo "usage: scripts/test_godot.sh [--keep-user-dir] [--suite core|retail|all] [--windowed]" >&2; exit 2 ;;
  esac
done
case "$suite" in
  core) unset OPENNOVA_JO_DIR OPENNOVA_JO_ASSETS ;;
  retail) python "$root/scripts/ci/test_suites.py" --suite retail --require-roots ;;
  all) ;;
  *) echo "unknown suite: $suite" >&2; exit 2 ;;
esac

label="$suite"
display=(--headless)
selection=()
if [[ "$windowed" == "1" ]]; then
  label="$suite-windowed"
  display=(--rendering-method forward_plus)
  selection=(--windowed)
fi

"$root/scripts/bootstrap_godot.sh"

source "$root/scripts/godot_bin.sh"
resolve_godot_bin "$root"

if [[ -z "${GODOT_BIN:-}" || ! -x "$GODOT_BIN" ]]; then
  echo "error: set GODOT_BIN to a Godot 4.6.1 binary (or drop one in .godot-bin/)" >&2
  exit 1
fi

log="$(mktemp)"
trap 'rm -f "$log"; rm -rf "$root/.godot-test-fixtures"' EXIT

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
  trap 'rm -f "$log"; rm -rf "$user_dir" "$root/.godot-test-fixtures"' EXIT
fi

# --- the class cache ------------------------------------------------------------
# A headless run resolves a class_name only through the class cache an import
# writes. After a base change brings a new class_name (or moves one), the
# scripts naming it fail to parse and GUT drops them until the project is
# imported again; the cache is checked against the scripts and the project
# imported when it is stale (inside the isolated user://, like the run).
if ! python "$root/scripts/godot_class_cache.py" "$root/godot"; then
  echo "test_godot: the class cache is stale; importing the project" >&2
  "$GODOT_BIN" --headless --path "$root/godot" --import >/dev/null 2>&1 || true
  if ! python "$root/scripts/godot_class_cache.py" "$root/godot"; then
    echo "error: the class cache is still stale after an import" >&2
    exit 1
  fi
fi

reports="$root/build/Testing"
mkdir -p "$reports"
config="$reports/gut-$label.json"
inventory="$reports/gut-$label-inventory.json"
report="$reports/gut-$label.xml"
python "$root/scripts/ci/test_suites.py" --suite "$suite" --godot-config "$config" \
  --inventory "$inventory" --report "$report" "${selection[@]}"
# Do not let a previous successful run stand in for a failed collector.
rm -f "$report"

# A windowed run starts behind every other window and never takes the foreground
# (game_mcp.py run; docs/mcp.md "Launching").
behind=()
if [[ "$windowed" == "1" ]]; then
  behind=(python "$root/scripts/mcp/game_mcp.py" run --)
fi
# The game's own files (the player profile's player.sav and weapon.sav) live in
# the directory it runs in (LaunchFlags.working_dir), for a GUT run the project
# (Godot's --path makes it the process's working directory; GUT refuses a
# --working-dir after its own options). A run starts without them and leaves
# none behind (git ignores them for a direct single-file run).
profile_files=("$root/godot/player.sav" "$root/godot/weapon.sav")
rm -f "${profile_files[@]}"
rm -f "$root"/godot/expansion/*/weapon.sav 2>/dev/null || true
set +e
${behind[@]+"${behind[@]}"} "$GODOT_BIN" "${display[@]}" --path "$root/godot" \
  -s addons/gut/gut_cmdln.gd -gconfig="$config" -gexit 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e
rm -f "${profile_files[@]}"
rm -f "$root"/godot/expansion/*/weapon.sav 2>/dev/null || true
rmdir "$root"/godot/expansion/* "$root/godot/expansion" 2>/dev/null || true

cp "$log" "$reports/gut-$label.log"

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

[[ "$status" == "0" ]] || exit "$status"
python "$root/scripts/ci/test_suites.py" --suite "$suite" --inventory "$inventory" \
  --report "$report" --gut-log "$log" "${selection[@]}"
