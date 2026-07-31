#!/usr/bin/env bash
# Build and export the OpenNova Godot applications for macOS (universal).
#
# Produces (version read from godot/project.godot):
#   dist/opennova-runtime-macos-v<version>.zip    (Runtime .app, universal)
#   dist/opennova-modtools-macos-v<version>.zip    (Mod Tools .app, universal)
#
# The apps are universal (arm64 + x86_64) and ad-hoc signed: they launch, but
# Gatekeeper flags them as from an unidentified developer. Bypass with a
# right-click -> Open, or `xattr -dr com.apple.quarantine <App>.app`. They are
# NOT notarized (CI has no Apple Developer certificate).
#
# Mirrors scripts/package_godot_windows.ps1. Requires a macOS machine with cmake +
# Xcode command line tools, and initialised submodules (third_party/godot-cpp).
#
# Usage: scripts/package_godot_macos.sh [all|editor|runtime]
set -euo pipefail

TARGET="${1:-all}"
case "$TARGET" in
  all|editor|runtime) ;;
  *)
    echo "error: target must be one of: all, editor, runtime" >&2
    exit 2
    ;;
esac
if [[ "$#" -gt 1 ]]; then
  echo "error: expected at most one target argument" >&2
  exit 2
fi

PACKAGE_EDITOR=0
PACKAGE_RUNTIME=0
case "$TARGET" in
  all) PACKAGE_EDITOR=1; PACKAGE_RUNTIME=1 ;;
  editor) PACKAGE_EDITOR=1 ;;
  runtime) PACKAGE_RUNTIME=1 ;;
esac
echo "=== Godot macOS package target: $TARGET ==="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 3)}"

GODOT_VERSION="4.6.1-stable"
GODOT_VERSION_PLAIN="4.6.1"
GODOT_DIR="$ROOT/.godot-bin"
GODOT_APP="$GODOT_DIR/Godot.app"
GODOT_BIN="$GODOT_APP/Contents/MacOS/Godot"
GODOT_ZIP_URL="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/Godot_v${GODOT_VERSION}_macos.universal.zip"
TEMPLATES_URL="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/Godot_v${GODOT_VERSION}_export_templates.tpz"
TEMPLATES_DIR="$HOME/Library/Application Support/Godot/export_templates/${GODOT_VERSION_PLAIN}.stable"

DIST="$ROOT/dist"
mkdir -p "$DIST"

