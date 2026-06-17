# libs/ — portable C++ core

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `libs/`.
  Godot binding code lives only in `godot/engine/`. Blender-only scene assembly lives in
  `apps/importer/scene_builder.py` and `blender/`.
- Layout per library: `libs/<domain>/{CMakeLists.txt, include/<domain>/, src/}`; CMake
  target `opennova_<domain>`; namespace `opennova`. C ABI exports stay flat and
  domain-prefixed — Python and Godot load the same `opennova_shared` library, so ABI
  stability matters.
- Ports are faithful structural translations of the original engine — implementing "our
  own version" of engine behavior is never allowed unless a tracked decision (ADR or an
  RE-record divergence entry) says otherwise. CRT/OS/platform primitives (strcpy/sprintf/
  memcpy, D3D, file I/O) are excluded — use standard equivalents. Cite the original inline
  at the port site: `[orig: Name @ 0xADDR]`. Engine-wide conventions: docs/engine-primer.md.
- Parity writers are built from scratch. Never smuggle raw input bytes through a writer to
  turn a parity test green (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
- Tests for this code live in `/tests/<domain>/` (ctest), not `godot/tests/`.
- 3DI models: modern `.3di` (3DI3) parses via `threedi_3di3_read`; legacy GP-era `.3di`
  (GPM/GPS/GPP) via `threedi_gp_read`. `ThreediModelIR`
  (libs/threedi/include/threedi/threedi_ir.h) is the unified in-memory representation
  both promote into; `threedi_ir_read` dispatches on file magic
  (libs/threedi/src/threedi_ir.cpp) — extend its detect/convert chain for a new variant.
