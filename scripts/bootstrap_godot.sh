#!/usr/bin/env bash
# Installs the GUT plugin into godot/addons/gut/ by copying from the
# third_party/gut submodule. The copy destination is gitignored;
# regenerate it by re-running this script whenever the submodule is bumped.
#
# Do not edit godot/addons/gut/** directly — changes will be overwritten.
# Upstream edits belong in the submodule itself.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/third_party/gut"
dst="$root/godot/addons/gut"

git -C "$root" submodule update --init --recursive -- "third_party/gut"

if [[ ! -f "$src/addons/gut/plugin.cfg" ]]; then
  echo "error: GUT submodule missing expected plugin.cfg at $src/addons/gut/" >&2
  exit 1
fi

# Copy only when a submodule file is missing or differs: a fresh copy rewrites
# every file time, which sends the Godot editor rescanning the addon. Files the
# editor adds under the copy (none today) do not count as a difference.
stale="$(diff -rq "$src/addons/gut" "$dst" 2>&1 | grep -v "^Only in $dst" || true)"
if [[ -n "$stale" ]]; then
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -r "$src/addons/gut/." "$dst/"
  verb=installed
else
  verb="up to date"
fi

echo "GUT plugin $verb at godot/addons/gut (version: $(grep '^version=' "$dst/plugin.cfg" | cut -d'"' -f2))"

# The imgui-godot addon (the Dear ImGui bridge for the in-engine dev tools, ADR 0039)
# rides the same bootstrap; it has its own script so the packaging job can install
# only it (an export must never carry GUT).
"$root/scripts/bootstrap_imgui_godot.sh"