# ---------------------------------------------------------------------------
# Version: read config/version and stamp it into the macOS presets so the .app
# Info.plist carries a real CFBundleShortVersionString / CFBundleVersion.
# ---------------------------------------------------------------------------
VERSION="$(sed -n 's/^config\/version="\([^"]*\)".*/\1/p' "$ROOT/godot/project.godot" | head -n1)"
if [[ -z "$VERSION" ]]; then
  echo "error: could not read config/version from godot/project.godot" >&2
  exit 1
fi
echo "=== OpenNova Godot apps version: $VERSION ==="

PRESETS="$ROOT/godot/export_presets.cfg"
# application/short_version and application/version are macOS-only keys (Windows
# presets use file_version/product_version), so replacing every occurrence only
# touches the two macOS presets.
sed -i.bak \
  -e "s|application/short_version=\"[^\"]*\"|application/short_version=\"$VERSION\"|g" \
  -e "s|application/version=\"[^\"]*\"|application/version=\"$VERSION\"|g" \
  "$PRESETS"
rm -f "$PRESETS.bak"

# ---------------------------------------------------------------------------
# 1. Install the Godot editor (.app) if missing.
# ---------------------------------------------------------------------------
if [[ ! -x "$GODOT_BIN" ]]; then
  echo "=== Downloading Godot $GODOT_VERSION (macOS) ==="
  mkdir -p "$GODOT_DIR"
  curl -fL -o "$GODOT_DIR/godot.zip" "$GODOT_ZIP_URL"
  unzip -o "$GODOT_DIR/godot.zip" -d "$GODOT_DIR"
  rm -f "$GODOT_DIR/godot.zip"
  chmod +x "$GODOT_BIN"
fi
[[ -x "$GODOT_BIN" ]] || { echo "error: Godot binary missing: $GODOT_BIN" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 2. Install export templates if missing (the .tpz is all-platform).
# ---------------------------------------------------------------------------
if [[ ! -f "$TEMPLATES_DIR/version.txt" ]]; then
  echo "=== Downloading export templates ==="
  mkdir -p "$TEMPLATES_DIR"
  curl -fL -o "$GODOT_DIR/templates.tpz" "$TEMPLATES_URL"
  EXTRACT="$GODOT_DIR/templates-extract"
  rm -rf "$EXTRACT"
  unzip -o "$GODOT_DIR/templates.tpz" -d "$EXTRACT"
  # .tpz archives contain a top-level "templates" folder.
  cp -R "$EXTRACT/templates/." "$TEMPLATES_DIR/"
  rm -rf "$EXTRACT" "$GODOT_DIR/templates.tpz"
fi

# ---------------------------------------------------------------------------
# 3. Build GDExtension binaries.
#    - debug arm64: loaded by the (arm64) editor while it scans for the export.
#    - release universal: bundled into the exported .app, so it runs on both
#      Apple Silicon and Intel.
# ---------------------------------------------------------------------------
build_gdextension() {
  local target="$1" build_dir="$2" build_type="$3" expected="$4"; shift 4
  echo "=== Building $target GDExtension ($build_type) ==="
  cmake -S godot/engine -B "$build_dir" \
    "-DGODOTCPP_TARGET=$target" \
    "-DCMAKE_BUILD_TYPE=$build_type" \
    "$@"
  cmake --build "$build_dir" --config "$build_type" --target opennova -j "$JOBS"
  [[ -f "$expected" ]] || { echo "error: expected GDExtension missing: $expected" >&2; exit 1; }
}

DEBUG_DYLIB="$ROOT/godot/bin/libopennova.macos.template_debug.arm64.dylib"
RELEASE_DYLIB="$ROOT/godot/bin/libopennova.macos.template_release.universal.dylib"

if [[ "${SKIP_BUILD:-0}" == "1" ]]; then
  # CI builds the GDExtension once (the build-gdextension-macos job) and downloads
  # the dylibs into godot/bin; skip the per-job rebuild. Local runs leave SKIP_BUILD
  # unset and build normally.
  echo "=== SKIP_BUILD=1: using prebuilt GDExtension dylibs in godot/bin ==="
  [[ -f "$DEBUG_DYLIB"   ]] || { echo "error: expected prebuilt debug dylib missing: $DEBUG_DYLIB" >&2; exit 1; }
  [[ -f "$RELEASE_DYLIB" ]] || { echo "error: expected prebuilt release dylib missing: $RELEASE_DYLIB" >&2; exit 1; }
else
  build_gdextension template_debug   build-godot-debug   Debug   "$DEBUG_DYLIB"
  build_gdextension template_release build-godot-release Release "$RELEASE_DYLIB" \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
fi

echo "=== GDExtension binaries ==="
ls -la "$ROOT/godot/bin"
echo "release dylib archs: $(lipo -archs "$RELEASE_DYLIB" 2>/dev/null || echo '?')"

# ---------------------------------------------------------------------------
# 4. Prime the import cache (a cold headless export can fail on first import).
# ---------------------------------------------------------------------------
bash "$ROOT/scripts/bootstrap_godot.sh"
ok=0
for attempt in 1 2 3; do
  if "$GODOT_BIN" --headless --path godot --import; then ok=1; break; fi
  echo "::warning::Godot import attempt ${attempt} failed; retrying"
done
[[ "$ok" == "1" ]] || { echo "error: Godot import did not complete after 3 attempts" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 5. Export, ad-hoc sign, and zip each app.
# ---------------------------------------------------------------------------
export_app() {
  local preset="$1" app_path="$2" zip_path="$3"
  echo "=== Exporting $preset -> $app_path ==="
  rm -rf "$app_path"
  local log; log="$(mktemp)"
  if ! "$GODOT_BIN" --headless --path godot --export-release "$preset" "$app_path" >"$log" 2>&1; then
    cat "$log"; echo "error: Godot export of '$preset' failed" >&2; exit 1
  fi
  cat "$log"
  if grep -Eq "SCRIPT ERROR: Parse Error|GDExtension dynamic library not found|No loader found for resource|Failed to load script" "$log"; then
    echo "error: Godot export of '$preset' logged script or GDExtension load errors" >&2; exit 1
  fi
  rm -f "$log"
  [[ -d "$app_path" ]] || { echo "error: export produced no .app at $app_path" >&2; exit 1; }

  # Ad-hoc sign the whole bundle (incl. the embedded universal dylib). Required
  # on Apple Silicon, where unsigned executable code is killed on launch.
  echo "=== Ad-hoc signing $(basename "$app_path") ==="
  codesign --force --deep --sign - "$app_path"
  codesign --verify --deep --strict "$app_path" || echo "::warning::codesign verify reported issues"

  # ditto preserves bundle structure + signatures inside the zip.
  rm -f "$zip_path"
  ditto -c -k --keepParent "$app_path" "$zip_path"
  [[ -f "$zip_path" ]] || { echo "error: packaging produced no zip at $zip_path" >&2; exit 1; }
}

MODTOOLS_APP="$DIST/opennova-modtools.app"
RUNTIME_APP="$DIST/opennova.app"
MODTOOLS_ZIP="$DIST/opennova-modtools-macos-v$VERSION.zip"
RUNTIME_ZIP="$DIST/opennova-runtime-macos-v$VERSION.zip"

PRODUCED_ZIPS=()

if [[ "$PACKAGE_EDITOR" == "1" ]]; then
  export_app "OpenNova Mod Tools (macOS)" "$MODTOOLS_APP" "$MODTOOLS_ZIP"
  PRODUCED_ZIPS+=("$MODTOOLS_ZIP")
fi

if [[ "$PACKAGE_RUNTIME" == "1" ]]; then
  export_app "OpenNova Runtime (macOS)" "$RUNTIME_APP" "$RUNTIME_ZIP"
  PRODUCED_ZIPS+=("$RUNTIME_ZIP")
fi

echo "=== Done ==="
ls -la "${PRODUCED_ZIPS[@]}"
