# OpenNova

OpenNova is a faithful reimplementation of NovaLogic's game engine, aiming for feature and visual parity with the originals. The engine is data driven: it runs off the same asset data the games shipped, which is what made the NovaLogic engine so moddable. The portable core is C++; Godot is the chosen shell for rendering, tooling, and the editor. Joint Operations (JO) is the first game we are bringing up.

This repo is the full toolchain: extract and edit assets with the importer, the Blender and 3ds Max plugins, and the OpenNova Editor (ONED), then load them in the Godot-based engine runtime. See [GOALS.md](GOALS.md) for the full vision.

## Screenshots

| Asset importer | Terrain workspace |
|---|---|
| [<img src="screenshots/importer.png" alt="OpenNova asset importer" width="420">](screenshots/importer.png) | [<img src="screenshots/overview.png" alt="Terrain editing in OpenNova" width="420">](screenshots/overview.png) |
| Object workspace | Mission workspace |
| [<img src="screenshots/object.png" alt="Object editing in OpenNova" width="420">](screenshots/object.png) | [<img src="screenshots/mission.png" alt="Mission editing in OpenNova" width="420">](screenshots/mission.png) |
| Fonts workspace | Credits workspace |
| [<img src="screenshots/fonts.png" alt="Font editing in OpenNova" width="420">](screenshots/fonts.png) | [<img src="screenshots/credits.png" alt="Credits editing in OpenNova" width="420">](screenshots/credits.png) |
| Strings workspace | Menus workspace |
| [<img src="screenshots/strings.png" alt="Strings editing in OpenNova" width="420">](screenshots/strings.png) | [<img src="screenshots/menus.png" alt="Menu editing in OpenNova" width="420">](screenshots/menus.png) |
| Music workspace | Sound workspace |
| [<img src="screenshots/music.png" alt="Music editing in OpenNova" width="420">](screenshots/music.png) | [<img src="screenshots/sound.png" alt="Sound editing in OpenNova" width="420">](screenshots/sound.png) |
| Avatars workspace | HUD workspace |
| [<img src="screenshots/avatars.png" alt="Avatar editing in OpenNova" width="420">](screenshots/avatars.png) | [<img src="screenshots/hud.png" alt="HUD layout preview in OpenNova" width="420">](screenshots/hud.png) |
| Environment workspace | Particles workspace |
| [<img src="screenshots/environment.png" alt="Environment editing in OpenNova" width="420">](screenshots/environment.png) | [<img src="screenshots/particles.png" alt="Particle editing in OpenNova" width="420">](screenshots/particles.png) |

## Architecture

Three layers:

- **Authoring (`godot/modtools/`).** The OpenNova Editor (ONED): workspaces for terrain, objects, missions, avatars, fonts, credits, strings, menus, HUD preview, music, particles, sound, and environment that write the game's canonical data formats (`.trn`, `.cpt`, `.til`, `.3di`, `.bms`, `.mis`, `Avatars.def`, `.fnt`, `.kda`, `.mnu`, `.sbf`, `.ptl`, `.lwf`, `.env`, …) directly.
- **Core engine (`libs/`).** Format parsers plus the runtime systems: terrain LOD, foliage scatter, environment sampling, the world substrate with its WAC script VM, BMS event runtime, and AI, skeletal animation, audio selection, and the virtual file system. Also consumed by Python (`opennova_blender/`, `apps/importer/`) and Blender (`blender/`).
- **Godot (`godot/engine/` + `godot/game/`).** GDExtension wrappers in `engine/` bind the core into Godot; `game/` is the runtime scene.

All of this is pre-1.0 and experimental. Nothing here is production-ready. The asset pipeline (importer, Blender addon, and ONED) is the most exercised surface today. The Godot runtime loads exported scenes, runs the terrain, foliage, and environment systems, and simulates authored missions (WAC scripts, BMS events, AI). On-foot play is coming up: weapons and loadouts, projectile physics and damage, throwables, mounted and emplaced weapons, vehicles, item destruction, optics, and the HUD are ported from the original engine and covered by tests. Multiplayer runs on a wire-compatible in-match protocol with single-player hosted as an in-process listen server, so joins and replication exercise the same path retail clients use. Every one of those systems still carries tracked gaps: the honest per-system status is the [divergence ledger](docs/divergence-ledger.md).

Pre-JO NovaLogic titles may sort of work by chance, but are not officially supported.

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

