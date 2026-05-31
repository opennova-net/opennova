# OpenNova

OpenNova is a faithful reimplementation of NovaLogic's game engine, aiming for feature and visual parity with the originals. The engine is data driven: it runs off the same asset data the games shipped, which is what made the NovaLogic engine so moddable. The portable core is C++; Godot is the chosen host for rendering, tooling, and the editor. Joint Operations (JO) is the first game we are bringing up.

This repo is the full toolchain: extract and edit assets with the importer, the Blender and 3ds Max plugins, and the OpenNova Editor (ONED), then load them in the engine, hosted in Godot. See [GOALS.md](GOALS.md) for the full vision.

## Screenshots

<p align="center">
  <a href="screenshots/overview.png">
    <img src="screenshots/overview.png" alt="OpenNova editor overview" width="900">
  </a>
</p>

| Asset importer | Object editing |
|---|---|
| [<img src="screenshots/importer.png" alt="OpenNova asset importer" width="420">](screenshots/importer.png) | [<img src="screenshots/object.png" alt="Object editing in OpenNova" width="420">](screenshots/object.png) |
| Font editing | Credits editing |
| [<img src="screenshots/fonts.png" alt="Font editing in OpenNova" width="420">](screenshots/fonts.png) | [<img src="screenshots/credits.png" alt="Credits editing in OpenNova" width="420">](screenshots/credits.png) |
| Strings editing | |
| [<img src="screenshots/strings.png" alt="Strings editing in OpenNova" width="420">](screenshots/strings.png) | |

## Architecture

Three layers:

- **Authoring (`godot/modtools/`).** The OpenNova Editor (ONED): workspaces for terrain, objects, fonts, credits, strings, and environment that write the game's canonical data formats (`.trn`, `.cpt`, `.til`, `.3di`, `.fnt`, `.kda`, `.env`, …) directly.
- **Core engine (`libs/`).** Format parsers, terrain LOD, foliage scatter, environment sampling. Also consumed by Python (`opennova_blender/`, `apps/importer/`) and Blender (`blender/`).
- **Godot (`godot/engine/` + `godot/game/`).** GDExtension wrappers in `engine/` bind the core into Godot; `game/` is the runtime scene.

All of this is pre-1.0 and under active development. Nothing here is production-ready. The asset pipeline (importer, Blender addon, and ONED) is the most exercised surface today; the Godot runtime loads exported scenes and runs the terrain and foliage systems; gameplay (player, missions, multiplayer) is still being built.

Pre-JO NovaLogic titles may sort of work by chance, but are not officially supported.

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

| Asset | What it is for | Install and use |
|---|---|---|
| **`opennova-asset-importer-windows-v<version>.exe`** | Standalone Windows importer for converting models to `.blend`, `.ase`, `.max`, and NovaLogic-compatible project files for tools like OED. | Run the exe directly, point it at your game directory, and select what to export. |
| **`opennova-blender-ase-exporter-v<version>.zip`** | Blender 5.x ASE exporter addon with pre-built native libraries for Windows and Linux. | Install from Blender with `Edit > Preferences > Add-ons > Install`, then use `File > Export > Novalogic ASE (.ase)`. |
| **`opennova-3ds-max-ase-exporter-windows-v<version>.mzp`** | 3ds Max plugin installer that adds NovaLogic ASE export. | Run the MZP in 3ds Max, restart 3ds Max, then use `File > Export > Novalogic ASE (.ase)`. |
| **`opennova-modding-editor-windows-v<version>.zip`** | Standalone OpenNova Editor (ONED) for authoring terrain, environment, tile, and related mod data. | Extract the zip, then run `opennova-modtools.exe`. |
| **`opennova-game-runtime-windows-v<version>.zip`** | Godot-hosted OpenNova runtime for loading exported scenes and runtime systems. | Extract the zip, then run `opennova.exe`. |
| **`opennova-modding-editor-macos-v<version>.zip`** | The ONED editor as a universal macOS app (Apple Silicon and Intel). | Unzip, move the `.app` to Applications, then open it. The app is ad-hoc signed, not notarized: right-click then `Open` the first time, or run `xattr -dr com.apple.quarantine` on the `.app`. |
| **`opennova-game-runtime-macos-v<version>.zip`** | The OpenNova runtime as a universal macOS app (Apple Silicon and Intel). | Unzip, move the `.app` to Applications, then open it. Clear Gatekeeper the same way as the editor app. |

## Asset Importer

Extract models from game files. Reads directly from PFF archives with automatic decryption and decompression. Launch `opennova-asset-importer-windows-v<version>.exe`, point it at your game directory, and select what to export.

Each import can write one or more selected output files:
- `.blend` - Blender project with the full scene hierarchy
- `.ase` - 3DS Max ASCII Scene Export
- `.max` - 3ds Max scene, when the external Max backend is available
- `.3dp` - Object project metadata for round-trip editing

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

The authoring layer for JO assets, organized into workspaces grouped by purpose:

- **World**: Terrain (sculpt, paint, foliage, tiles, layout), Object (`.3di` model projects), and Mission (planned).
- **Interface**: Fonts (`.fnt` bitmap fonts), Credits (`.kda` rolling credits), and Strings (RTXT string tables).
- **Atmosphere**: Environment (`.env` weather, lighting, and time of day), a popup that overlays the active 3D view.

