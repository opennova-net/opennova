# OpenNova

Open-source toolchain and runtime for NovaLogic's Joint Operations (JO) and newer games. Extract and edit assets with the importer, Blender addon, and OpenNova Editor (ONED). Load them in a C++ reimplementation of the game engine, hosted in Godot.

![OpenNova screenshot](https://i.imgur.com/MNtveOj.jpeg)

## Architecture

Three layers:

- **Authoring (`godot/modtools/`).** The OpenNova Editor (ONED): terrain, environment, etc that writes the game's canonical data formats (`.trn`, `.cpt`, `.til`, `.env`, …) directly.
- **Core engine (`libs/`).** Format parsers, terrain LOD, foliage scatter, environment sampling. Also consumed by Python (`apps/onimport/`) and Blender (`blender/`).
- **Godot (`godot/engine/` + `godot/game/`).** GDExtension wrappers in `engine/` bind the core into Godot; `game/` is the runtime scene.

All of this is pre-1.0 and under active development. Nothing here is production-ready. The asset pipeline (importer, Blender addon, and ONED) is the most exercised surface today; the Godot runtime loads exported scenes and runs the terrain and foliage systems; gameplay (player, missions, multiplayer) is still being built.

Pre-JO NovaLogic titles may sort of work by chance, but are not officially supported.

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

- **`onimport.exe`**: standalone Windows importer. Use this to convert models etc to modern formats like .glb, .blend and .fbx. Also exports files
compat. with NovaLogic's original tools (ie: OED).
- **`opennova_blender.zip`**: Blender 5.x addon with pre-built native libraries for Windows and Linux.
- **`opennova_max-v<version>.mzp`**: 3ds Max importer plugin. Drag into 3ds Max, restart Max, then open `OpenNova > Importer...`.
- **`opennova-modtools-windows.zip`**: standalone OpenNova Editor (ONED) for Windows. The open source NILE.
- **`opennova-runtime-windows.zip`**: "game" runtime.

## Exporter (confusingly called the importer sometimes)

Extract models from game files. Reads directly from PFF archives with automatic decryption and decompression. Just launch `onimport.exe`, point it at your game directory, and select what to export.

Each import can write one or more selected output files:
- `.blend` - Blender project with the full scene hierarchy
- `.ase` - 3DS Max ASCII Scene Export
- `.3dp` / `.3da` - Project metadata for round-trip editing
- `.glb` / `.fbx` - Optional runtime/interchange exports

Imported scenes include meshes, materials, textures, LODs, skeletal armatures, collision volumes, occlusion geometry, lights, and user points.

### Running from source

Requires Python 3.11, uv, and bpy 5.x. Build the shared library (see [Building](#building)) and copy `build/Release/opennova.dll` into `pyopennova/lib/windows-x64/`, then `uv sync && uv run onimport`.

## Blender Addon

A Blender 5.x addon for the export side of the asset pipeline. Install via the zip from [Releases](../../releases) or point Blender at the `blender/` directory for development.

### ASE Export

`File > Export > Novalogic ASE (.ase)`

Exports the current visible scene or LOD to NovaLogic's ASCII Scene Export format. Supports bone weights for skinned models, vertex normals, texture coordinates, lights, and texture export as TGA. Configurable float precision and scale.

### Animation Export

`File > Export > Novalogic Anims (.adm + .bad)`

Exports armature NLA actions to NovaLogic's BAD (bone animation data) and ADM (animation definition) formats. Select which clips to include and configure per-clip flags: loop, root translation, and reset pose. Each clip writes a `.bad` file; the combined `.adm` maps action names to clips.

### Mixamo FBX Import

`File > Import > Mixamo FBX for Novalogic (.fbx)`

Imports Mixamo FBX animations and retargets them onto a NovaLogic skeleton. Strips the `mixamorig:` prefix, optionally creates a root motion bone, and pushes each animation as an NLA strip ready for export.

## OpenNova Editor (ONED)

The authoring layer for JO assets: terrain, environments, and objects in one editor. Sculpt heightmaps, paint surface types, scatter foliage, and bake straight to the game's data formats (`.trn`, `.cpt`, `.til`, `.env`). The packaged build launches into the terrain editing scene (`godot/modtools/terrain/terrain_editor.tscn`).

## Repo Layout

| Path | Contents |
|------|----------|
| `libs/` | C/C++ format libraries (`adm`, `ase`, `bad`, `bfc1`, `cpt`, `def`, `env`, `pcx`, `pff`, `scr`, `tdp`, `threedi`, `til`, `tpj`, `trn`) plus runtime subsystems (`terrain`, `foliage`, `runtime`). |
| `apps/onimport/` | Python importer + scene builder; backs `onimport.exe`. |
| `blender/` | Blender 5.x addon (export side of the pipeline). |
| `godot/` | Godot 4.6.1 host. `engine/` (GDExtension bindings to `libs/`), `modtools/` (authoring), `game/` (runtime scene), `tests/` (GUT suite). |
| `scripts/` | Build, test, and packaging scripts (sh + ps1). |
| `tests/` | C++ test suite (ctest). Godot tests live under `godot/tests/`. |
| `third_party/` | Vendored deps: godot-cpp, gut, minhook, imgui. |

**Conventions.** Format libraries use `opennova_<domain>` CMake target names and the `opennova` C++ namespace; C ABI exports stay flat and domain-prefixed for FFI stability. The shared library target is `opennova_shared`, which bundles the core statics into `opennova.dll` / `libopennova.so`. Blender custom properties owned by this project use `opennova_*` keys.

## C/C++ Libraries

Modular libraries for parsing and writing NovaLogic formats. All expose a C API suitable for FFI.

| Library | Format | Description |
|---------|--------|-------------|
| **threedi** | `.3di` | 3D models: geometry, materials, part animations, collision, occlusion. GP and 3DI3 formats. |
| **ase** | `.ase` | ASCII Scene Export: read/write 3DS Max scene files. |
| **tdp** | `.3dp`/`.3da` | Project files: material definitions, LOD settings, part animation metadata. |
| **bad** | `.bad` | Skeletal animation: bone hierarchies, quaternion keyframes, events. |
| **adm** | `.adm` | Animation definitions: key/value metadata mapping actions to BAD files. |
| **def** | `.def` | Game definitions: weapons, items, ammo, HUD configuration. |
| **pff** | `.pff` | Archive containers: PFF3, PFF4, BHD variants. |
| **bfc1** | | BFC1 decompression (zlib-based) |
| **scr** | | SCR decryption (multiple keys for different game editions) |

## Building

### Prerequisites

- CMake 3.16+
- C++ compiler with C++17 support
- Python 3.11 and uv for importer tooling and Python tests
- Godot 4.6.1 (only for Godot work). Set `GODOT_BIN` or drop the binary in `.godot-bin/`. `scripts/package_godot_windows.ps1` will fetch it automatically when packaging Windows builds.

### Build and Test

```bash
scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPENNOVA_ENABLE_PYTHON_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release
```

### Package Blender Addon

Builds native libraries for Linux and Windows (via MinGW cross-compile), then packages the addon as a zip.

```bash
scripts/package_addon.sh
```

### Package Standalone Importer

Builds `onimport.exe` for Windows using PyInstaller. Requires Python 3.11.

```powershell
scripts/package_importer_windows.ps1
```

### Package 3ds Max Plugin

Builds `opennova_max-v<version>.mzp` for Windows. Requires CMake and Python, but does not require 3ds Max.

```powershell
scripts/package_max_mzp.ps1
```

### Build the GDExtension

```bash
cmake -S godot/engine -B build-godot -DCMAKE_BUILD_TYPE=Release
cmake --build build-godot --config Release --target opennova
```

Outputs `godot/bin/libopennova.<platform>.template_debug.x86_64.{dll,so}`. Open `godot/project.godot` in Godot to load the editor with the extension available.

### Run the Godot tests

```bash
scripts/test_godot.sh
```

Runs the GDScript suite under `godot/tests/` headless via GUT. Requires `GODOT_BIN` set, or a Godot 4.6.1 binary in `.godot-bin/`. `scripts/bootstrap_godot.sh` installs the bundled GUT plugin into `godot/addons/gut/`.

### Package Godot Exports

Builds `opennova-runtime-windows.zip` and `opennova-modtools-windows.zip` via headless Godot export. Windows-only; requires MSVC and CMake.

```powershell
scripts/package_godot_windows.ps1
```

## License

MIT License. See [LICENSE](LICENSE) for details.
