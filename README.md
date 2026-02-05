# OpenNova

Open-source tools and libraries for working with NovaLogic game file formats.

https://snaps.screensnapr.io/9192c63

## Features

### C Libraries

Libraries for parsing NovaLogic file formats.

- **threedi** — 3DI model files (geometry, materials, part animations, collision models). Supports GP and 3DI3 format.
- **bad** — BAD skeletal animation files (bone hierarchies, quaternion keyframes, animation events)
- **adm** — ADM animation definition files (key/value animation metadata)
- **pff** — PFF archive containers (PFF3, PFF4, BHD format variants)
- **bfc1** — BFC1 decompression (zlib-based)
- **def** — DEF game definition files (weapons, items, ammo, HUD configuration)
- **scr** — SCR decryption (multiple encryption keys for different game editions)

### Blender Addon

A Blender 4.2+ addon for importing and exporting NovaLogic assets.

- Import 3DI models with materials, armatures, and animations
- Resolve assets from loose files or PFF archives (with automatic decryption and decompression)
- Export to ASE (ASCII Scene Export) format
- Windows and Linux support

### Tools

- **3di_dump** — CLI tool for inspecting 3DI binary files in a diff-friendly text format

## Building

### Prerequisites

- CMake 3.16+
- C compiler with C99 support

### Build and Test

```bash
./scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

### Build Shared Library

To build a single shared library (for use by the Blender addon):

```bash
cmake -S . -B build -DBUILD_SHARED_LIB=ON
cmake --build build
```

### Package Blender Addon

```bash
./scripts/package_addon.sh
```

## License

MIT License — see [LICENSE](LICENSE) for details.
