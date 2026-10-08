# Developing OpenNova

How to build and run OpenNova locally: the C++ core, the Godot GDExtension, the
game, and the NovaWorld servers, including how to test the servers
against both retail Joint Operations and our own Godot client.

For the runtime build and release layout see [README.md](README.md). For
deploying your own instance to the cloud see [DEPLOY.md](DEPLOY.md).

## Prerequisites

- **CMake 3.24+** (the pinned SQLite and Dear ImGui fetches use 3.24 options) and a
  **C++17** compiler (MSVC, Clang, or GCC).
- **Godot 4.6.1** (only for Godot work). Set `GODOT_BIN` to the binary, or drop it in `.godot-bin/`.
- **Docker** (Docker Desktop on Windows/macOS) to run the NovaWorld servers locally.
- **Git LFS** (binary test fixtures are LFS objects).

## First-time setup

```bash
git lfs install --local && git lfs pull
git submodule update --init --recursive
```

`docs/` is a submodule of a private repository, so the recursive update skips it.
Maintainers with access opt in once per clone with
`git config submodule.docs.update checkout` before running it.

The GUT test plugin is copied out of the `third_party/gut` submodule into
`godot/addons/gut/` (gitignored) by `scripts/bootstrap_godot.sh`, which also
downloads the pinned imgui-godot release into `godot/addons/imgui-godot/`
(gitignored; the Dear ImGui bridge for the in-engine dev tools, ADR 0039); the
build and test scripts run it for you. `godot-cpp` lives at `third_party/godot-cpp` and is
pulled in by the submodule update above.

## Build the C++ core and run the tests

One shot (configure, build the libraries, run `ctest`, then build the GDExtension):

```bash
scripts/build.sh
```

- `scripts/build.sh --no-godot` skips the GDExtension for fast library-only iteration.
- `scripts/build.sh --no-test` builds without running `ctest`; `-R <regex>` runs only the
  matching tests.
- `--jobs N` sets the build parallelism (`scripts/build.sh` and `scripts/build_godot.sh`).
- Both scripts configure their build tree once and skip that step on later runs; the build
  re-runs CMake by itself when a `CMakeLists.txt` changes.
- `scripts/test_godot.sh --keep-user-dir` leaves the suite's isolated `user://` in place.
- On Windows, keeping the build trees out of real-time antivirus scanning (a Defender
  exclusion, or a Dev Drive) speeds up compiling, linking and the first launch of each
  freshly linked test.

The manual equivalent:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

When you are working on the NovaWorld server, configure with `-DBUILD_NOVAWORLD_HTTP=ON`
(it fetches Asio + Crow and adds the HTTP/API layer to `apps/novaworld_server`, which
otherwise builds with only its UDP listeners; it is OFF by default so the
Windows/macOS hot path stays lean), and scope `ctest` to the net stack:

```bash
ctest --test-dir build --output-on-failure -R "gate|lobby|novaworld|napi|nwu|crypto|pubcrypto|epask|url_cipher|session|gsb|protocol_message"
```

### Naming an app

Every executable under `apps/` takes one name, spelled the same way everywhere (ADR 0040):

| | Form | Example |
|---|---|---|
| Directory | `apps/<name>` (snake case) | `apps/lan_probe` |
| CMake target | `opennova_<name>` | `opennova_lan_probe` |
| Shipped binary (`OUTPUT_NAME`) | `opennova-<name>`, kebab case | `opennova-lan-probe.exe` |
| Usage, help and error text | the binary name | `usage: opennova-lan-probe ...` |
| Companion libraries | `opennova_<name>_<role>` | `opennova_lan_probe_client` |
| Namespace, when it has one | `opennova::<name>` | `opennova::lan_probe` |
| ctest names for the app itself | `opennova_<name>_*` | `opennova_3di_build` |

The entry point is `main.cpp`. `apps/3di` is the one spelling exception: an identifier
cannot start with a digit and `opennova::threedi` is the format library, so its namespace
is `opennova::threedi_cli`. `scripts/package_apps.sh` lists only the directories and
derives the rest, so a new app ships by adding its directory there and its exe to the
`package-apps-windows.yml` smoke list.

## Build the GDExtension

This is the native library that registers our C++ classes (`NovaWorldClient`,
`Terrain`, `ResourceRoot`, ...) with Godot:

```bash
scripts/build_godot.sh            # Dev       -> optimized + symbols (the default)
scripts/build_godot.sh DebugFull  # DebugFull -> /Od, for native debugging
scripts/build_godot.sh Release    # Release   -> plain /O2, no symbols
```

All three flavors produce the same `template_debug`-named artifact, the one loaded by
the Godot editor and source/debug runs of the game; they differ only
in compiler flags. Build `DebugFull` when you need to step through native code;
expect roughly 1.5x whole-frame cost in-game while it is installed, so never profile
against it. The `template_release` DLL that a release export loads is not produced by
this script at all: build it with `scripts/package_godot_windows.ps1`
(`-ExportMode release`, without `-SkipBuild`) locally, or rely on CI's
`build-gdextension-windows` `template_release` leg (master and trunk pushes, and manual
runs).

