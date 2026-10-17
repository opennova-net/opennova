# Developing OpenNova

How to build and run OpenNova locally: the C++ core, the Godot GDExtension, the
game, the NovaWorld servers and the switcher, including how to test the servers
against both retail Joint Operations and our own Godot client.

For the runtime build and release layout see [README.md](README.md). For
deploying your own instance to the cloud see [DEPLOY.md](DEPLOY.md).

## Prerequisites

- **CMake 3.24+** (the pinned rgbcx and Dear ImGui fetches use 3.24 options) and a
  **C++17** compiler (MSVC, Clang, or GCC).
- **libpq** (`libpq-dev` on Debian/Ubuntu; only for the NovaWorld server, Linux), and a
  PostgreSQL 17 server for its tests and local runs (`scripts/dev_postgres.sh` runs one in
  Docker). Its HTTP layer also needs **hiredis** and OpenSSL (`libhiredis-dev libssl-dev`)
  and a Redis server, Valkey 8 (`scripts/dev_redis.sh` runs one in Docker).
- **Godot 4.6.1** (only for Godot work). Set `GODOT_BIN` to the binary, or drop it in `.godot-bin/`.
- **Docker** (Docker Desktop on Windows/macOS, Compose v2.20+) to run the NovaWorld servers
  locally (`scripts/dev_stack.sh`).
- **.NET 8 SDK** (only for the switcher, Windows).
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
- `scripts/build.sh --suite core --shard editor|rest` runs one of CI's two core shards (the
  rows labelled `editor`, or the others) with its own attestation.
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

When you are working on the NovaWorld service (Linux only), configure with
`-DOPENNOVA_NOVAWORLD_SERVICE=ON -DBUILD_NOVAWORLD_HTTP=ON`: the first defines the
service, its five programs (`apps/novaworld_common`, the shared library, and
`apps/novaworld_gate`, `_lobby`, `_web`, `_controller`, `_worker`) and their tests, the
second fetches Asio + Crow and adds the HTTP layer, which web, the controller and the
worker need (without it only the gate and the lobby build).
Both are OFF by default, so the game and tool builds stay lean. The service's database
tests make their databases on a PostgreSQL server named by
`-DOPENNOVA_TEST_PG=<uri>`, and without one they skip (which `--suite core` fails):
`scripts/dev_postgres.sh up` starts a local one in Docker and prints the value. The HTTP
layer's tests need a Redis server as well, `-DOPENNOVA_TEST_REDIS=<url>`, and skip the same
way without one: `scripts/dev_redis.sh up` starts one and prints the value. Then scope
`ctest` to the net stack:

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
`build-gdextension-windows` `template_release` leg (master and editor-trunk pushes, and
manual runs).

The artifacts land in `godot/bin/` alongside `godot/bin/opennova.gdextension`. **After a
rebuild, fully restart the Godot editor.** GDExtension class registration does not
hot-reload reliably, and on Windows the running editor holds the DLL lock so the swap is
deferred. A stale DLL shows up as GDScript "class not found" errors for classes that
`engine/` has since added.

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

The whole NovaWorld service runs on your machine with one command, in Docker, in the
shape a deployment runs it (`deploy/compose/`, ADR 0063): the five programs each in a
container of its own, PostgreSQL, Valkey, the site behind prod's nginx portal, and, given a
data set, hosted game servers on machines the worker launches.

```bash
scripts/dev_stack.sh up        # build the images and start the stack (about 10 min the first time)
scripts/dev_stack.sh smoke     # the smoke test against it
scripts/dev_stack.sh status    # its containers and hosted machines
scripts/dev_stack.sh logs -f novaworld-web
scripts/dev_stack.sh down      # stop it; the database stays (reset wipes it and the state)
```

`up` finds this machine's LAN address and announces it everywhere (`ONNET_PUBLIC_HOST`: the
gate's `POSTIPADDRESS`, the lobby's endpoint, web's host); `--address <ipv4>` names another.
It is never loopback, since the stack's own containers (the smoke test, the hosted servers)
reach the published ports there too. It prints what to point a client at when it is up.

