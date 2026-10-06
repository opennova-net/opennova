#!/usr/bin/env bash
# Package the Blender .3di and animation add-on: build opennova-3di (Release)
# and zip the add-on with the CLI in its bin/ directory, ready for Blender's
# Edit > Preferences > Get Extensions > Install from Disk.
#
#   scripts/package_blender_addon.sh [out.zip] [version]
#
# out.zip defaults to build/opennova_3di.zip. A version (a release tag such as
# v0.0.10; the leading v is dropped) is stamped into the zipped
# blender_manifest.toml, which Blender reports as the add-on's version; the
# tracked manifest is left as it is. Without one the zip keeps the tracked
# manifest's version.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/opennova_3di.zip}"
version="${2:-}"
version="${version#v}"
build="$root/build/blender-addon"

# Keep the package's static MSVC runtime separate from development builds.
# The installed add-on must not require a separately installed VC++ runtime.
# Weapon timing links the runtime FSM too, and `texture` the editor core's
# image import; keep the unrelated game dev tools and the editor's windows
# (and the Dear ImGui fetch they bring) off.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPENNOVA_DEVTOOLS=OFF -DOPENNOVA_EDITOR=OFF > /dev/null
cmake --build "$build" --config Release --target opennova_3di

exe="$build/apps/3di/Release/opennova-3di.exe"
[ -f "$exe" ] || exe="$build/apps/3di/opennova-3di.exe"
[ -f "$exe" ] || { echo "opennova-3di was not built" >&2; exit 1; }

python - "$root/tools/blender/opennova_3di" "$exe" "$out" "$version" <<'EOF'
import os, re, sys, zipfile
src, exe, out, version = sys.argv[1:5]
# Blender's own manifest check (bl_pkg/cli/blender_ext.py RE_MANIFEST_SEMVER).
semver = re.compile(
    r'^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)'
    r'(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?'
    r'(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$')
if version and not semver.match(version):
    sys.exit(f"version {version!r} is not a semantic version Blender accepts")
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        path = os.path.join(src, name)
        if not os.path.isfile(path):
            continue
        if name == "blender_manifest.toml" and version:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            text, n = re.subn(r'(?m)^version = "[^"]*"$', f'version = "{version}"', text)
            if n != 1:
                sys.exit(f"{path}: expected one version line, found {n}")
            z.writestr("opennova_3di/" + name, text)
        elif name.endswith(".py") or name.endswith(".toml"):
            z.write(path, "opennova_3di/" + name)
    z.write(exe, "opennova_3di/bin/opennova-3di.exe")
print("wrote", out, f"(version {version})" if version else "")
EOF
