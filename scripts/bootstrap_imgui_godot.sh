#!/usr/bin/env bash
# Installs the imgui-godot addon (the Dear ImGui bridge for the in-engine dev
# tools, ADR 0039) into godot/addons/imgui-godot/ from the pinned upstream
# release zip. The copy destination is gitignored; re-run this script to
# regenerate it. Called by scripts/bootstrap_godot.sh (which also installs GUT)
# and on its own by the packaging job, which must not install GUT.
#
# The pin is load-bearing: the addon's prebuilt GDExtension bundles one exact
# Dear ImGui commit (addons/imgui-godot/include/imgui-version.txt), and the
# engine's own ImGui copy (third_party/imgui/CMakeLists.txt) must be that same
# commit or ImGuiGD.GetImGuiPtrs rejects the context hand-off at runtime.
# Bump both together.
#
# Do not edit godot/addons/imgui-godot/** directly — changes will be overwritten.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

IMGUI_GODOT_VERSION="6.3.2"
IMGUI_VERSION="1.91.6"
IMGUI_GODOT_ZIP="imgui-godot-${IMGUI_GODOT_VERSION}_imgui-${IMGUI_VERSION}.zip"
IMGUI_GODOT_URL="https://github.com/pkdawson/imgui-godot/releases/download/v${IMGUI_GODOT_VERSION}/${IMGUI_GODOT_ZIP}"
IMGUI_GODOT_SHA256="ff02d8544cf5fe583212eb518fad4331006d46dfba354f327c3b0bec7125c468"

cache_dir="$root/.godot-bin"
zip_path="$cache_dir/$IMGUI_GODOT_ZIP"
extract_dir="$cache_dir/imgui-godot-extract"
dst="$root/godot/addons/imgui-godot"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    cmake -E sha256sum "$1" | cut -d' ' -f1
  fi
}

mkdir -p "$cache_dir"
if [[ ! -f "$zip_path" || "$(sha256_of "$zip_path")" != "$IMGUI_GODOT_SHA256" ]]; then
  echo "Downloading $IMGUI_GODOT_URL"
  curl -fsSL --retry 3 -o "$zip_path.part" "$IMGUI_GODOT_URL"
  mv -f "$zip_path.part" "$zip_path"
fi

actual="$(sha256_of "$zip_path")"
if [[ "$actual" != "$IMGUI_GODOT_SHA256" ]]; then
  echo "error: $IMGUI_GODOT_ZIP sha256 mismatch: expected $IMGUI_GODOT_SHA256, got $actual" >&2
  exit 1
fi

# cmake -E tar handles zips everywhere the build runs (Git Bash on the Windows
# runners has no unzip).
rm -rf "$extract_dir"
mkdir -p "$extract_dir"
(cd "$extract_dir" && cmake -E tar xf "$zip_path")

src="$extract_dir/imgui-godot-${IMGUI_GODOT_VERSION}/addons/imgui-godot"
if [[ ! -f "$src/plugin.cfg" ]]; then
  echo "error: $IMGUI_GODOT_ZIP does not contain addons/imgui-godot/plugin.cfg at the expected path" >&2
  exit 1
fi

rm -rf "$dst"
mkdir -p "$dst"
cp -r "$src/." "$dst/"
rm -rf "$extract_dir"

echo "imgui-godot addon installed at godot/addons/imgui-godot (version: $(grep '^version=' "$dst/plugin.cfg" | cut -d'"' -f2), imgui: $(tr -d '[:space:]' < "$dst/include/imgui-version.txt"))"
