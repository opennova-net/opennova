# OpenNova

OpenNova is a faithful reimplementation of NovaLogic's game engine, aiming for feature and visual parity with the originals. The engine is data driven: it runs off the same asset data the games shipped, which is what made the NovaLogic engine so moddable. The portable core is C++; Godot is the chosen shell for rendering and application packaging. Joint Operations (JO) is the first game we are bringing up.

This repo is the full toolchain: inspect and extract assets with the importer, build model sources with Blender and the OpenNova addon, maintain the game's canonical data files under `assets/`, then run them in the Godot-based engine. ONED is the companion utility for loose OpenNova runs and retail compatibility checks. See [GOALS.md](GOALS.md) for the full vision.

## Screenshots

| Asset importer | OpenNova runtime |
|---|---|
| [<img src="screenshots/importer.png" alt="OpenNova asset importer" width="420">](screenshots/importer.png) | [<img src="screenshots/killfeed.png" alt="OpenNova game runtime" width="420">](screenshots/killfeed.png) |

## Architecture

Three layers:

- **Core engine (`engine/`).** Format parsers plus the runtime systems: terrain LOD, foliage scatter, environment sampling, the world substrate with its WAC script VM, BMS event runtime, and AI, skeletal animation, audio selection, and the virtual file system. Also consumed by Python (`opennova_blender/`, `apps/importer/`) and Blender (`blender/`).
- **Godot (`godot/src/` + `godot/game/`).** GDExtension bindings in `src/` bind the engine into Godot; `game/` is the runtime scene.
- **Tools and run controls (`apps/`, `blender/`, `godot/modtools/`).** The importer and Blender addon handle extraction and DCC interchange. ONED stores run settings, runs `opennova.exe` against loose or packed game data, stages that data for retail, and stops the one child it started. It does not author assets.

All of this is pre-1.0 and experimental. Nothing here is production-ready. The engine, importer, Blender interchange, and committed game-data pipeline are the most exercised surfaces today. The Godot runtime loads the game data, runs the terrain, foliage, and environment systems, and simulates missions (WAC scripts, BMS events, AI). Weapons and loadouts, projectile physics and damage, throwables, mounted and emplaced weapons, vehicles, item destruction, optics, and the HUD are ported from the original engine and covered by tests. Multiplayer runs on a wire-compatible in-match protocol with single-player hosted as an in-process listen server, so joins and replication exercise the same path retail clients use. Every one of those systems still carries tracked gaps: the honest per-system status is the [divergence ledger](docs/divergence-ledger.md).

Pre-JO NovaLogic titles may sort of work by chance, but are not officially supported.

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

| Asset | What it is for | Install and use |
|---|---|---|
| **`opennova-asset-importer-windows-v<version>.exe`** | Standalone Windows importer for converting models to `.blend`, `.ase`, and NovaLogic-compatible project files for tools like OED. | Run the exe directly, point it at your game directory, and select what to export. |
| **`opennova-blender-ase-exporter-v<version>.zip`** | Blender 5.x ASE exporter addon with pre-built native libraries for Windows and Linux. | Install from Blender with `Edit > Preferences > Add-ons > Install`, then use `File > Export > Novalogic ASE (.ase)`. |
| **`opennova-game-windows-v<version>.zip`** | The OpenNova game, runnable as shipped: the zip root is a retail-style game dir (`localres.pff` beside `opennova.exe`), and the game boots from its own directory with zero setup. | Extract the zip anywhere, keeping its layout, and run `opennova.exe`. |

Dev builds come from CI rather than releases: every pull request and master build produces **`opennova-windows-v<version>.zip`** with `opennova.exe`, ONED (`opennova-modtools.exe`), and the game's loose sources under `assets/` in one folder. Run `opennova.exe` directly, or use ONED to select loose sources or an existing packed game, run OpenNova against it, stage and run a configured retail JO install, and stop the managed child.

## Asset Importer

