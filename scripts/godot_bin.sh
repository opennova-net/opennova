#!/usr/bin/env bash
# The one Godot-binary resolver for the sh-side scripts: honors $GODOT_BIN,
# else picks the first runnable binary under .godot-bin/ in the repo root or
# in any parent directory (a linked worktree under .claude/worktrees/ borrows
# the main checkout's copy). Source this, then rely on $GODOT_BIN (empty = not
# found; the caller owns the error message).
# The PowerShell twin lives in scripts/net/lib.ps1 and the Python twin in
# scripts/mcp/game_mcp.py GODOT_BINARIES (cross-language boundary — kept in
# sync by hand, console binary first, the same parent walk; change all three).
resolve_godot_bin() {
  local root="$1"
  if [[ -n "${GODOT_BIN:-}" ]]; then
    return 0
  fi
  local dir="$root" parent candidate name
  while :; do
    for name in "Godot_v4.6.1-stable_win64_console.exe" "Godot_v4.6.1-stable_win64.exe" \
                "Godot_v4.6.1-stable_linux.x86_64" "Godot_v4.6.1-stable_macos.universal"
    do
      candidate="$dir/.godot-bin/$name"
      if [[ -x "$candidate" ]]; then
        GODOT_BIN="$candidate"
        return 0
      fi
    done
    parent="$(dirname "$dir")"
    if [[ "$parent" == "$dir" ]]; then
      break
    fi
    dir="$parent"
  done
  return 0
}