| service | on this machine | purpose |
|---|---|---|
| `novaworld-gate` | `7597/udp` | the gate: a retail client's probe, answered with the live lobby |
| `novaworld-lobby` | `64206/udp` | a lobby instance: sessions, the server browser, listings |
| `novaworld-web` | `8080/tcp` | the retail `NW*.dll` and `.gsb` routes |
| `portal` | `http://localhost:8000` | the site (prod's nginx in web's network, as in web's ECS task) and its `/api` |
| `novaworld-controller`, `novaworld-worker` | none | the supervisor and the jobs; the machines' internal listener at `172.30.77.10:8070` |
| `postgres`, `valkey` | none | the database (the `pg_data` volume), web's state |
| `docker-api`, `machine-data`, `dev-machine-<id>` | none | hosted servers (below) |

The dev values: the admin token `dev-admin-token` (`Authorization: Bearer`), the seeded
accounts `test`/`test` (the first site admin), `foo`/`foo` and `test1`..`test3`/`test`, and the
portal's `/admin` pages' basic auth `admin`/`dev`. The site's API answers through the portal
alone (`ONNET_API_PROXY_ONLY`, as deployed), so call `http://localhost:8000/api/...`, not
`:8080`. Billing is off unless the `STRIPE_*` settings are in the shell (`op run
--env-file=deploy/env/billing.dev.env.tpl -- scripts/dev_stack.sh up`); its webhooks need
`STRIPE_WEBHOOK_SECRET` set by hand from `stripe listen --print-secret` (with `stripe listen
--forward-to localhost:8000/api/billing/webhook` running). No AWS credential is needed.

Other options of `up`: `--no-build` starts the images already built, `--vite` also runs the
site's Vite dev server at `http://localhost:5173` (hot reload of `web/`, its `/api` through
the portal), and `--prebuilt <dir>` builds the images from a Linux build tree's binaries
(a configure of the service as `.github/workflows/net-linux.yml`'s, built on Ubuntu 24.04,
the images' own release, so their glibc and libstdc++ match) instead of compiling in
Docker, as CI does. `up` refuses to start while another container publishes one of its ports
(an older stack: stop it first).

### Hosted game servers locally

With a data set the stack runs hosted servers as a deployment does, machines and all: the
worker's docker machine driver launches each fleet machine as a container of the serve image
(`dev-machine-<id>`, through a socket proxy that admits the Docker containers API on every
container of this machine, which is root-equivalent on it: the proxy sits on an internal
network that the worker alone joins, and the stack is for a developer's machine, never a
deployment), whose
own `opennova-hostd` redeems the one-time token its launch handed it at the controller's
internal listener, syncs from the machine's address, and runs each server the controller
places on its slots as an `opennova-serve` child (`apps/hostd/README.md`, "The process
runtime"); the controller's admin links reach the slots' admin ports at the machine's
address, and the servers list through the gate at the announced address.

```bash
scripts/dev_stack.sh up --data "<Joint Operations install>"   # a licensed install (the default: OPENNOVA_JO_DIR)
scripts/dev_stack.sh up --synthetic-data                      # no licensed data: one synthetic deathmatch map
scripts/dev_stack.sh up --no-hosting                          # neither
```

`--data` mounts the folder read-only in every machine as the `jop_2_consumer` data set
(`--data-set` names another) with the rotation `ASH_G1A.bms` (`--mission` names another).
Retail data never enters the repository or an image. With no data set hosting stays off
(web logs `hosted servers: off (provider none ...)` and `up` prints it).

To start one by hand: log in to the site as `test`, grant an account a server (the admin's
comp grant, `PUT /api/admin/users/<id>/comp-servers` with `{"comp_servers": 1}`), and start it
from that account's servers page (`POST /api/me/servers`). It lists in the lobby a retail
client browses; its listing's address is the machine's as the lobby sees it through Docker,
so joining it is for a client on the stack's network, not a retail client on this machine.

### The smoke test

`scripts/dev_stack.sh smoke` runs `tests/novaworld/stack_smoke.cpp` from a container on the
stack's network (the `smoke` image) and exits non-zero on any failure, printing the
programs' and machines' last log lines. It checks every program's `GET /livez` and web's
`GET /healthz`; an account registered and logged in through the portal's API and `GET
/api/me`; the gate's answer to a retail probe naming a live lobby and a NovaWorld session
verified there (the engine's own codecs); the jobs admin API; and, with a data set, a hosted
server started through the site that reaches `running` on a machine the worker launched, is
listed in the lobby, answers console commands through the controller's admin link (`PLAYER
LIST`, `CHAT SEND`) and stops. CI's `local-stack.yml` runs `up --synthetic-data --prebuilt`
and the smoke test on every change to the service, its images, the compose files or `web/`.

### Fast iteration

Avoid rebuilding images to test changes:

- **Web (Vue):** `scripts/dev_stack.sh up --vite` runs the Vite dev server, so edits under
  `web/` hot-reload live at `http://localhost:5173` with no rebuild. (Even lighter: run Vite
  on the host, `cd web && npm install && npm run dev`, with `VITE_API_TARGET=http://localhost:8000`.)
- **Service (C++):** the images are for "I just need the backend up". For active service
  work, run the local binaries (below) and rebuild incrementally with `cmake --build build`.
  To refresh just the programs' images in the stack: `scripts/dev_stack.sh up` again (only
  the changed layers rebuild), or on Linux `scripts/dev_stack.sh up --prebuilt build` after an
  incremental build.

