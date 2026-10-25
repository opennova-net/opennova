# OpenNova

OpenNova is an open-source C++ and Godot reimplementation of the Joint
Operations generation of NovaLogic games. The repository contains the game
runtime, native format readers, NovaWorld-compatible services, and the project
web portal. The minimal runtime downloads OpenNova's own game on first run:
"OP: Acid Gambit", a mission for single player and Co-op over LAN, and its
"OP: Nifty Package" expansion. The same `opennova.exe` can run against an existing
Joint Operations or Delta Force Xtreme 2 install: put it in that game's folder and run it.

This is a pre-1.0 project. Implemented systems and known differences from the
retail games are tracked in the [divergence ledger](docs/divergence-ledger.md).

## What is in the repository

| Path | Purpose |
| --- | --- |
| `engine/` | Native formats, runtime, networking, and service code. |
| `godot/` | Godot game, project resources, and GDScript tests. |
| `apps/` | Native command-line and NovaWorld service applications. |
| `switcher/` | OpenNova Switcher: a Windows tray app that points a stock Joint Operations or Delta Force Xtreme 2 install at OpenNova's NovaWorld, NovaLogic's, or an address you enter, through the hosts file (.NET 8, with its xunit tests). Only the original exes need it: `opennova.exe` reaches OpenNova's NovaWorld itself. |
| `tools/blender/opennova_3di/` | Experimental Blender add-on for importing and exporting NovaLogic 3DI models and their ADM/BAD animations. |
| `fixtures/` | Test fixtures: synthetic files minted by `tests/fixtures/*_gen.cpp` (the 3DI model set under `fixtures/threedi/synth/`, terrain, fonts, sound banks) plus a small retail-interop keep set (`fixtures/README.md`). |
| `tests/` | Native CTest suite. |
| `web/` | NovaWorld web portal. |
| `deploy/`, `infra/`, `backend/` | Local and hosted service infrastructure. |

Runtime asset loading is native and the supported object input is 3DI. The
repository holds no game data: OpenNova's own game downloads through the updater
on first launch, built from OpenNova Editor projects outside the repository. The
experimental [Blender add-on](tools/blender/opennova_3di/README.md) imports and
exports 3DI models and their ADM/BAD animations for authoring, using the
bundled native `opennova-3di` readers and writers. See its documentation for
round-trip limitations.

