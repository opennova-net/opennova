#!/usr/bin/env bash
# Regenerate the README editor screenshots.
#
# Boots the OpenNova Editor (ONED) via the capture driver scene, which switches
# through each workspace, opens a representative asset from the resource dir, and
# writes PNGs to <repo>/screenshots/ (LFS-tracked). Re-run after UI changes to
# keep the README screenshots in sync with the editor.
#
# Requires a real rendering window (NOT --headless), so run on a desktop session.
#
# Expects $GODOT_BIN to point at Godot 4.6.1. Falls back to the binary in
# .godot-bin/ if one isn't set.
#
# Assets are opened by name from $NOVA_RESOURCE_DIR (the external resource dir
# that also feeds the runtime/editor). Defaults to ~/Desktop/revx02; override by
# exporting NOVA_RESOURCE_DIR. The dir must hold Dvxi5.trn (+ textures),
# full_00.env, the font, credits, the strings table, and the object .3di.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

: "${NOVA_RESOURCE_DIR:=$HOME/Desktop/revx02}"
if [[ ! -d "$NOVA_RESOURCE_DIR" ]]; then
  echo "error: NOVA_RESOURCE_DIR '$NOVA_RESOURCE_DIR' is not a directory" >&2
  exit 1
fi
export NOVA_RESOURCE_DIR

if [[ -z "${GODOT_BIN:-}" ]]; then
  for candidate in \
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

"$GODOT_BIN" --path "$root/godot" --resolution 1600x900 \
  res://modtools/tools/screenshot_capture.tscn

echo "Screenshots written to $root/screenshots"