**Without Docker** (Linux), build and run the five programs directly. Each reads its
config from env vars (`apps/novaworld_common/server_config.cpp`); their database is a
local PostgreSQL that `scripts/dev_postgres.sh up` starts in Docker (and prints the
settings for), and the other defaults boot a fresh checkout from the repo root:

```bash
scripts/dev_postgres.sh up      # opennova-dev-postgres on 127.0.0.1:5432
scripts/dev_redis.sh up         # opennova-dev-redis on 127.0.0.1:6379
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPENNOVA_NOVAWORLD_SERVICE=ON -DBUILD_NOVAWORLD_HTTP=ON \
    -DOPENNOVA_TEST_PG=postgresql://novaworld:dev@127.0.0.1:5432/postgres \
    -DOPENNOVA_TEST_REDIS=redis://127.0.0.1:6379
cmake --build build --config Release --target opennova_novaworld_gate opennova_novaworld_lobby \
    opennova_novaworld_web opennova_novaworld_controller opennova_novaworld_worker
# run from the repo root so the default relative paths resolve; the five programs, each in
# its own terminal (or with &), each its own health port (8081 is every one's default):
export DATABASE_PASSWORD=dev ADMIN_API_TOKEN=dev-admin-token ONNET_PUBLIC_HOST=127.0.0.1 \
    ONNET_WEB_HOST=127.0.0.1
ONNET_HEALTH_PORT=8081 ONNET_LOBBY_ENDPOINT=127.0.0.1:64206 ./build/apps/novaworld_lobby/opennova-novaworld-lobby
ONNET_HEALTH_PORT=8082 ./build/apps/novaworld_gate/opennova-novaworld-gate
./build/apps/novaworld_web/opennova-novaworld-web
ONNET_HEALTH_PORT=8083 ./build/apps/novaworld_controller/opennova-novaworld-controller
ONNET_HEALTH_PORT=8084 ./build/apps/novaworld_worker/opennova-novaworld-worker
```

