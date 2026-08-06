# engine/ — the engine (portable C++ core)

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `engine/`.
  Godot binding code lives only in `godot/adapter/`. Blender-only scene assembly lives in
  `apps/importer/scene_builder/` and `blender/`.
- Four groups (ADR 0028), directories only — never link groups, namespaces, include-path
  segments, or target-name segments:
  - `base/` — shared substrate and repo plumbing: io, vfs, resource_index, gameprofile,
    pcapio, oned_edit, refs.
  - `formats/` — one library per NovaLogic format (ADR 0024): pff, scr, bfc1, pcx, fnt,
    rtxt, cbin, threedi, tdp, ase, bad, def, avatars, trn, tpj, cpt, til, foliage, env,
    mnu, mns, sbf, lwf, dbf, mus, playersav, oed.
  - `runtime/` — the in-match systems: world, wac, mission, anim, audio, particle,
    renderer, controls, terrain, terrain_query.
  - `net/` — the wire/protocol stack (ADRs 0009–0012, 0019; Model-B-only): novacrypto,
    napi, npwire, novaworld, netsim, npruntime.
- Layout per library: `engine/<group>/<domain>/{CMakeLists.txt, include/<domain>/, src/}`;
  CMake target `opennova_<domain>` (a lib may ship a second split target — `engine/runtime/mission`
  builds `opennova_mission_format` beside `opennova_mission`); namespace `opennova`.
  C ABI exports stay flat and domain-prefixed — Python and Godot load the same
  `opennova_shared` library, so ABI stability matters.
- C ABI conventions for NEW exports: annotate with the lib's `<DOMAIN>_EXPORT` macro
  (a per-lib alias of `OPENNOVA_API` from `io/export.h` — never copy the raw
  `__declspec` block again), return `int` status with `0 = success`, out-params last,
  ownership released by a matching `<domain>_free`/`_destroy`. Existing exports keep
  their historical semantics (some predate this — `opennova_vfs_*` returns 1=success,
  `oed` uses a status enum); changing a shipped export's return semantics is an FFI
  behavior change and needs a deliberate, versioned decision.
- A struct crossing the C ABI is mirrored in Python TWICE — `pyopennova/<x>_ffi.py` AND
  `blender/opennova/<x>_ffi.py`. Any C-side layout change updates BOTH mirrors in the
  same commit and extends the native-stride pin tests (`tests/test_def_ffi_mirror.py`,
  `tests/test_threedi_ffi_mirror.py`) with the new fields. The cross-mirror comparison
  alone proves nothing — the mirrors are copies of each other and have agreed on the
  same stale stride before; only the native-stride pins catch the drift.
- Two consumption models (LIBS-3, ADR 0024). **Model A — the flat C ABI**:
  `opennova_shared` (`opennova.dll` / `libopennova.so`) whole-archives the
  `OPENNOVA_CORE_TARGETS` list and exports ONLY `OPENNOVA_API`-annotated symbols — the
  surface pinned by the `abi_export_identity` ctest baseline. Consumers: the Python FFI
  (`pyopennova`, `apps/importer`) and the DCC plugins. **Model B — C++ static link**:
  `godot/adapter`, the apps, the ctest suite, and the entire net stack link
  `opennova_<domain>` targets directly; no export macro involved. The net/protocol libs
  are Model B ONLY — formally outside the C ABI (ADR 0019; NET-4's forbidden-family
  guard). A lib may mix models: only its annotated functions are Model A (unannotated
  functions stay off the DLL under hidden default visibility). Adding a Model-A export
  is a deliberate decision: annotate it AND bump the `abi_export_identity` baseline in
  the same commit, logged in docs/maturity-program.md.