The OpenNova Editor (pre-1.0 and experimental, ADR 0046) is a separately exported
application, `opennova-editor.exe`, for the data of the JO and newer titles, starting with
Joint Operations. A project is a directory of loose game files, a base game or an
expansion. A Problems list checks every file the engine expects by name and offers fixes
(one Fix all makes a blank but bootable game). Each file opens in a tab with its unsaved
changes marked: the catalogs and other definition tables, string tables, menus and their
stylesheets, models with their animation clips and tables, missions in a 3D view of their
terrain with their event logic, terrains (heights, maps and tiles), textures, fonts,
sounds and music banks, scripts, environments, AI profiles and more. Previews draw a menu
screen, a model or a mission the way the game does. Imports bring files from a folder, an
archive or a game install along with the files they need, and convert PNG images, glyph
sheets, terrain image sets and the Blender add-on's `.o3d` and `.o3a` files. Build packs
the archives, Play runs the game on them (OpenNova's runtime or the retail executable),
and Export writes a game folder. An agent can drive it through its own MCP endpoint
([docs/mcp.md](docs/mcp.md)). Geometry and animation keys are authored in Blender with the
add-on above (ADR 0047).

## Build and test

Prerequisites are CMake, a C++17 compiler, Git LFS, and Git submodules. Godot
4.6.1 is required for the game and GDScript suite; the .NET 8 SDK for the
Windows switcher.

```bash
git lfs install --local
git lfs pull
git submodule update --init --recursive

# Configure, build, and run CTest, then build the GDExtension.
scripts/build.sh

# Native-only iteration.
scripts/build.sh --no-godot

# Headless Godot tests.
scripts/test_godot.sh
```

The equivalent native commands are:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

See [DEVELOPING.md](DEVELOPING.md) for the GDExtension, servers, switcher, and
retail-compatibility development workflows.

## Run OpenNova

Build the GDExtension, point `GODOT_BIN` at Godot 4.6.1, import the project's resources
once on a fresh checkout, and run it:

```bash
$GODOT_BIN --headless --path godot --import   # once; retry it if a cold cache crashes it
$GODOT_BIN --path godot -- /game jo --resource-dir "C:/Games/Joint Operations" --no-update
```

In its own folder the packaged runtime plays OpenNova's game, and its first launch
downloads OpenNova's content through the adjacent `update.exe`. Existing content
starts normally while an update check runs in the background; **UPDATE** appears
in the main menu when a newer package is available. Installed content works offline.

To play a retail install, extract the runtime into the retail game's folder and run
`opennova.exe`: without `/game`, the folder's `UPDATE.WIZ` says which game it holds
(Joint Operations or Delta Force Xtreme 2; a folder without one is OpenNova's). A
folder whose wizard names another game is refused. `/game` chooses explicitly, also
for a separate content directory:

```bash
opennova.exe -- /game dfx2 --resource-dir "C:/Games/Delta Force Xtreme 2"
```

For loose data without boot archives, add `--loose-root /d`. `/game <code>`
selects `opennova`, `jo`, or `dfx2`; `/exp <name>` selects an expansion. A
`--resource-dir` directory is never saved. An empty `--resource-dir` prints
usage and exits with code 2. Use `--no-update` for development runs; Editor Play
sets it automatically. Source checkouts use exported content from the assets
repository or an existing retail install.

## Downloads

CI and tagged releases publish `opennova-game-windows-v<version>.zip`. It
contains `opennova.exe`, its native dependencies, `update.exe`, `revupdat.exe`,
and the small bootstrap configuration. Game content is downloaded on first run.
Retail game data is supplied separately. Debug builds include
the game's dev tools (Insert).

They also publish `opennova-editor-windows-v<version>.zip`: `editor/` holds the
OpenNova Editor (`opennova-editor.exe`) and `runtime/` the game it plays your project
with. No Godot install is needed. The editor is pre-1.0 and experimental, and works on
Joint Operations projects today; what it edits is listed above.

A PR's CI build comment also links `opennova-blender-addon-windows.zip`.
Tagged [releases](https://github.com/opennova-net/opennova/releases) include
`opennova-blender-addon-windows-v<version>.zip`. Install that zip directly in
Blender 5.0 or newer using **Preferences > Get Extensions > Install from Disk**.
The add-on currently supports Windows x64.

The native apps ship together in `opennova-apps-windows.zip` (on a PR's build
comment) and `opennova-apps-windows-v<version>.zip` (on tagged releases): the
`opennova-3di`, `opennova-extract`, `opennova-lan-probe`, `opennova-parity`,
`opennova-update-tool` (builds and serves packed game updates for NovaLogic's updater) and
`opennova-wire` command-line tools and the headless game server
(`opennova-serve`, with a sample host file; it plays a map rotation and takes retail's
remote admin), for Windows x64. Its `README.txt` describes each one. The NovaWorld
service runs on Linux only; on Windows, run the local stack (`scripts/dev_stack.sh up`,
[DEVELOPING.md](DEVELOPING.md), "Run the NovaWorld servers locally") under Docker Desktop.

The OpenNova Switcher ships as `opennova-switcher-windows.zip` (on a PR's build
comment) and `opennova-switcher-windows-v<version>.zip` (on tagged releases):
`opennova-switcher.exe` and its README. It is a tray app that switches a stock
Joint Operations install between OpenNova's NovaWorld, NovaLogic's NovaWorld and
an address you enter, by managing one block in the Windows hosts file; the game's
files stay stock. Only the original exes need it: `opennova.exe`'s NovaWorld screen
reaches OpenNova's NovaWorld (`nw.opennova.net`) by itself. It needs administrator rights, since the hosts file does. The
exe is unsigned, so releases also carry the zip's SHA-256
(`opennova-switcher-windows-v<version>.zip.sha256`) to check before running it.
See [switcher/README.md](switcher/README.md).

## Run your own NovaWorld

To run your own NovaWorld, the whole hosted service (the six programs, the website,
billing and the hosted game servers) on your own AWS, Cloudflare, Stripe and 1Password
accounts, follow [DEPLOY.md](DEPLOY.md): its "From zero, in order" checklist is the
bring-up, and "Running it under your own names" lists what to change on a fork. Try the
same stack on one machine first with `scripts/dev_stack.sh up` (DEVELOPING.md).

## Documentation

The architecture, format notes, reverse-engineering records, and implementation
status live in `docs/`, a submodule of a private repository that
`git submodule update --init --recursive` skips.

## License

See [LICENSE](LICENSE).