The artifacts land in `godot/bin/` alongside `godot/bin/opennova.gdextension`. **After a
rebuild, fully restart the Godot editor.** GDExtension class registration does not
hot-reload reliably, and on Windows the running editor holds the DLL lock so the swap is
deferred. A stale DLL shows up as GDScript "class not found" errors for classes that
`engine/` has since added.

The web build (ADR 0049) compiles the same sources as a wasm32 threads side module:
`scripts/build_godot_web.sh [--release]` refuses any Emscripten but 4.0.20 (its header
shows the run inside the pinned `emscripten/emsdk` image), and
`scripts/package_godot_web.sh` exports the `OpenNova Web` preset into `godot/exports/web`.
`deploy/game/Dockerfile` runs both and serves the site on `:8090`; see
[DEPLOY.md](DEPLOY.md) for building and running that image.

## Run the game

```bash
$GODOT_BIN --path godot -- --resource-dir "C:/Games/Joint Operations"
```

On a fresh checkout, import the resources once before the first run:

```bash
$GODOT_BIN --headless --path godot --import
```

The import can crash on a cold cache; retry it as CI does. The main scene is
the runtime game. With no arguments it boots the bundled placeholder menu from
`assets/` (ADR 0048), whose PLAY RETAIL picks and remembers a retail install;
pass `--resource-dir <dir>` to boot a game-data directory directly, with
`--loose-root /d` for an extracted loose tree. The repository and downloads
contain no retail game data. See [README.md](README.md) for packaged launch
examples and exit codes.

## Run the NovaWorld servers locally