- Family link groups (LIBS-2, ADR 0024): one-lib-per-format stands; the terrain and
  audio format stacks additionally exist as CMake INTERFACE groups
  (`opennova_terrain_family`, `opennova_audio_family` — `engine/families.cmake`, included
  by both CMake roots) so whole-family consumers (the GDExtension) name the family, not
  twelve targets. Link conveniences only — never physical merges, and never a way around
  `link_graph_check.py`'s forbidden edges; leaf consumers (ctests, apps) keep linking
  exactly the libs they use.
- Shared infrastructure lives in `engine/base/io` (`opennova::io` / `opennova::strutil`,
  header-only): bounds-checked `ByteReader`/`ByteWriter`, LSB-first `BitReader`/
  `BitWriter`, `io/le.h` primitives (including the `append_*_le` vector writers every
  streaming encoder wants), `io/fixed.h` (16.16 / 2.14), `io/log.h` (the diagnostic
  sink), `io/strutil.h` ASCII case-insensitive helpers. Do not hand-roll a new byte
  reader or export-macro block; migrate existing per-lib copies on-touch (delegate the
  body, keep the local signature, gated on that lib's byte-exact roundtrip tests).
- Two byte-cursor CONTRACTS exist on purpose, and a copy is only duplication if it
  matches one of them. `io::ByteReader` is the FORMAT-PARSER contract: a clipped read
  yields 0, the cursor does not advance, and parsing continues, so a file still
  round-trips byte-exactly; `ok()` reports truncation without changing that. A PROTOCOL
  decoder wants the opposite — the first short read poisons the cursor so a truncated
  datagram cannot half-decode into plausible state; that is
  `engine/net/npwire/src/wire_cursor.h`, and it must not be folded into `ByteReader`.
- Migration exceptions, each with its reason (do not "clean these up" casually):
  the `mus`/`wac` VM program-counter cursors are a witnessed faithful-port surface with
  their own clamp semantics. `engine/formats/cpt`'s bit codec and `io/bit_stream.h` have DIVERGED
  since the latter was lifted (cpt's writer carries a normalizing `set_position` and a
  `write_to_file`; its reader carries `remaining_bits`) — adopting the shared one in cpt
  is a real migration needing a CPT-corpus byte diff, not a swap. The byte diff is
  available: `tests/terrain/parametric_parity_test` asserts byte-identical CPT output
  across four fixtures (it is registered but DISABLED in CI for runtime, so run the
  built exe directly whenever you touch the CPT encoder). `engine/runtime/mission`'s BMS `Reader`
  is still its own class; its `has_bytes`/`skip` overflow was fixed in place (2026-07-28,
  W2-7) by phrasing the bound as `count <= size_ - pos_`, so what remains is a mechanical
  migration, not a hardening one.
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
- The protocol libs (`engine/net/novacrypto`, `engine/net/napi`, `engine/net/npwire`, `engine/net/novaworld`, `engine/net/netsim`) are
  held to wire compatibility: encoders produce bytes a stock client/server accepts,
  decoders read what a stock client/server emits, and opennova↔opennova requires
  encoder/decoder self-consistency. The witness record is docs/net/novaworld-net-re.md.
- Tests for this code live in `/tests/<domain>/` (ctest), not `godot/tests/`.
- 3DI models: 3DI3 only, consumed directly (ADR 0027). `threedi_3di3_read` produces
  `Threedi3di3` (engine/formats/threedi/include/threedi/threedi_3di3.h) and that parsed struct IS
  the model every consumer walks — the Godot document edits it in place, `tdp_from_3di`
  generates `.3dp` from it, and the Python FFI mirrors its packed layout. There is no
  intermediate model representation, and the GP-era (GPM/GPS/GPP) reader/writer is gone —
  the format knowledge lives in docs/threedi/3di-gp-format-re.md. Shared derivations are
  3DI3-native helpers in that header (userpoint decode, collision run prefix sums +
  runtime-safety validation, `threedi_3di3_ground_anchor`); load-time fixups (the CFAC
  normal-run resolve) happen at the consumer, where retail's loader performs them.
