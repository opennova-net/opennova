#!/usr/bin/env bash
# Package the native apps under apps/ for Windows: build every app (Release)
# and zip the executables flat, with the NovaWorld server's migrations and seed
# and opennova-nw-lister's example listing beside them and a README.txt naming
# each tool.
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
apps=(3di extract lan_probe novaworld_server nw_lister wire)

# The static MSVC runtime keeps the zip free of a VC++ runtime install, and a
# separate build tree keeps it apart from development builds. No app links the
# dev tools, so they (and the Dear ImGui fetch they bring) stay off. The
# server builds without BUILD_NOVAWORLD_HTTP: the HTTP portal ships only in
# the Linux container image.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPENNOVA_DEVTOOLS=OFF > /dev/null
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

opennova-lan-probe.exe
    Waits for a LAN game host to answer the discovery probe and reports
    whether it is ready. It never joins the game.

opennova-novaworld-server.exe
    The NovaWorld server: the gate (UDP 7597) and session (UDP 64206)
    listeners. The web portal and HTTP routes are only in the Linux container
    image. Run it from this folder: it applies backend\\migrations and
    backend\\seed and keeps its database under backend\\data. Settings are
    environment variables, listed in apps/novaworld_server/README.md in the
    repository.

opennova-nw-lister.exe
    Lists one server on a NovaWorld master without running the game: the
    name, map, players and settings come from a JSON file it re-reads
    (listing.example.json), or from the game server's remote-admin port
    with --admin. Stopping it removes the row. It sends to 127.0.0.0/8
    only unless given --allow-public.

opennova-wire.exe
    Decodes NovaWorld in-game traffic from a .pcap/.pcapng capture, a .sph
    server log or a hexcap text file.
"""
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("README.txt", readme.replace("\n", "\r\n"))
    for exe in exes:
        z.write(exe, os.path.basename(exe))
    z.write(os.path.join(root, "apps", "nw_lister", "listing.example.json"), "listing.example.json")
    for sub in ("migrations", "seed"):
        src = os.path.join(root, "backend", sub)
        for name in sorted(os.listdir(src)):
            if name.endswith(".sql"):
                z.write(os.path.join(src, name), f"backend/{sub}/{name}")
print("wrote", out)
EOF