Each workspace reads and writes the game's canonical formats directly. The packaged build opens to the Terrain workspace by default. See [`godot/modtools/README.md`](godot/modtools/README.md) for per-workspace docs and the editor's code-first framework.

## Repo Layout

| Path | Contents |
|------|----------|
| `libs/` | C/C++ format libraries (`adm`, `ase`, `bad`, `bfc1`, `cbin`, `cpt`, `def`, `env`, `fnt`, `pcx`, `pff`, `rtxt`, `scr`, `tdp`, `threedi`, `til`, `tpj`, `trn`) plus runtime and support subsystems (`foliage`, `oed`, `renderer`, `resource_index`, `terrain`). |
| `apps/importer/` | `onimport` launcher and CLI compatibility shell. |
| `opennova_jobs/` | Host-neutral import request/result/job models and validation. |
| `opennova_qt_ui/` | Host-agnostic PySide6 importer dialog and pure UI helpers. |
| `opennova_blender/` | Standalone Blender-backed importer backend for the Qt UI. |
| `opennova_max/` | External 3ds Max batch helpers and Max-side export hooks. |
| `blender/` | Blender 5.x addon (export side of the pipeline). |
| `godot/` | Godot 4.6.1 host. `engine/` (GDExtension bindings to `libs/`), `modtools/` (the [OpenNova Editor](godot/modtools/README.md)), `game/` (runtime scene), `tests/` (GUT suite). |
| `scripts/` | Build, test, and packaging scripts (sh + ps1). |
| `tests/` | C++ test suite (ctest). Godot tests live under `godot/tests/`. |
| `third_party/` | Vendored deps: godot-cpp, gut, modsuperoed. |

**Conventions.** Format libraries use `opennova_<domain>` CMake target names and the `opennova` C++ namespace; C ABI exports stay flat and domain-prefixed for FFI stability. The shared library target is `opennova_shared`, which bundles the core statics into `opennova.dll` / `libopennova.so`. Blender custom properties owned by this project use `opennova_*` keys.

## C/C++ Libraries

Modular libraries for the NovaLogic formats and runtime systems. The format parsers expose a flat, domain-prefixed C ABI for FFI; the runtime subsystems are portable C++ shared by the engine and editor.

### Format parsers and codecs

| Library | Format | Description |
|---------|--------|-------------|
| **threedi** | `.3di` | 3D models: geometry, materials, part animations, collision, occlusion. GP and 3DI3 formats. |
| **ase** | `.ase` | ASCII Scene Export: read/write 3DS Max scene files. |
| **tdp** | `.3dp` | Object projects: material definitions, LOD settings, part-animation metadata. |
| **bad** | `.bad` | Skeletal animation: bone hierarchies, quaternion keyframes, events. |
| **adm** | `.adm` | Animation definitions: key/value metadata mapping actions to BAD files. |
| **def** | `.def` | Game definitions: weapons, items, ammo, HUD configuration. |
| **pff** | `.pff` | Archive containers: PFF3, PFF4, and BHD variants. |
| **pcx** | `.pcx` | PCX (ZSoft Paintbrush) indexed images. |
| **fnt** | `.fnt` | Bitmap fonts (FNT0): glyph pages, per-glyph metrics, shadow offset. |
| **rtxt** | `.bin` | RTXT localized string tables (sectioned key/text entries). |
| **env** | `.env` | Environment: fog, sky, lighting, water, and time-of-day keyframes. |
| **trn** | `.trn` | Terrain runtime config: maps, foliage, sectors, water. |
| **til** | `.til` | Tile-overlay placement list. |
| **cpt** | `.cpt` | Compiled terrain mesh, collision and render (DPTH and CDEP flavors). |
| **tpj** | `.tpj` | Editable terrain project: a terrain config plus editor lock coordinates and project metadata. |
| **cbin** | `.kda` | Rolling credits: obfuscated text compiled to a CBIN blob. |
| **scr** | | SCR decryption (multiple keys for different game editions). |
| **bfc1** | | BFC1 decompression (zlib-based). |

### Runtime and support subsystems

| Library | Description |
|---------|-------------|
| **terrain** | Terrain core: heightmap sampling, normals, sector mesh geometry, LOD. |
| **foliage** | Procedural foliage scatter from the foliage map, distance cull, dispatch. |
| **renderer** | Material classification and per-vertex/object light evaluation shared by runtime and editor. |
| **resource_index** | Indexes asset files under a root directory by kind, for the editor's Open dialogs. |
| **oed** | OED export session: parse an ASE scene once, then export and re-export to `.3di`. |

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

Builds `dist/onimport-v<version>.exe` for Windows using PyInstaller. Requires Python 3.11.

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

Builds `dist/opennova-runtime-windows-v<version>.zip` and `dist/opennova-modtools-windows-v<version>.zip` via headless Godot export. Windows-only; requires MSVC and CMake.

```powershell
scripts/package_godot_windows.ps1
```

## License

MIT License. See [LICENSE](LICENSE) for details.
