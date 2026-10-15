#!/usr/bin/env bash
# Package the native apps under apps/ for Windows: build every app (Release)
# and zip the executables flat, with opennova-serve's example host file beside
# them and a README.txt naming each tool. The NovaWorld server is not one of
# them: it is Linux-only and ships as its container image (deploy/).
#
#   scripts/package_apps.sh [out.zip]
#
# out.zip defaults to build/opennova-apps-windows.zip.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/opennova-apps-windows.zip}"
build="$root/build/apps-package"

# The apps/ directory of each shipped executable. An app in apps/<dir> builds
# target opennova_<dir> as opennova-<dir, kebab-cased> (DEVELOPING.md).
apps=(3di extract lan_probe parity serve update_tool wire)

# The static MSVC runtime keeps the zip free of a VC++ runtime install, and a
# separate build tree keeps it apart from development builds. No app links the
# dev tools or the editor's windows, so they (and the Dear ImGui fetch they
# bring) stay off.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPENNOVA_DEVTOOLS=OFF \
    -DOPENNOVA_EDITOR=OFF > /dev/null
targets=()
for dir in "${apps[@]}"; do targets+=("opennova_$dir"); done
cmake --build "$build" --config Release --target "${targets[@]}"

exes=()
for dir in "${apps[@]}"; do
    name="opennova-${dir//_/-}"
    exe="$build/apps/$dir/Release/$name.exe"
    [ -f "$exe" ] || exe="$build/apps/$dir/$name.exe"
    [ -f "$exe" ] || { echo "$name was not built" >&2; exit 1; }
    exes+=("$exe")
done

python - "$root" "$out" "${exes[@]}" <<'EOF'
import os, sys, zipfile
root, out, exes = sys.argv[1], sys.argv[2], sys.argv[3:]
readme = """\
OpenNova apps for Windows x64 (pre-1.0, experimental)

Command-line tools and the headless game server, built from
https://github.com/opennova-net/opennova. No game data is included. Each
tool prints its usage when run with no arguments or --help. The NovaWorld
server runs on Linux only: self-hosters run its container stack (deploy/ in
the repository), on Windows through Docker Desktop.

opennova-3di.exe
    Builds a .3di model from .o3d scene text and writes one back as text;
    inspects and compares models. "opennova-3di anim" does the same for
    .adm/.bad animations through .o3a text. The Blender add-on uses it.

opennova-extract.exe
    Writes named entries out of a Joint Operations install, mounted the way
    the game mounts it (its .pff archives, expansion and loose overrides).

opennova-lan-probe.exe
    Waits for a LAN game host to answer the discovery probe and reports
    whether it is ready. It never joins the game.

opennova-parity.exe
    The retail-LAN parity verdict: compares the run folders of one parity
    case (retail against retail twice, then OpenNova as client, host or
    both) and judges each wire metric against the retail-vs-retail noise.
    The runs come from the repository's scripts/net/run_parity_suite.ps1.

opennova-serve.exe
    The headless game server: the game's Serve Only host with no window,
    configured by a host file, retail's dedicated-server format
    (example.host documents every key). Run
    "opennova-serve --resource-dir <game folder> /HOST example.host".
    It answers LAN browsers and admits joiners on the retail LAN port
    range and plays the host file's map rotation. It keeps game.cfg in
    the folder it runs from; a nonzero remote_admin_port there opens
    retail's remote-admin console (users in admin.cfg, beside it).

opennova-update-tool.exe
    Builds a packed update (.sfs) from a product's baseline install and
    the install to update it to, inspects one, and serves packed updates
    over NovaLogic's updater protocol, so a stock UPDATE.EXE (or OpenNova's)
    updates from it: "opennova-update-tool build --from <baseline> --to
    <new> --out update.sfs --base JOPTR.CMD [--expansion jox01=JOEPTR.CMD]".

opennova-wire.exe
    Decodes NovaWorld in-game traffic from a .pcap/.pcapng capture, a .sph
    server log or a hexcap text file.
"""
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("README.txt", readme.replace("\n", "\r\n"))
    for exe in exes:
        z.write(exe, os.path.basename(exe))
    z.write(os.path.join(root, "apps", "serve", "example.host"), "example.host")
print("wrote", out)
EOF
