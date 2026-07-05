# libs/ — portable C++ core

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `libs/`.
  Godot binding code lives only in `godot/engine/`. Blender-only scene assembly lives in
  `apps/importer/scene_builder.py` and `blender/`.
- Layout per library: `libs/<domain>/{CMakeLists.txt, include/<domain>/, src/}`; CMake
  target `opennova_<domain>`; namespace `opennova`. C ABI exports stay flat and
  domain-prefixed — Python and Godot load the same `opennova_shared` library, so ABI
  stability matters.
- C ABI conventions for NEW exports: annotate with the lib's `<DOMAIN>_EXPORT` macro
  (a per-lib alias of `OPENNOVA_API` from `io/export.h` — never copy the raw
  `__declspec` block again), return `int` status with `0 = success`, out-params last,
  ownership released by a matching `<domain>_free`/`_destroy`. Existing exports keep
  their historical semantics (some predate this — `opennova_vfs_*` returns 1=success,
  `oed` uses a status enum); changing a shipped export's return semantics is an FFI
  behavior change and needs a deliberate, versioned decision.
- Shared infrastructure lives in `libs/io` (`opennova::io` / `opennova::strutil`,
  header-only): bounds-checked `ByteReader`/`ByteWriter`, LSB-first `BitReader`/
  `BitWriter`, `io/le.h` primitives, `io/fixed.h` (16.16 / 2.14), `io/strutil.h`
  ASCII case-insensitive helpers. Do not hand-roll a new byte reader or export-macro
  block; migrate existing per-lib copies on-touch (delegate the body, keep the local
  signature, gated on that lib's byte-exact roundtrip tests). Excluded from migration:
  the `mus`/`wac` VM program-counter cursors (witnessed faithful-port surface with
  their own clamp semantics).
- Ports are faithful structural translations of the original engine — implementing "our
  own version" of engine behavior is never allowed unless a tracked decision (ADR or an
  RE-record divergence entry) says otherwise. CRT/OS/platform primitives (strcpy/sprintf/
  memcpy, D3D, file I/O) are excluded — use standard equivalents. Cite the original inline
  at the port site: `[orig: Name @ 0xADDR]`. Engine-wide conventions: docs/engine-primer.md.
- Parity writers are built from scratch. Never smuggle raw input bytes through a writer to
  turn a parity test green (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
  The full writer-parity gate — writer from scratch + roundtrip test + retail-corpus byte
  sweep where a corpus exists + a ledgered D-entry when output legitimately differs — is
  recorded in docs/oned/workspace-maturity-program.md (F4).
- The protocol libs (`libs/novacrypto`, `libs/napi`, `libs/npwire`, `libs/novaworld`, `libs/netsim`) are
  held to wire compatibility: encoders produce bytes a stock client/server accepts,
  decoders read what a stock client/server emits, and opennova↔opennova requires
  encoder/decoder self-consistency. The witness record is docs/net/novaworld-net-re.md.
- Tests for this code live in `/tests/<domain>/` (ctest), not `godot/tests/`.
- 3DI models: modern `.3di` (3DI3) parses via `threedi_3di3_read`; legacy GP-era `.3di`
  (GPM/GPS/GPP) via `threedi_gp_read`. `ThreediModelIR`
  (libs/threedi/include/threedi/threedi_ir.h) is the unified in-memory representation
  both promote into; `threedi_ir_read` dispatches on file magic
  (libs/threedi/src/threedi_ir.cpp) — extend its detect/convert chain for a new variant.
