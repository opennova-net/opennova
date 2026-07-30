# Developing OpenNova

How to build and run OpenNova locally: the C++ core, the Godot GDExtension, the
Godot editor/game, and the NovaWorld servers, including how to test the servers
against both retail Joint Operations and our own Godot client.

For the asset pipeline and packaging (Blender addon, 3ds Max plugin, standalone
importer, Godot exports) see the [Building](README.md#building) section of the
README. For deploying your own instance to the cloud see [DEPLOY.md](DEPLOY.md).

## Prerequisites

- **CMake 3.16+** and a **C++17** compiler (MSVC, Clang, or GCC).
- **Python 3.11 + [uv](https://docs.astral.sh/uv/)** for the importer tooling and Python tests.
- **Godot 4.6.1** (only for Godot work). Set `GODOT_BIN` to the binary, or drop it in `.godot-bin/`.
- **Docker** (Docker Desktop on Windows/macOS) to run the NovaWorld servers locally.
- **.NET 8 SDK** to build the launcher (Windows).
- **Git LFS** (assets and some fixtures are LFS objects).

## First-time setup

```bash
git lfs install --local && git lfs pull
git submodule update --init --recursive
```

The GUT test plugin is copied out of the `third_party/gut` submodule into
`godot/addons/gut/` (gitignored) by `scripts/bootstrap_godot.sh`; the build and
test scripts run it for you. `godot-cpp` lives at `third_party/godot-cpp` and is
pulled in by the submodule update above.

## Build the C++ core and run the tests

One shot (configure, build the libraries, run `ctest`, then build the GDExtension):

```bash
scripts/build.sh
```

- `BUILD_GODOT=0 scripts/build.sh` skips the GDExtension for fast library-only iteration.
- `JOBS=N` sets the build parallelism.

The manual equivalent:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIB=ON -DOPENNOVA_ENABLE_PYTHON_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

When you are working on the NovaWorld server, configure with `-DBUILD_NOVAWORLD_HTTP=ON`
(it fetches Asio + Crow and builds `apps/novaworld_server`; it is OFF by default so the
Windows/macOS hot path stays lean), and scope `ctest` to the net stack:

```bash
ctest --test-dir build --output-on-failure -R "gate|lobby|novaworld|napi|nwu|crypto|pubcrypto|epask|url_cipher|session|gsb|protocol_message"
```

## Build the GDExtension

This is the native library that registers our C++ classes (`NovaWorldClient`,
`NovaTerrain`, `NovaResourceRoot`, ...) with Godot:

```bash
scripts/build_godot.sh            # Dev       -> the editor (optimized + symbols)
scripts/build_godot.sh DebugFull  # DebugFull -> the editor (/Od, for native debugging)
scripts/build_godot.sh Release    # Release   -> exports
```

`Dev` and `DebugFull` produce the same `template_debug`-named artifact — the one loaded
by the Godot editor and by standalone game runs launched from ONED (F5/F6) — differing
only in compiler flags. Build `DebugFull` when you need to step through native code;
expect roughly 1.5x whole-frame cost in-game while it is installed, so never profile
against it.

The artifacts land in `godot/bin/` alongside `godot/bin/opennova.gdextension`. **After a
rebuild, fully restart the Godot editor.** GDExtension class registration does not
hot-reload reliably, and on Windows the running editor holds the DLL lock so the swap is
deferred. A stale DLL shows up as GDScript "class not found" errors for classes that
`engine/` has since added.

(The README documents a `cmake -S godot/engine -B build-godot ...` equivalent; the script
is the canonical path.)

## Run the Godot editor and game

```bash
$GODOT_BIN --path godot
```

On a fresh checkout, import the resources once before the first run:

```bash
$GODOT_BIN --headless --path godot --import
```

The import can crash on a cold cache; just run it again (CI retries it). The project's
main scene is the runtime game; the OpenNova Editor (ONED) workspaces open from there.

## Run the NovaWorld servers locally

The whole stack (gate + NovaWorld UDP + HTTP/API + web portal) via Docker:

```bash
cd deploy/compose
docker compose -f docker-compose.yml -f docker-compose.dev.yml up --build
```

| service | port | purpose |
|---|---|---|
| gate | `7597/udp` | client bootstrap probe (answers with the server address) |
| NovaWorld UDP | `64206/udp` | NAPI session + in-match traffic (HELLO/JOIN/SESSION/GOODBYE) |
| HTTP / API | `8080/tcp` | `/api/*`, `/api/server-info`, the legacy `NW*.dll` routes |
| web UI (Vite) | `http://localhost:5173` | the Vue site with **hot-reload**; Vite proxies `/api` to the server |

Most dev values come from `deploy/env/app.dev.env` (committed, non-secret): `admin`/`admin`
basic auth, `ADMIN_API_TOKEN=dev-admin-token`. The Docker dev override has
machine-specific defaults for `ONNET_PUBLIC_HOST` and `ONNET_CLIENT_REFLECT_IP`; set them
explicitly before retail host/join tests. Use `127.0.0.1` only when the retail client and
server run on the same Windows host, and use the reachable LAN IP for second-machine tests.
Sanity check and DB reset:

```bash
curl http://127.0.0.1:8080/api/server-info
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
- **Ports at a glance:** `5173` = web UI (dev/HMR), `8080` = API/server, `8088` = nginx (only the
  prod-parity build). Hitting `http://localhost:8080/` shows an "API server" note — that is expected;
  the dev UI is at `:5173`.

**Without Docker**, build and run the server binary directly. It reads its config from
env vars (`apps/novaworld_server/server_config.cpp`), and the defaults boot a fresh
checkout from the repo root with no setup:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_NOVAWORLD_HTTP=ON
cmake --build build --config Release --target opennova_novaworld_server
# run from the repo root so the default relative paths resolve:
ONNET_PUBLIC_HOST=127.0.0.1 ADMIN_API_TOKEN=dev-admin-token ./build/apps/novaworld_server/opennova-novaworld
```

On Windows multi-config generators, the executable is under the selected config directory
and has `.exe`, for example `build/apps/novaworld_server/Release/opennova-novaworld.exe`.

Overridable env vars (defaults in parentheses): `ONNET_PUBLIC_HOST` (`127.0.0.1`),
`ONNET_GATE_UDP_PORT` (`7597`), `ONNET_NW_UDP_PORT` (`64206`), `ONNET_HTTP_PORT` (`8080`),
`DATABASE_PATH` (`backend/data/state.db`), `MIGRATIONS_DIR`, `SEED_DIR`, `WEB_DIST_DIR`,
`TEMPLATES_DIR`, `STATIC_DIR`, `ADMIN_API_TOKEN` (unset = admin API closed).

## Test with retail Joint Operations

Build and run the launcher locally (.NET 8, Windows). It requests administrator rights
(editing the hosts file is its whole job), so run it from an elevated terminal or accept the
UAC prompt at startup:

```bash
dotnet build launcher/OpenNovaLauncher.sln -c Debug    # or -c Release
dotnet test  launcher/OpenNovaLauncher.sln             # the xunit suite (optional)
dotnet run --project launcher/src/OpenNovaLauncher/OpenNovaLauncher.csproj
```

`dotnet run` builds and launches `OpenNovaLauncher.exe` from the build output under
`launcher/src/OpenNovaLauncher/bin/`; you can also run that exe directly. The single-file
distributable (`dotnet publish ... -r win-x64`) is a packaging step you do not need for local
dev (see [`launcher/README.md`](launcher/README.md)).

Then, in the launcher:

1. **Preferences**: enable **"Developer mode (redirect to 127.0.0.1)"**.
2. Register your Joint Operations install directory.
3. Turn on **"Manage NovaWorld redirection"**.
4. Launch the game from the launcher.

The launcher points `gs.novaworld.net` at `127.0.0.1` through a managed hosts-file block;
your game files stay completely stock (no patched exe, no injected DLL). With the dev
stack up, retail JO connects straight to the local gate.

Notes:
- **Windows + WSL2**: the published container ports reach the Windows host at `127.0.0.1`,
  so retail JO on the same machine connects through.
- UDP `7597` and `64206` must be allowed through the Windows firewall.
- Windows Defender may flag the hosts edit (`SettingsModifier:Win32/HostsFileHijack`); see
  [`launcher/README.md`](launcher/README.md).

## Test with our Godot game

1. Build the GDExtension (above) and run the project: `$GODOT_BIN --path godot`.
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
- **Python**: `scripts/test_python.sh`.

The full cross-system suite can be flaky from shared state; if a run shows an unrelated
red, re-run that single test file in isolation to confirm before treating it as real.

## Troubleshooting

- **GDScript "class not found" / a GUT test silently dropped**: stale GDExtension. Re-run
  `scripts/build_godot.sh` and fully restart the editor.
- **`error: set GODOT_BIN ...`**: point `GODOT_BIN` at a Godot 4.6.1 binary, or drop one in `.godot-bin/`.
- **Missing assets or parse errors on a fresh clone**: `git lfs pull` and
  `git submodule update --init --recursive`, then re-import (`--headless --import`).
- **Retail JO will not connect**: confirm the dev stack is up (`curl /api/server-info`),
  the launcher shows redirection active, and UDP `7597`/`64206` are not firewalled.
- **The first import crashes**: re-run `--headless --import` (cold-cache flake).

## Where to go next

- [README.md](README.md): the asset pipeline and packaging.
- [DEPLOY.md](DEPLOY.md): deploying your own instance.
- [docs/README.md](docs/README.md): architecture and the reverse-engineering records.
