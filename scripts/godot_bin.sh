#!/usr/bin/env bash
# The one Godot-binary resolver for the sh-side scripts: honors $GODOT_BIN,
# else picks the first runnable binary under .godot-bin/. Source this, then
# rely on $GODOT_BIN (empty = not found; the caller owns the error message).
# The PowerShell twin lives in scripts/net/lib.ps1 (cross-language boundary —
# kept in sync by hand; change both).
resolve_godot_bin() {
  local root="$1"
  if [[ -n "${GODOT_BIN:-}" ]]; then
    return 0
  fi
  local candidate
  for candidate in     "$root/.godot-bin/Godot_v4.6.1-stable_win64_console.exe"     "$root/.godot-bin/Godot_v4.6.1-stable_win64.exe"     "$root/.godot-bin/Godot_v4.6.1-stable_linux.x86_64"     "$root/.godot-bin/Godot_v4.6.1-stable_macos.universal"
  do
    if [[ -x "$candidate" ]]; then
      GODOT_BIN="$candidate"
      return 0
    fi
  done
  return 0
}
