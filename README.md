# OpenNova

Open-source tools for extracting, converting, and editing 3D assets from Delta Force and other NovaLogic games.

![OpenNova screenshot](https://snaps.screensnapr.io/9192c63)

## Downloads

Pre-built binaries are available on the [Releases](../../releases) page:

- **`onimport.exe`** — Standalone Windows importer. No Python or Blender install required.
- **`opennova_blender.zip`** — Blender 4.2+ addon with pre-built native libraries for Windows and Linux.

## Importer

Extract weapons, vehicles, buildings, and other models from game files. Reads directly from PFF archives with automatic decryption and decompression — just point it at your game directory.

Each import produces:
- `.blend` — Blender project with the full scene hierarchy
- `.ase` — 3DS Max ASCII Scene Export
- `.3dp` / `.3da` — Project metadata for round-trip editing

Imported scenes include meshes, materials, textures, LODs, skeletal armatures, collision volumes, occlusion geometry, lights, and user points.

### Standalone (no Blender install required)

```bash
# List all weapons and items in a game directory
onimport scan --dir "C:\Games\Delta Force"

# Import a single weapon or item
onimport import --dir "C:\Games\Delta Force" --item M16A2 --type weapon --output ./out

# Import a loose .3di file
onimport import-loose --file model.3di --output ./out

# Batch import everything
onimport export-all --dir "C:\Games\Delta Force" --output ./out
```

Run `onimport` with no arguments for the GUI.

### Running from source

Requires Python 3.11 and the native library (see [Building](#building)).

```bash
poetry install

# Build and copy the native library
cmake -S . -B build -DBUILD_SHARED_LIB=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target opennova_shared
mkdir -p blender/lib/windows-x64
cp build/Release/opennova.dll blender/lib/windows-x64/

poetry run onimport scan --dir "C:\Games\Delta Force"
```

## Blender Addon

A Blender 4.2+ addon for the export side of the asset pipeline. Install via the zip from [Releases](../../releases) or point Blender at the `blender/` directory for development.

### ASE Export

`File > Export > Novalogic ASE (.ase)`

Exports the current scene to NovaLogic's ASCII Scene Export format. Supports multi-LOD scenes, BulletLOD collision meshes, bone weights for skinned models, vertex normals, texture coordinates, and diffuse texture export as TGA. Configurable float precision and scale.

### Animation Export

`File > Export > Novalogic Anims (.adm + .bad)`

Exports armature NLA actions to NovaLogic's BAD (bone animation data) and ADM (animation definition) formats. Select which clips to include and configure per-clip flags: loop, root translation, and reset pose. Each clip writes a `.bad` file; the combined `.adm` maps action names to clips.

### Mixamo FBX Import

`File > Import > Mixamo FBX for Novalogic (.fbx)`

Imports Mixamo FBX animations and retargets them onto a NovaLogic skeleton. Strips the `mixamorig:` prefix, optionally creates a root motion bone, and pushes each animation as an NLA strip ready for export.

## C/C++ Libraries

Modular libraries for parsing and writing NovaLogic formats. All expose a C API suitable for FFI.

| Library | Format | Description |
|---------|--------|-------------|
| **threedi** | `.3di` | 3D models — geometry, materials, part animations, collision, occlusion. GP and 3DI3 formats. |
| **ase** | `.ase` | ASCII Scene Export — read/write 3DS Max scene files |
| **tdp** | `.3dp`/`.3da` | Project files — material definitions, LOD settings, part animation metadata |
| **bad** | `.bad` | Skeletal animation — bone hierarchies, quaternion keyframes, events |
| **adm** | `.adm` | Animation definitions — key/value metadata mapping actions to BAD files |
| **def** | `.def` | Game definitions — weapons, items, ammo, HUD configuration |
| **pff** | `.pff`/`.bhd` | Archive containers — PFF3, PFF4, BHD variants |
| **bfc1** | | BFC1 decompression (zlib-based) |
| **scr** | | SCR decryption (multiple keys for different game editions) |

## Building

### Prerequisites

- CMake 3.16+
- C++ compiler with C++17 support

### Build and Test

```bash
scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
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

## Documentation

- BAD/ADM animation format notes: `docs/formats/bad-adm-animation.md`

## License

MIT License — see [LICENSE](LICENSE) for details.
