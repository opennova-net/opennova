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

rm -rf "$dst"
mkdir -p "$dst"
cp -r "$src/addons/gut/." "$dst/"

echo "GUT plugin installed at godot/addons/gut (version: $(grep '^version=' "$dst/plugin.cfg" | cut -d'"' -f2))"
