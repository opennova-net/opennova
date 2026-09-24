#!/usr/bin/env bash
# Package the Blender .3di exporter: build opennova-3di (Release) and zip the
# add-on with the CLI in its bin/ directory, ready for Blender's
# Preferences > Add-ons > Install from Disk.
#
#   scripts/package_blender_addon.sh [out.zip]   (default build/opennova_3di.zip)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/opennova_3di.zip}"
build="$root/build/blender-addon"

# Keep the package's static MSVC runtime separate from development builds.
# The installed add-on must not require a separately installed VC++ runtime.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded > /dev/null
cmake --build "$build" --config Release --target opennova_3di

exe="$build/apps/threedi_cli/Release/opennova-3di.exe"
[ -f "$exe" ] || exe="$build/apps/threedi_cli/opennova-3di.exe"
[ -f "$exe" ] || { echo "opennova-3di was not built" >&2; exit 1; }

python - "$root/tools/blender/opennova_3di" "$exe" "$out" <<'EOF'
import os, sys, zipfile
src, exe, out = sys.argv[1:4]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        path = os.path.join(src, name)
        if os.path.isfile(path) and (name.endswith(".py") or name.endswith(".toml")):
            z.write(path, "opennova_3di/" + name)
    z.write(exe, "opennova_3di/bin/opennova-3di.exe")
print("wrote", out)
EOF