There is no all-in-one binary: each program is its own process (`apps/novaworld_common/README.md`
"Programs"). The lobby needs `ONNET_LOBBY_ENDPOINT` (the endpoint it announces, which the
gate hands clients; in a deployment the load balancer's, shared by every lobby); the gate, the lobby and web need
`ONNET_WEB_HOST` (the address the gate, the lobby and the pages announce for HTTP), and the
gate `ONNET_PUBLIC_HOST`. On one machine each program but web answers `GET /livez` on its
own `ONNET_HEALTH_PORT`. Hosted servers need machines of their own (containers, or
cloud machines): run them in the Docker stack above (the in-process tests,
`hosted_local_e2e`, drive the real agent on one machine).

`scripts/dev_postgres.sh down` stops the database and keeps its data;
`scripts/dev_postgres.sh reset` deletes the data too. `scripts/dev_redis.sh down` stops
Redis (it keeps nothing).

Overridable env vars (defaults in parentheses): `ONNET_PUBLIC_HOST` (`127.0.0.1`),
`ONNET_GATE_UDP_PORT` (`7597`), `ONNET_NW_UDP_PORT` (`64206`), `ONNET_HTTP_PORT` (`8080`),
`ONNET_HEALTH_PORT` (`8081`),
`DATABASE_URL` (`postgresql://novaworld@127.0.0.1:5432/novaworld`), `DATABASE_PASSWORD`
(unset), `DATABASE_POOL_MAX` (`16`), `ONNET_REDIS_URL` (`redis://127.0.0.1:6379`),
`ONNET_REDIS_PASSWORD` (unset), `MIGRATIONS_DIR`, `SEED_DIR`, `WEB_DIST_DIR`,
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
`ip_restrict = 127.0.0.1`), and connect with retail's `RAT.exe`. The engine halves are
`engine/net/admin/admin_server.h` (the wire), `engine/runtime/inmatch/admin_console.h` (the verbs) and
`engine/runtime/inmatch/rotation_admin.h` (the MISSION verbs over the host's rotation); the
socket pump is `apps/common/admin_tcp_server.h`.

Its usage, the host file's keys, the working directory's files, the remote admin and its
exit codes are in `apps/serve/README.md`.

## Test with retail Joint Operations

Point the retail client's gate host at the stack's address, the one `scripts/dev_stack.sh
up` printed (this machine's LAN address). The OpenNova Switcher (below) does it: pick
**Custom address...** and enter that address, then **NovaLogic NovaWorld** to go back. By
hand it is one hosts-file line in `C:\Windows\System32\drivers\etc\hosts` (edit it from
an elevated editor):

```text
192.168.1.50 gs.novaworld.net
```

`gs.novaworld.net` is the one hostname the retail JO matchmaking flow needs redirected
(NW-L2 in [docs/net/novaworld-net-re.md](docs/net/novaworld-net-re.md)). The same address
works for a game on another machine of the LAN; delete the line to go back to NovaLogic's
service. The game files stay stock; launch `Jointops.exe` as usual and log in with a dev
account (`test`/`test`).

Notes:
- **Windows + WSL2**: the published container ports reach the Windows host at its LAN
  address (and at `127.0.0.1`), so retail JO on the same machine connects through.
- UDP `7597` and `64206` must be allowed through the Windows firewall.
- Windows Defender may flag the hosts edit (`SettingsModifier:Win32/HostsFileHijack`);
  allow that one line.

## Build the switcher

The OpenNova Switcher (`switcher/`, ADR 0054) is a .NET 8 WinForms tray app outside the
CMake build. It switches retail's NovaWorld hostname (`gs.novaworld.net`) between
OpenNova's service, NovaLogic's and a custom address through one managed block in the
hosts file; [`switcher/README.md`](switcher/README.md) is its user guide.

```bash
dotnet test switcher/OpenNovaSwitcher.sln -c Release
dotnet publish switcher/src/OpenNovaSwitcher/OpenNovaSwitcher.csproj -c Release -r win-x64 -o build/switcher
```

The publish step writes the self-contained single-file `build/switcher/opennova-switcher.exe`.
Its manifest asks for administrator rights, so starting it from a terminal that is not
elevated shows the UAC prompt (and `dotnet run` fails there with "requires elevation").
Its **OpenNova service URL...** setting points it at a local stack, `http://localhost:8000`
(the portal), whose `GET /api/server-info` answers `ONNET_PUBLIC_HOST`. CI's
`package-switcher-windows.yml` runs the suite, publishes the exe and zips it with its README.

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

## Lint and format checks

CI's required gates are the `scripts/lint/*.py --enforce` checks in `ci.yml`'s `test`
job (stdlib Python, no installs). Formatting is advisory for now: the tree is not yet
formatted to `.clang-format`, `ruff.toml`, `gdformatrc` or `gdlintrc`, so
`scripts/lint/format_lint.py` checks only the lines a change adds, against the
merge-base with `origin/master`, and CI's `format-lint` job annotates the diff
without failing. Run it locally with the same pins (new files count once staged):

```bash
pip install clang-format==21.1.6 ruff==0.16.9 gdtoolkit==4.5.0
python scripts/lint/format_lint.py            # C++, Python and GDScript on changed lines
ruff format <file.py>; gdformat <file.gd>     # format a file you own end to end
```

Never reformat a whole existing file as part of another change.

## Cut a release

Tag a master merge head whose master CI run passed (`git tag v0.0.N <sha>` then
`git push origin v0.0.N`). `release.yml`'s `gate` job refuses a tag that is not on
master or whose commit has no green master-push `ci.yml` run (it waits for one still
running), so the release ships only what every required CI gate already passed; the
package jobs then build the game, Blender add-on, apps and switcher zips.

## Troubleshooting

- **GDScript "class not found" / a GUT test silently dropped**: stale GDExtension. Re-run
  `scripts/build_godot.sh` and fully restart the editor.
- **`error: set GODOT_BIN ...`**: point `GODOT_BIN` at a Godot 4.6.1 binary, or drop one in `.godot-bin/`.
- **Missing classes or parse errors on a fresh clone**: `git submodule update --init --recursive`,
  build the GDExtension, then re-import (`--headless --import`). Test fixtures need `git lfs pull`.
- **Retail JO will not connect**: confirm the stack is up (`scripts/dev_stack.sh smoke`),
  `gs.novaworld.net` resolves to the stack's address (the Switcher, or the hosts line
  above), and UDP `7597`/`64206` are not firewalled.
- **The first import crashes**: re-run `--headless --import` (cold-cache flake).

## Where to go next

- [README.md](README.md): project layout, runtime build, and packaging.
- [DEPLOY.md](DEPLOY.md): deploying your own instance.
- [docs/README.md](docs/README.md): architecture and the reverse-engineering records.
