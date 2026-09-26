# OpenNova

OpenNova is an open-source C++ and Godot reimplementation of the Joint
Operations generation of NovaLogic games. The repository contains the game
runtime, native format readers, NovaWorld-compatible services, and the project
web and launcher applications. Users supply their own game data.

This is a pre-1.0 project. Implemented systems and known differences from the
retail games are tracked in the [divergence ledger](docs/divergence-ledger.md).

## What is in the repository

| Path | Purpose |
| --- | --- |
| `engine/` | Native formats, runtime, networking, and service code. |
| `godot/` | Godot game, project resources, and GDScript tests. |
| `apps/` | Native command-line and NovaWorld service applications. |
| `tools/blender/opennova_3di/` | Experimental Blender add-on for importing and exporting NovaLogic 3DI models and their ADM/BAD animations. |
| `fixtures/` | Test fixtures: synthetic files minted by `tests/fixtures/*_gen.cpp` (the 3DI model set under `fixtures/threedi/synth/`, terrain, fonts, sound banks) plus a small retail-interop keep set (`fixtures/README.md`). |
| `tests/` | Native CTest suite. |
| `launcher/` | Windows launcher and its .NET tests. |
| `web/` | NovaWorld web portal. |
| `deploy/`, `infra/`, `backend/` | Local and hosted service infrastructure. |

Runtime asset loading is native and the supported object input is 3DI. The
experimental [Blender add-on](tools/blender/opennova_3di/README.md) imports and
exports 3DI models and their ADM/BAD animations for authoring, using the
bundled native `opennova-3di` readers and writers. See its documentation for
round-trip limitations.

## Build and test

Prerequisites are CMake, a C++17 compiler, Git LFS, and Git submodules. Godot
4.6.1 is required for the game and GDScript suite; .NET 8 is required for the
Windows launcher.

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

See [DEVELOPING.md](DEVELOPING.md) for the GDExtension, servers, launcher, and
retail-compatibility development workflows.

## Run OpenNova

Build the GDExtension, point `GODOT_BIN` at Godot 4.6.1, and supply the path
to your game data:

```bash
$GODOT_BIN --path godot -- --resource-dir "C:/Games/Joint Operations"
```

Packaged Windows runtime:

```text
opennova.exe -- --resource-dir "C:\Games\Joint Operations"
```

For loose data without boot archives, add `--loose-root /d`. `/game <code>`
selects the game (default `jo`); `/exp <name>` selects an expansion. The data
directory is required on every launch and is never saved. Missing input prints
usage and exits with code 2; an invalid or unmountable directory exits with code 1.

## Downloads

CI and tagged releases publish `opennova-game-windows-v<version>.zip`. It
contains `opennova.exe`, the matching native dependencies, and launch instructions.
Game data is supplied separately. Debug builds include the game's F3 tools.

A PR's CI build comment also links `opennova-blender-addon-windows.zip`.
Tagged [releases](https://github.com/opennova-net/opennova/releases) include
`opennova-blender-addon-windows-v<version>.zip`. Install that zip directly in
Blender 4.2 or newer using **Preferences > Get Extensions > Install from Disk**.
The add-on currently supports Windows x64.

## Documentation

Start with [docs/README.md](docs/README.md) for the architecture, format notes,
reverse-engineering records, and current implementation status. Deployment is
documented separately in [DEPLOY.md](DEPLOY.md).

## License

See [LICENSE](LICENSE).