Extract models from game files. Reads directly from PFF archives with automatic decryption and decompression. Launch `opennova-asset-importer-windows-v<version>.exe`, point it at your game directory, and select what to export.

Each import can write one or more selected output files:
- `.blend` - Blender project with the full scene hierarchy
- `.ase` - ASCII Scene Export
- `.3dp` - Object project metadata for round-trip editing

Imported scenes include meshes, materials, textures, LODs, skeletal armatures, collision volumes, occlusion geometry, lights, and user points.

### Running from source

Requires Python 3.11, uv, bpy 5.0.0, and PySide6. Build the shared library (see [Building](#building)) and copy `build/Release/opennova.dll` into `blender/lib/windows-x64/`, then `uv sync && uv run onimport`.

## Blender ASE Export Addon

Blender exposes the author-facing export target:

`File > Export > Novalogic ASE (.ase)`

Install the Blender 5.x addon via the zip from [Releases](../../releases) or point Blender at the `blender/` directory for development.

### ASE Export

Exports the current scene to NovaLogic's ASCII Scene Export format. Supports multi-LOD scenes, bone weights for skinned models, vertex normals, texture coordinates, and diffuse texture export as TGA. Configurable float precision and scale.

## ONED

ONED is the small developer utility shipped as `opennova-modtools.exe`. Its settings select a loose or packed game-data directory and, optionally, a retail Joint Operations install. From there it offers three operations:

- **Run OpenNova** launches the sibling `opennova.exe` against the selected loose or packed directory with loose-file override enabled.
- **Stage & Run Retail** stages the selected files and the configured retail runtime into ONED's own data directory, then runs `Jointops.exe` from that staged directory.
- **Stop** ends the one child process ONED owns.

ONED has no workspaces, asset editors, project files, import database, or MCP server. Asset creation remains in source-control-friendly format tools and external DCC applications. The hidden `--pack-game` command is packaging infrastructure used by CI to assemble the packed game zip; it is not an interactive authoring action. See [`godot/modtools/README.md`](godot/modtools/README.md).

## Repo Layout

| Path | Contents |
|------|----------|
| `engine/` | C/C++ engine libraries: format parsers, runtime systems, networking, and tooling support (see [C/C++ Libraries](#cc-libraries)). |
| `docs/` | Tracked architecture and reverse-engineering records; start at [`docs/README.md`](docs/README.md). |
| `apps/` | Native and Python tools: the `onimport` importer CLI, the NovaWorld service (`novaworld_server`), a dev/golden-harness in-match host (`nw_server`, never shipped), the packet pretty-printer (`nw_pp`), and shared socket/pcap helpers (`common/`). |
| `pyopennova/` | Python ctypes FFI layer over the shared `opennova` library, used by the importer and the Python tests. |
| `opennova_jobs/` | Import request/result/job models and validation. |
| `opennova_qt_ui/` | PySide6 importer dialog and pure UI helpers. |
| `opennova_blender/` | Standalone Blender-backed importer backend for the Qt UI. |
| `blender/` | Blender 5.x addon (export side of the pipeline). |
| `godot/` | The Godot 4.6.1 project. `src/` (pure C++ GDExtension bindings), `game/` (the game shell plus the game-level GDScript runtime), `modtools/` ([ONED](godot/modtools/README.md)), `tests/` (GUT suite). |
| `web/` | NovaWorld web portal (Vue 3 + TypeScript): landing, lobbies, admin, downloads. Built in CI and deployed with the service stack. |
| `launcher/` | Windows tray app that points a stock game install at OpenNova's NovaWorld servers via one managed hosts-file entry. |
| `backend/` | NovaWorld service data: migrations and seed data. |
| `deploy/`, `infra/` | Deployment stack for the NovaWorld service (Docker, Terraform); see [DEPLOY.md](DEPLOY.md). |
| `scripts/` | Build, test, and packaging scripts (sh + ps1). |
| `tests/` | C++ test suite (ctest). Godot tests live under `godot/tests/`. |
| `third_party/` | Vendored deps: godot-cpp, gut, and modsuperoed as submodules, plus the in-tree bcrypt and sqlite sources. |

**Conventions.** The engine builds as five group CMake targets (`opennova_formats`, `opennova_base`, `opennova_runtime`, `opennova_net`, `opennova_novaworld_service`); every library shares the `opennova` C++ namespace, and C ABI exports stay flat and domain-prefixed for FFI stability. The shared library target is `opennova_shared`, which bundles the core groups into `opennova.dll` / `libopennova.so`. Blender custom properties owned by this project use `opennova_*` keys.

## C/C++ Libraries

Modular libraries for the NovaLogic formats and runtime systems. The format parsers expose a flat, domain-prefixed C ABI for FFI; the runtime subsystems are portable C++ shared by the game and external tools where useful.

### Format parsers and codecs

| Library | Format | Description |
|---------|--------|-------------|
| **threedi** | `.3di` | 3D models: geometry, materials, part animations (PANM), collision, occlusion. The 3DI3 format, consumed directly (ADR 0027); the GP era is documented only. |
| **ase** | `.ase` | ASCII Scene Export: read/write scene files used by the object pipeline. |
| **tdp** | `.3dp` | Object projects: material definitions, LOD settings, part-animation metadata. |
| **bad** | `.bad` | Skeletal animation: bone hierarchies, quaternion keyframes, events. |
| **adm** | `.adm` | Animation definition maps: anim slots bound to `.bad` clip names, with multi-clip variant rings. |
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
| **cbin** | `.kda` | Rolling credits: obfuscated text compiled to a CBIN blob. |
| **mission** | `.bms` `.mis` | Missions: the binary and text document models, records, document mutation, and the flat C ABI. |
| **aip** | `.aip` | AI profile text (partial port: the two witnessed speed keys; the remainder is a tracked gap). |
| **ptl** | `.ptl` | Particle effects: the effect/emitter definition model, parser, writer, and enum/field tables. |
| **mnu** | `.mnu` | Menu screens: window tree, widgets, and Actions, with a round-trip writer that preserves the format superset. Includes the NovaLogic-flavored XML reader. |
| **mns** | `.mns` | Menu stylesheets: named style variables the menu screens reference. |
| **lwf** | `.lwf` | Sound profiles (LWF1): trigger sets of layered member sounds. |
| **dbf** | `.dbf` | Dialog banks (DLG0): grouped dialog and voice entries. |
| **sbf** | `.sbf` | Sound-buffer banks: the sample banks behind interactive music. |
| **mus** | `.bin` | Interactive-music scripts (SCR0/MU01): parser, compiler, and VM. |
| **bink** | `.bik` | Bink video: the portable BIKi video-only decoder behind the menu movies, with the retail YUV to RGB law (`binkw32.dll`). |
| **score** | `score.ini` | Scoring configuration: the per-game-type table of fixed 452-byte rows behind the end-of-round board. |
| **playersav** | `weapon.sav` | Player-profile weapon file: five profile slots, each with BLUE and RED side blocks (class byte, avatar selection, per-class kit pages) plus a single-player kit page. |
| **foliage** | `.trn` foliage map | Foliage definitions and procedural scatter from the foliage map, distance cull, dispatch. |
| **wac** | `.wac` | Mission scripts, the language front end: lexer, parser/AST, the 165-command table, and the compiled-program model. |
| **scr** | | SCR decryption (multiple keys for different game editions). |
| **sph** | `.sph` | The `/PROFILE` server-log FOURCC chunk container (payload records decode in the net stack). |
| **bfc1** | | BFC1 decompression (zlib-based). |

### Engine runtime

| Library | Description |
|---------|-------------|
| **terrain** | Terrain core: heightmap sampling, normals, sector mesh geometry, LOD. |
| **terrain_query** | The world-to-terrain query seam: the zero-dependency height and coordinate query leaf that `world` links and `terrain` builds on (ADR 0020). |
| **renderer** | Material classification and per-vertex/object light evaluation used by the runtime. |
| **world** | World substrate: entity registry and pools, variable store, the logic tick, and the ported gameplay systems on top of it: AI and the infantry motor, collision and occlusion queries, the weapon FSM/inventory/tables, rounds and ballistics, throwables, vehicle mount and drive, item destruction, spawn selection, zones, and the player view. |
| **wac** | WAC scripting: the compiler (binds script names and vars against the live world) and the bytecode VM (the language front end lives in the wac parser lib). |
| **mission** (runtime half) | The BMS event runtime and mission-to-world promotion over the parsed mission (the `.bms`/`.mis` document model itself lives in the mission format lib). |
| **anim** | Skeletal animation: the evaluator that samples `.bad` clips into per-bone transforms. |
| **particle** | The particle simulator: the emitter integrator and effect scene used by the runtime (the `.ptl` format itself lives in the ptl parser lib). |
| **audio** | Sound-set member-selection state machine used by the runtime. |
| **vfs** | Virtual file system: loose directories and PFF archives behind one lookup, with SCR/BFC1 decode. |
| **gameprofile** | Per-game profiles: one source of truth for game identity, archive keys, and SCR codec policy. |

### Networking

Wire-compatible with the original protocols: our encoders produce bytes a stock client or server accepts, and our decoders read what stock endpoints emit. The witness record is [`docs/net/novaworld-net-re.md`](docs/net/novaworld-net-re.md).

| Library | Description |
|---------|-------------|
| **novacrypto** | CRC-32/MPEG-2 and the NWU/EPASK/URL ciphers behind every NovaWorld exchange. |
| **napi** | NAPI envelope and TLV containers: the checksummed message envelope of the NovaWorld matchmaking protocol. |
| **npwire** | The in-game wire protocol: the in-match message codec and catalog, NWU session framing, and the capture decode chain (ADR 0019). |
| **novaworld** | The NovaWorld matchmaking lib: session state above the framing and the gate (first contact, login, server-browser data). The matchmaking/session persistence service builds separately as `opennova_novaworld_service` (the only sqlite link). |
| **inmatch** | The in-match session boundary (ADR 0036): `opennova::inmatch::Session` owns lifecycle, role policy, fixed-tick banking, and input consumption over an `inmatch::TickTarget`. `netsim` (the World-to-wire bridge, transports, and the in-process loopback behind single-player-as-listen-server) and `npruntime` (the NP server and client state machines and frame loop, ported from the original engine) are its implementation directories, not public libraries. |

### Tooling support

| Library | Description |
|---------|-------------|
| **resource_index** | Indexes asset files under a root directory by kind. |
| **controls** | The input-binding catalog behind the Options controls table. |
| **oed** | OED export session: parse an ASE scene once, then export and re-export to `.3di`. |

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
cmake -S godot/src -B build-godot -DCMAKE_BUILD_TYPE=Release
cmake --build build-godot --config Release --target opennova
```

Outputs `godot/bin/libopennova.<platform>.template_debug.x86_64.{dll,so}`. Open `godot/project.godot` in Godot to load the editor with the extension available.

### Run the Godot tests

```bash
scripts/test_godot.sh
```

Runs the GDScript suite under `godot/tests/` headless via GUT. Requires `GODOT_BIN` set, or a Godot 4.6.1 binary in `.godot-bin/`. `scripts/bootstrap_godot.sh` installs the bundled GUT plugin into `godot/addons/gut/`.

### Package Godot Exports

Builds both Windows zips via headless Godot export: `dist/opennova-windows-v<version>.zip` (the dev build with both exes, their shared GDExtension DLL, and the tracked loose sources under `assets/`) and `dist/opennova-game-windows-v<version>.zip` (the tagged-release build with `opennova.exe` plus `localres.pff`, built from those sources by ONED's hidden `--pack-game` CLI). Windows-only; requires MSVC, CMake, and pulled LFS assets.

```powershell
scripts/package_godot_windows.ps1
```

## License

MIT License. See [LICENSE](LICENSE) for details.