| Asset | What it is for | Install and use |
|---|---|---|
| **`opennova-asset-importer-windows-v<version>.exe`** | Standalone Windows importer for converting models to `.blend`, `.ase`, `.max`, and NovaLogic-compatible project files for tools like OED. | Run the exe directly, point it at your game directory, and select what to export. |
| **`opennova-blender-ase-exporter-v<version>.zip`** | Blender 5.x ASE exporter addon with pre-built native libraries for Windows and Linux. | Install from Blender with `Edit > Preferences > Add-ons > Install`, then use `File > Export > Novalogic ASE (.ase)`. |
| **`opennova-3ds-max-ase-exporter-windows-v<version>.mzp`** | 3ds Max plugin installer that adds NovaLogic ASE export. | Run the MZP in 3ds Max, restart 3ds Max, then use `File > Export > Novalogic ASE (.ase)`. |
| **`opennova-modding-editor-windows-v<version>.zip`** | Standalone OpenNova Editor (ONED) for authoring terrain, object, mission, interface, audio, and environment mod data. | Extract the zip, then run `opennova-modtools.exe`. |
| **`opennova-game-runtime-windows-v<version>.zip`** | Godot-based OpenNova runtime for loading exported scenes and runtime systems. | Extract the zip, then run `opennova.exe`. |
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

- **World**: Terrain (sculpt, paint, foliage, tiles, layout), Object (`.3di` model projects), Mission (`.bms`/`.mis` missions: entities, waypoints, zones, BMS event scripting, saved as loose assets and tested in the standalone game with F5/F6), and Avatars (`Avatars.def` player characters: head/body/arms parts and combos under the nationality/division tree, with a 3D preview).
- **Interface**: Fonts (`.fnt` bitmap fonts), Credits (`.kda` rolling credits), Strings (RTXT string tables), Menus (`.mnu` / `.mns` menu screens with a WYSIWYG canvas and interactive preview), and HUD (a read-only preview of the in-game HUD layout).
- **Audio**: Music (interactive music: `.sbf` banks plus `.bin` music scripts).
- **Atmosphere**: Particles (`.ptl` effects: explosions, smoke, muzzle flashes, water spray), Sound (`.lwf` sound profiles), and Environment (`.env` weather, lighting, and time of day), a popup that overlays the active 3D view.

Each workspace reads and writes the game's canonical formats directly. The packaged build opens to the Terrain workspace by default. See [`godot/modtools/README.md`](godot/modtools/README.md) for per-workspace docs and the editor's code-first framework.

## Repo Layout

