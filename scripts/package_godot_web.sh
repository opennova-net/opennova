#!/usr/bin/env bash
# Exports the OpenNova Web preset and stages the static site game.<domain>
# serves (ADR 0049): the Godot export (index.html from godot/web/shell.html,
# the engine, the PCK and the GDExtension side module) plus the bundled
# assets/ the page copies into the engine's filesystem before it starts,
# listed in assets/manifest.json.
#
# Needs, in this order:
#   - the web side module:  scripts/build_godot_web.sh [--release]
#   - a native GDExtension for the exporting editor to load (so it resolves the
#     engine's classes while importing): scripts/build_godot.sh, or the Linux
#     build the image uses
#   - Godot 4.6.1 ($GODOT_BIN or .godot-bin/, scripts/godot_bin.sh) with its
#     web_dlink_* export templates installed
#   - the shipped assets/ pulled from LFS (an unpulled pointer is refused)
# deploy/game/Dockerfile runs all of it; this script is the export leg.
#
# Usage: scripts/package_godot_web.sh [--debug] [--out DIR]
#   (default)  -> --export-release with the template_release side module
#   --debug    -> --export-debug with the template_debug side module
#   --out DIR  -> the site directory (default: godot/exports/web)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="release"
out="$root/godot/exports/web"
usage="usage: scripts/package_godot_web.sh [--debug] [--out DIR]"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) mode="debug"; shift ;;
        --out)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            out="$2"; shift 2 ;;
        --out=*) out="${1#--out=}"; shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# The files of assets/ the site ships. The on_ar15/on_arms art is about 540 MB
# of textures and nothing references it yet (assets/README.md); every visitor
# would download it before the menu. Drop a pattern here once the game uses
# that art.
web_asset_excludes=("README.md" "project.opennova" "on_ar15*" "on_arms*")

source "$root/scripts/godot_bin.sh"
resolve_godot_bin "$root"
[[ -n "${GODOT_BIN:-}" ]] || { echo "no Godot 4.6.1 binary: set GODOT_BIN (scripts/godot_bin.sh)" >&2; exit 1; }

side_module="$root/godot/bin/libopennova.web.template_$mode.wasm32.wasm"
[[ -f "$side_module" ]] || {
    echo "missing $side_module: run scripts/build_godot_web.sh$([[ $mode == release ]] && echo ' --release')" >&2
    exit 1
}

echo "Importing the project..."
"$GODOT_BIN" --headless --path "$root/godot" --import

echo "Exporting OpenNova Web ($mode) to $out..."
"$GODOT_BIN" --headless --path "$root/godot" --export-"$mode" "OpenNova Web" "$out/index.html"
for file in index.html index.js index.wasm index.pck "$(basename "$side_module")"; do
    [[ -s "$out/$file" ]] || { echo "the export is missing $file" >&2; exit 1; }
done

echo "Staging assets/..."
rm -rf "$out/assets"
mkdir -p "$out/assets"
shipped=()
for path in "$root"/assets/*; do
    name="$(basename "$path")"
    [[ -f "$path" ]] || continue
    skip=0
    for pattern in "${web_asset_excludes[@]}"; do
        # shellcheck disable=SC2053 # the pattern is a glob on purpose
        [[ "$name" == $pattern ]] && skip=1
    done
    (( skip )) && continue
    if head -c 64 "$path" | grep -q "^version https://git-lfs"; then
        echo "assets/$name is an unpulled LFS pointer: git lfs pull --include=assets/$name" >&2
        exit 1
    fi
    cp "$path" "$out/assets/$name"
    shipped+=("$name")
done
(( ${#shipped[@]} > 0 )) || { echo "no assets/ file to ship" >&2; exit 1; }
{
    printf '{"files": ['
    for index in "${!shipped[@]}"; do
        (( index > 0 )) && printf ', '
        printf '"%s"' "${shipped[$index]}"
    done
    printf ']}\n'
} > "$out/assets/manifest.json"

echo "Staged the web site in $out (${#shipped[@]} bundled assets)."
