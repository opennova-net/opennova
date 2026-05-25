# OpenNova

Open-source toolchain and runtime for NovaLogic's Joint Operations (JO) and newer games. Extract and edit assets with the importer, Blender addon, and OpenNova Editor (ONED). Load them in a C++ reimplementation of the game engine, hosted in Godot.

![OpenNova screenshot](https://i.imgur.com/MNtveOj.jpeg)

## Architecture

Three layers:

- **Authoring (`godot/modtools/`).** The OpenNova Editor (ONED): terrain, environment, etc that writes the game's canonical data formats (`.trn`, `.cpt`, `.til`, `.env`, …) directly.
- **Core engine (`libs/`).** Format parsers, terrain LOD, foliage scatter, environment sampling. Also consumed by Python (`opennova_blender/`, `apps/importer/`) and Blender (`blender/`).
- **Godot (`godot/engine/` + `godot/game/`).** GDExtension wrappers in `engine/` bind the core into Godot; `game/` is the runtime scene.

All of this is pre-1.0 and under active development. Nothing here is production-ready. The asset pipeline (importer, Blender addon, and ONED) is the most exercised surface today; the Godot runtime loads exported scenes and runs the terrain and foliage systems; gameplay (player, missions, multiplayer) is still being built.

Pre-JO NovaLogic titles may sort of work by chance, but are not officially supported.

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

- **`onimport.exe`**: standalone Windows importer. Use this to convert models to `.blend`, `.ase`, `.max`, and NovaLogic-compatible project files for tools like OED.
- **`opennova_blender.zip`**: Blender 5.x addon with pre-built native libraries for Windows and Linux.
- **`opennova_max-v<version>.mzp`**: 3ds Max plugin installer that adds NovaLogic ASE export.
- **`opennova-modtools-windows.zip`**: standalone OpenNova Editor (ONED) for Windows. The open source NILE.
- **`opennova-runtime-windows.zip`**: "game" runtime.

## Exporter (confusingly called the importer sometimes)

Extract models from game files. Reads directly from PFF archives with automatic decryption and decompression. Just launch `onimport.exe`, point it at your game directory, and select what to export.

Each import can write one or more selected output files:
- `.blend` - Blender project with the full scene hierarchy
- `.ase` - 3DS Max ASCII Scene Export
- `.max` - 3ds Max scene, when the external Max backend is available
- `.3dp` / `.3da` - Project metadata for round-trip editing

Imported scenes include meshes, materials, textures, LODs, skeletal armatures, collision volumes, occlusion geometry, lights, and user points.

### Running from source

Requires Python 3.11, uv, bpy 5.0.0, and PySide6. Build the shared library (see [Building](#building)) and copy `build/Release/opennova.dll` into `blender/lib/windows-x64/`, then `uv sync && uv run onimport`.

## DCC ASE Export Plugins

Blender and 3ds Max both expose the same author-facing export target:

`File > Export > Novalogic ASE (.ase)`

Install the Blender 5.x addon via the zip from [Releases](../../releases) or point Blender at the `blender/` directory for development. Install the 3ds Max plugin by running the MZP from [Releases](../../releases); it copies an Autodesk ApplicationPlugins bundle under your user profile.

### ASE Export

Exports the current scene to NovaLogic's ASCII Scene Export format. Supports multi-LOD scenes, bone weights for skinned models, vertex normals, texture coordinates, and diffuse texture export as TGA. Configurable float precision and scale.

## OpenNova Editor (ONED)

The authoring layer for JO assets: terrain, environments, and objects in one editor. Sculpt heightmaps, paint surface types, scatter foliage, and bake straight to the game's data formats (`.trn`, `.cpt`, `.til`, `.env`). The packaged build launches into the terrain editing scene (`godot/modtools/terrain/terrain_editor.tscn`).

## Repo Layout

| Path | Contents |
|------|----------|
| `libs/` | C/C++ format libraries (`adm`, `ase`, `bad`, `bfc1`, `cpt`, `def`, `env`, `pcx`, `pff`, `scr`, `tdp`, `threedi`, `til`, `tpj`, `trn`) plus runtime subsystems (`terrain`, `foliage`, `runtime`). |
| `apps/importer/` | `onimport` launcher and CLI compatibility shell. |
| `opennova_jobs/` | Host-neutral import request/result/job models and validation. |
| `opennova_qt_ui/` | Host-agnostic PySide6 importer dialog and pure UI helpers. |
| `opennova_blender/` | Standalone Blender-backed importer backend for the Qt UI. |
| `opennova_max/` | External 3ds Max batch helpers and Max-side export hooks. |
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

### Build ModSuperOED Automation Tools

Headless driver for NovaLogic's ModSuperOED, used for batch `.3di` export
without GUI interaction. The hook DLL and injector build by default when CMake
is configured with a 32-bit Windows toolchain; other toolchains skip them with
a status message. The fixture pack (ModSuperOed.exe and the CharModel
reference files) lives under `third_party/modsuperoed` as a Git LFS submodule,
so a fresh clone needs the submodule initialised and its LFS objects pulled.

```powershell
git submodule update --init --recursive third_party/modsuperoed
git -C third_party/modsuperoed lfs install --local
git -C third_party/modsuperoed lfs pull
cmake -S . -B build-modsuperoed -A Win32
cmake --build build-modsuperoed --config Release --target modsuperoed_injector modsuperoed_hook
$env:OPENNOVA_MODSUPEROED_DIR = "$PWD\third_party\modsuperoed"
uv run --frozen pytest tests/test_modsuperoed_automation.py::test_external_modsuperoed_smoke -q
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
