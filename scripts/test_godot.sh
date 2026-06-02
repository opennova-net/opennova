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

exit "$status"