| Path | Contents |
|------|----------|
| `libs/` | C/C++ engine libraries: format parsers, runtime systems, networking, and editor support (see [C/C++ Libraries](#cc-libraries)). |
| `docs/` | Tracked architecture and reverse-engineering records; start at [`docs/README.md`](docs/README.md). |
| `apps/` | Native and Python tools: the `onimport` importer CLI, the NovaWorld service (`novaworld_server`), a dev/golden-harness in-match host (`nw_server`, never shipped), the packet pretty-printer (`nw_pp`), the replay streamer (`nw_replay`), and shared socket/pcap helpers (`common/`). |
| `pyopennova/` | Python ctypes FFI layer over the shared `opennova` library, used by the importer and the Python tests. |
| `opennova_jobs/` | DCC-neutral import request/result/job models and validation. |
| `opennova_qt_ui/` | DCC-agnostic PySide6 importer dialog and pure UI helpers. |
| `opennova_blender/` | Standalone Blender-backed importer backend for the Qt UI. |
| `opennova_max/` | External 3ds Max batch helpers and Max-side export hooks. |
| `blender/` | Blender 5.x addon (export side of the pipeline). |
| `godot/` | The Godot 4.6.1 project. `engine/` (GDExtension bindings to `libs/` plus the shared GDScript engine layer both shells run on), `modtools/` (the [OpenNova Editor](godot/modtools/README.md)), `game/` (runtime shell), `server/` (vestigial; dedicated hosting will be a serve mode of the game runtime, see [ADR 0015](docs/adr/0015-two-products-serve-mode.md)), `tests/` (GUT suite). |
| `web/` | NovaWorld web portal (Vue 3 + TypeScript): landing, lobbies, admin, downloads. Built in CI and deployed with the service stack. |
| `launcher/` | Windows tray app that points a stock game install at OpenNova's NovaWorld servers via one managed hosts-file entry. |
| `backend/` | NovaWorld service data: migrations and seed data. |
| `deploy/`, `infra/` | Deployment stack for the NovaWorld service (Docker, Terraform); see [DEPLOY.md](DEPLOY.md). |
| `scripts/` | Build, test, and packaging scripts (sh + ps1). |
| `tests/` | C++ test suite (ctest). Godot tests live under `godot/tests/`. |
| `third_party/` | Vendored deps: godot-cpp, gut, and modsuperoed as submodules, plus the in-tree bcrypt and sqlite sources. |

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
| **def** | `.def` | Game definitions: weapons, items, ammo, HUD configuration. |
| **avatars** | `Avatars.def` | Player-character definitions: head/body/arms parts composed into combos under a nationality/division tree, with a from-scratch round-trip writer. |
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
| **particle** | `.ptl` | Particle effects: the effect/emitter definition parser and writer, plus the emitter integrator and effect scene the runtime and the editor preview share. |
| **mnu** | `.mnu` | Menu screens: window tree, widgets, and Actions, with a round-trip writer that preserves the format superset. Includes the NovaLogic-flavored XML reader. |
| **mns** | `.mns` | Menu stylesheets: named style variables the menu screens reference. |
| **lwf** | `.lwf` | Sound profiles (LWF1): trigger sets of layered member sounds. |
| **dbf** | `.dbf` | Dialog banks (DLG0): grouped dialog and voice entries. |
| **sbf** | `.sbf` | Sound-buffer banks: the sample banks behind interactive music. |
| **mus** | `.bin` | Interactive-music scripts (SCR0/MU01): parser, compiler, and VM. |
| **scr** | | SCR decryption (multiple keys for different game editions). |
| **bfc1** | | BFC1 decompression (zlib-based). |

### Engine runtime

| Library | Description |
|---------|-------------|
| **terrain** | Terrain core: heightmap sampling, normals, sector mesh geometry, LOD. |
| **terrain_query** | The world-to-terrain query seam: the zero-dependency height and coordinate query leaf that `world` links and `terrain` builds on (ADR 0020). |
| **foliage** | Procedural foliage scatter from the foliage map, distance cull, dispatch. |
| **renderer** | Material classification and per-vertex/object light evaluation shared by runtime and editor. |
| **world** | World substrate: entity registry and pools, variable store, the logic tick, and the ported gameplay systems on top of it: AI and the infantry motor, collision and occlusion queries, the weapon FSM/inventory/tables, rounds and ballistics, throwables, vehicle mount and drive, item destruction, spawn selection, zones, and the player view. |
| **wac** | WAC scripting: lexer, parser, compiler, and bytecode VM. |
| **mission** | `.bms`/`.mis` missions: records and schema reflection, the BMS event runtime, and mission-to-world promotion. Two targets: `opennova_mission_format` (parse/write/schema) and `opennova_mission` (event runtime + promotion). |
| **anim** | Skeletal animation: `.adm` definition parsing plus the evaluator that samples `.bad` clips into per-bone transforms. |
| **audio** | Sound-set member-selection state machine shared by the runtime and the editor. |
| **vfs** | Virtual file system: loose directories and PFF archives behind one lookup, with SCR/BFC1 decode. |
| **gameprofile** | Per-game profiles: one source of truth for game identity, archive keys, and SCR codec policy. |

### Networking

Wire-compatible with the original protocols: our encoders produce bytes a stock client or server accepts, and our decoders read what stock endpoints emit. The witness record is [`docs/net/novaworld-net-re.md`](docs/net/novaworld-net-re.md).

| Library | Description |
|---------|-------------|
| **novacrypto** | CRC-32/MPEG-2 and the NWU/EPASK/URL ciphers behind every NovaWorld exchange. |
| **napi** | NAPI envelope and TLV containers: the checksummed message envelope of the lobby protocol. |
| **npwire** | The in-game wire protocol: the in-match message codec and catalog, NWU session framing, and the capture/replay chain (ADR 0019). |
| **novaworld** | The NovaWorld matchmaking and service lib: session state above the framing, gate (first contact, login, server-browser data), and lobby persistence (the only sqlite link). |
| **netsim** | The in-match net seam: the World-to-wire bridge, transports, and the in-process loopback behind single-player-as-listen-server. |
| **npruntime** | The in-match NP server and client state machines and frame loop, ported from the original engine over the netsim seam. |

### Editor and tooling support

| Library | Description |
|---------|-------------|
| **resource_index** | Indexes asset files under a root directory by kind, for the editor's Open dialogs. |
| **refs** | Cross-asset reference graph: which files reference which, with per-format extractors (env, cbin, def, 3di, mission, menus). |
| **controls** | The input-binding catalog behind the Options controls table. |
| **oed** | OED export session: parse an ASE scene once, then export and re-export to `.3di`. |
| **oned_edit** | Shared undo/redo edit-history core for the ONED workspaces. |

### Shared infrastructure

| Library | Description |
|---------|-------------|
| **io** | Header-only shared infrastructure: bounds-checked byte cursors, bit streams, little-endian primitives, fixed-point conversions, and ASCII string helpers the format libraries build on. |

## Building

New to the project? [DEVELOPING.md](DEVELOPING.md) is the full local-development
walkthrough: toolchain setup, building the GDExtension, running the NovaWorld servers
locally, and testing against both retail Joint Operations and our own client. The quick
commands are below.

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
