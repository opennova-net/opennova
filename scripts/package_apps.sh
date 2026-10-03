#!/usr/bin/env bash
# Package the native apps under apps/ for Windows: build every app (Release)
# and zip the executables flat, with the NovaWorld server's migrations and seed
# beside them and a README.txt naming each tool.
#
#   scripts/package_apps.sh [out.zip]
#
# out.zip defaults to build/opennova-apps-windows.zip.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/opennova-apps-windows.zip}"
build="$root/build/apps-package"

# target:output-name:apps/ directory, one row per shipped executable.
apps=(
    opennova_3di:opennova-3di:threedi_cli
    opennova_extract:opennova-extract:extract
    nw_pp:nw_pp:nw_pp
    opennova_nw_lan_probe:nw-lan-probe:nw_lan_probe
    opennova_novaworld_server:opennova-novaworld:novaworld_server
)

# The static MSVC runtime keeps the zip free of a VC++ runtime install, and a
# separate build tree keeps it apart from development builds. No app links the
# dev tools or the editor's windows, so they (and the Dear ImGui fetch they
# bring) stay off. The server builds without BUILD_NOVAWORLD_HTTP: the HTTP
# portal ships only in the Linux container image.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPENNOVA_DEVTOOLS=OFF \
    -DOPENNOVA_EDITOR=OFF > /dev/null
targets=()
for row in "${apps[@]}"; do targets+=("${row%%:*}"); done
cmake --build "$build" --config Release --target "${targets[@]}"

exes=()
for row in "${apps[@]}"; do
    IFS=: read -r _ name dir <<< "$row"
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

Command-line tools and the NovaWorld server, built from
https://github.com/opennova-net/opennova. No game data is included. Each
tool prints its usage when run with no arguments or --help.

opennova-3di.exe
    Builds a .3di model from .o3d scene text and writes one back as text;
    inspects and compares models. "opennova-3di anim" does the same for
    .adm/.bad animations through .o3a text. The Blender add-on uses it.

opennova-extract.exe
    Writes named entries out of a Joint Operations install, mounted the way
    the game mounts it (its .pff archives, expansion and loose overrides).

nw_pp.exe
    Decodes NovaWorld in-game traffic from a .pcap/.pcapng capture, a .sph
    server log or a hexcap text file.

nw-lan-probe.exe
    Waits for a LAN game host to answer the discovery probe and reports
    whether it is ready. It never joins the game.

opennova-novaworld.exe
    The NovaWorld server: the gate (UDP 7597) and session (UDP 64206)
    listeners. The web portal and HTTP routes are only in the Linux container
    image. Run it from this folder: it applies backend\\migrations and
    backend\\seed and keeps its database under backend\\data. Settings are
    environment variables, listed in apps/novaworld_server/README.md in the
    repository.
"""
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("README.txt", readme.replace("\n", "\r\n"))
    for exe in exes:
        z.write(exe, os.path.basename(exe))
    for sub in ("migrations", "seed"):
        src = os.path.join(root, "backend", sub)
        for name in sorted(os.listdir(src)):
            if name.endswith(".sql"):
                z.write(os.path.join(src, name), f"backend/{sub}/{name}")
print("wrote", out)
EOF
