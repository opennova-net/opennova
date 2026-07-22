#!/usr/bin/env bash
# Regenerate the README editor and importer screenshots.
#
# Boots the OpenNova Editor (ONED) via the capture driver scene, which switches
# through each workspace, opens a representative asset from the resource dir, and
# writes PNGs to <repo>/screenshots/ (LFS-tracked). Then captures the standalone
# importer UI through its deterministic Qt screenshot driver. Re-run after UI
# changes to keep the README screenshots in sync with the tools.
#
# Requires a real rendering window (NOT --headless), so run on a desktop session.
#
# Expects $GODOT_BIN to point at Godot 4.6.1. Falls back to the binary in
# .godot-bin/ if one isn't set.
#
# Assets are opened by name from $NOVA_RESOURCE_DIR (the external resource dir
# that also feeds the runtime/editor). Defaults to ~/Desktop/JOX; override by
# exporting NOVA_RESOURCE_DIR. The dir must hold Dvxi5.trn (+ textures),
# full_00.env, the font, credits, the strings table, and the object .3di.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

find_godot_bin() {
  local dir="$1"
  while [[ -n "$dir" && "$dir" != "/" ]]; do
    for candidate in \
      "$dir/.godot-bin/Godot_v4.6.1-stable_win64_console.exe" \
      "$dir/.godot-bin/Godot_v4.6.1-stable_win64.exe" \
      "$dir/.godot-bin/Godot_v4.6.1-stable_linux.x86_64" \
      "$dir/.godot-bin/Godot_v4.6.1-stable_macos.universal"
    do
      if [[ -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
    dir="$(dirname "$dir")"
  done
  return 1
}

: "${NOVA_RESOURCE_DIR:=$HOME/Desktop/JOX}"
if [[ ! -d "$NOVA_RESOURCE_DIR" ]]; then
  echo "error: NOVA_RESOURCE_DIR '$NOVA_RESOURCE_DIR' is not a directory" >&2
  exit 1
fi
export NOVA_RESOURCE_DIR

if [[ -z "${GODOT_BIN:-}" ]]; then
  GODOT_BIN="$(find_godot_bin "$root" || true)"
fi

if [[ -z "${GODOT_BIN:-}" || ! -x "$GODOT_BIN" ]]; then
  echo "error: set GODOT_BIN to a Godot 4.6.1 binary (or drop one in .godot-bin/)" >&2
  exit 1
fi

editor_screenshots=(
  overview.png
  object.png
  avatars.png
  mission.png
  fonts.png
  credits.png
  strings.png
  menus.png
  hud.png
  music.png
  particles.png
  sound.png
  environment.png
)

"$GODOT_BIN" --path "$root/godot" --import

capture_stamp="$(mktemp)"
touch "$capture_stamp"
trap 'rm -f "$capture_stamp"' EXIT

"$GODOT_BIN" --path "$root/godot" --resolution 1600x900 \
  res://modtools/tools/screenshot_capture.tscn

for name in "${editor_screenshots[@]}"; do
  path="$root/screenshots/$name"
  if [[ ! -s "$path" ]]; then
    echo "error: editor screenshot '$name' was not written" >&2
    exit 1
  fi
  if [[ ! "$path" -nt "$capture_stamp" ]]; then
    echo "error: editor screenshot '$name' was not refreshed" >&2
    exit 1
  fi
done

(
  cd "$root"
  uv run python -m apps.importer.ui.screenshot_capture \
    --output "$root/screenshots/importer.png"
)

echo "Screenshots written to $root/screenshots"