The whole stack (gate + NovaWorld UDP + HTTP/API + web portal + the game's web build) via
Docker. `--build` also builds the game image (the wasm and native GDExtensions plus a
Godot export, so its first build is slow), which needs the submodules checked out:

```bash
cd deploy/compose
docker compose -f docker-compose.yml -f docker-compose.dev.yml up --build
```

| service | port | purpose |
|---|---|---|
| gate | `7597/udp` | client bootstrap probe (answers with the server address) |
| NovaWorld UDP | `64206/udp` | NAPI session + in-match traffic (HELLO/JOIN/SESSION/GOODBYE) |
| HTTP / API | `8080/tcp` | `/api/*`, the legacy `NW*.dll` routes |
| web UI (Vite) | `http://localhost:5173` | the Vue site with **hot-reload**; Vite proxies `/api` to the server |
| game (web build) | `http://localhost:8090` | the browser build of the game (`deploy/game/`, ADR 0049), single player only |

The Docker dev override sets the dev values: `ADMIN_API_TOKEN=dev-admin-token`, the seeded
test accounts, and machine-specific defaults for `ONNET_PUBLIC_HOST` and
`ONNET_CLIENT_REFLECT_IP`; set those two explicitly before retail host/join tests. Use `127.0.0.1` only when the retail client and
server run on the same Windows host, and use the reachable LAN IP for second-machine tests.
Sanity check and DB reset:

```bash
curl http://127.0.0.1:8080/api/health
docker compose -f docker-compose.yml -f docker-compose.dev.yml down -v   # wipe the SQLite volume
```

### Fast iteration

Avoid rebuilding images to test changes:

- **Web (Vue):** the dev stack runs the Vite dev server, so edits under `web/` hot-reload live at
  `http://localhost:5173` with no rebuild. (Even lighter: skip the web container and run Vite on the
  host — `cd web && npm install && npm run dev` — it proxies `/api` to `127.0.0.1:8080`.)
- **Server (C++):** the dockerized server is for "I just need the backend up." For active server work,
  run the local binary (below) and rebuild incrementally with `cmake --build build` (only the changed
  objects, seconds). To refresh just the server image in the stack: `docker compose ... up -d --build novaworld`.
- **Ports at a glance:** `5173` = web UI (dev/HMR), `8080` = API/server, `8090` = the
  game's web build. Hitting
  `http://localhost:8080/` shows an "API server" note, which is expected; the dev UI is at `:5173`.

**Without Docker**, build and run the server binary directly. It reads its config from
env vars (`apps/novaworld_server/server_config.cpp`), and the defaults boot a fresh
checkout from the repo root with no setup:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_NOVAWORLD_HTTP=ON
cmake --build build --config Release --target opennova_novaworld_server
# run from the repo root so the default relative paths resolve:
ONNET_PUBLIC_HOST=127.0.0.1 ADMIN_API_TOKEN=dev-admin-token ./build/apps/novaworld_server/opennova-novaworld-server
```

On Windows multi-config generators, the executable is under the selected config directory
and has `.exe`, for example `build/apps/novaworld_server/Release/opennova-novaworld-server.exe`.

Overridable env vars (defaults in parentheses): `ONNET_PUBLIC_HOST` (`127.0.0.1`),
`ONNET_GATE_UDP_PORT` (`7597`), `ONNET_NW_UDP_PORT` (`64206`), `ONNET_HTTP_PORT` (`8080`),
`DATABASE_PATH` (`backend/data/state.db`), `MIGRATIONS_DIR`, `SEED_DIR`, `WEB_DIST_DIR`,
`TEMPLATES_DIR`, `STATIC_DIR`, `ADMIN_API_TOKEN` (unset = admin API closed).

### The headless game server

`opennova-serve` (`apps/serve`, ADR 0051) is the game's Serve Only host with no window,
configured by retail's host file; it boots the mission through the same engine host boot
the game's hosts run (`engine/runtime/inmatch/host_boot.h`). Build and run it against an
install, with the sample host file (`apps/serve/example.host`, shipped in the apps zip):

```bash
cmake --build build --config Release --target opennova_serve
./build/apps/serve/opennova-serve --resource-dir "<Joint Operations install>" /HOST apps/serve/example.host
ctest --test-dir build -C Release -R "serve|host_boot|host_file|admin|mission_rotation"
```

It keeps its files in the directory it runs from, as retail's dedicated server does:
`game.cfg`, `activesrvr.txt`, and for the remote admin `admin.cfg`, `admin_log.txt`,
`banned.txt` and `banlist.txt`. To try the remote admin, set `remote_admin_port` in that
`game.cfg`, give `admin.cfg` a user line and a whitelist that admits you (`boss secret 4F`,
`ip_restrict = 127.0.0.1`), and connect with retail's `RAT.exe` or
`opennova-nw-lister --admin`. The engine halves are `engine/net/admin/admin_server.h` (the
wire), `engine/runtime/inmatch/admin_console.h` (the verbs) and
`engine/runtime/inmatch/rotation_admin.h` (the MISSION verbs over the host's rotation); the
socket pump is `apps/common/admin_tcp_server.h`.

Its usage, the host file's keys, the working directory's files, the remote admin and its
exit codes are in `apps/serve/README.md`.

## Test with retail Joint Operations

Point the retail client's gate host at the dev stack with one hosts-file line in
`C:\Windows\System32\drivers\etc\hosts` (edit it from an elevated editor):

```text
127.0.0.1 gs.novaworld.net
```

`gs.novaworld.net` is the one hostname the retail JO matchmaking flow needs redirected
(NW-L2 in [docs/net/novaworld-net-re.md](docs/net/novaworld-net-re.md)). Use the server's
reachable LAN IP instead of `127.0.0.1` when the game runs on another machine, and delete
the line to go back to NovaLogic's service. The game files stay stock; launch `Jointops.exe`
as usual.

Notes:
- **Windows + WSL2**: the published container ports reach the Windows host at `127.0.0.1`,
  so retail JO on the same machine connects through.
- UDP `7597` and `64206` must be allowed through the Windows firewall.
- Windows Defender may flag the hosts edit (`SettingsModifier:Win32/HostsFileHijack`);
  allow that one line.

## Test with our Godot game

1. Build the GDExtension (above) and run the project:
   `$GODOT_BIN --path godot -- --resource-dir <game dir>`, or run it with no arguments and
   press **PLAY RETAIL** on the placeholder menu.
2. From the menu, open **NovaWorld**. The panel (`godot/game/novaworld_panel.gd`) creates a
   `NovaWorldClient` that probes the local gate at `127.0.0.1:7597` (its `server_host` /
   `gate_port` exports), runs the session handshake against the dev server, fills the
   server browser, and exposes **Host a Game**.
3. For a two-client host/join, run a second instance, or a second machine with
   `ONNET_PUBLIC_HOST` set to your LAN IP so the gate advertises a reachable address.

## Run the test suites

- **C++ (`ctest`)**: run by `scripts/build.sh`, or directly with
  `ctest --test-dir build --output-on-failure -C Release` (net-scoped `-R` above).
- **GDScript (GUT)**: `scripts/test_godot.sh` (headless; needs `GODOT_BIN` or a binary in
  `.godot-bin/`). The script fails if a `*_test.gd` is dropped from collection (usually a
  stale GDExtension DLL missing a class), so a green run means the extension is current.

The full cross-system suite can be flaky from shared state; if a run shows an unrelated
red, re-run that single test file in isolation to confirm before treating it as real.

## Troubleshooting

- **GDScript "class not found" / a GUT test silently dropped**: stale GDExtension. Re-run
  `scripts/build_godot.sh` and fully restart the editor.
- **`error: set GODOT_BIN ...`**: point `GODOT_BIN` at a Godot 4.6.1 binary, or drop one in `.godot-bin/`.
- **Missing classes or parse errors on a fresh clone**: `git submodule update --init --recursive`,
  build the GDExtension, then re-import (`--headless --import`). Test fixtures need `git lfs pull`.
- **Retail JO will not connect**: confirm the dev stack is up (`curl /api/health`),
  `gs.novaworld.net` resolves to the dev server (the hosts line above), and UDP
  `7597`/`64206` are not firewalled.
- **The first import crashes**: re-run `--headless --import` (cold-cache flake).

## Where to go next

- [README.md](README.md): project layout, runtime build, and packaging.
- [DEPLOY.md](DEPLOY.md): deploying your own instance.
- [docs/README.md](docs/README.md): architecture and the reverse-engineering records.
