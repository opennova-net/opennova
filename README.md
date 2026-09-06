# OpenNova

OpenNova is an open-source C++ and Godot reimplementation of the Joint
Operations generation of NovaLogic games. The repository contains the game
runtime, native format readers, NovaWorld-compatible services, Godot world authoring and
packaging tools, and the project web and launcher applications.

This is a pre-1.0 project. Implemented systems and known differences from the
retail games are tracked in the [divergence ledger](docs/divergence-ledger.md).

## What is in the repository

| Path | Purpose |
| --- | --- |
| `engine/` | Native formats, runtime, networking, and service code. |
| `godot/` | Godot game, editor tools, project resources, and GDScript tests. |
| `apps/` | Native command-line and NovaWorld service applications. |
| `assets/` | Source-controlled game data consumed by OpenNova. |
| `fixtures/` | Test fixtures: synthetic files minted by `tests/fixtures/*_gen.cpp` (the 3DI model set under `fixtures/threedi/synth/`, terrain, fonts, sound banks) plus a small retail-interop keep set (`fixtures/README.md`). |
| `tests/` | Native CTest suite. |
| `launcher/` | Windows launcher and its .NET tests. |
| `web/` | NovaWorld web portal. |
| `deploy/`, `infra/`, `backend/` | Local and hosted service infrastructure. |

The project does not ship a Python, Qt, Blender, or standalone asset-importer
toolchain. Runtime asset loading is native and the supported object input is
3DI.

Future asset tools will use GLB/GLTF as its scene interchange: GLB/GLTF to 3DI
for runtime assets and 3DI to GLB for editing. That converter is not part of
the current repository. Its scene contract is documented without depending on
importer metadata or DCC custom properties.

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

Build the GDExtension, point `GODOT_BIN` at Godot 4.6.1, then run:

```bash
$GODOT_BIN --path godot
```

Open the world editor to configure native world data, edit time, sky and foliage,
play the selected world, stage retail data, or pack a loose source tree:

```bash
$GODOT_BIN --editor --path godot res://examples/world_preview.tscn
```

See the [world editor guide](godot/addons/opennova_world/README.md) and
[game data workflows](godot/tools/README.md).

## Downloads

Tagged releases publish one Windows game archive named
`opennova-game-windows-v<version>.zip`. It contains `opennova.exe`, the matching
GDExtension, and packed game data. Development archives produced by CI also
contain the game with loose tracked data for testing. Authoring tools live in the
Godot source project.

## Documentation

Start with [docs/README.md](docs/README.md) for the architecture, format notes,
reverse-engineering records, and current implementation status. Deployment is
documented separately in [DEPLOY.md](DEPLOY.md).

## License

See [LICENSE](LICENSE).
