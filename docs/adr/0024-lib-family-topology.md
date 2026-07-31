# ADR 0024: lib family topology — one lib per format, families as link groups

- **Status**: accepted (2026-07-12, rides the Wave-2 trunk PR)
- **Owners**: maturity program LIBS track
- **Supersedes/updates**: executes LIBS-2/LIBS-3 of the maturity umbrella
  (docs/maturity-program.md); records the LIBS-2 clause reversal already
  decided in ADR 0023.

## Context

`libs/` holds ~44 libraries under the tracked one-lib-per-format rule. The
maturity exploration found the count itself is fine; the real topology issues
were one heavy edge (world→terrain, fixed by ADR 0020 / LIBS-1) and two
*families* — the terrain format stack (terrain, terrain_query, cpt, til, trn,
tpj, foliage) and the audio format stack (audio, sbf, mus, lwf, dbf) — whose
members are always linked as a set by whole-family consumers. The GDExtension
(`godot/engine/CMakeLists.txt`) names all twelve individually; the original
LIBS-2 sketch also proposed folding `libs/renderer` away (it was 4 files
dodging one oed header at the time).

Separately, the two ways `libs/` code is consumed had never been named,
which made rules like "net libs are outside the C ABI" (ADR 0019, NET-4's
forbidden-family export guard) read as folklore rather than structure.

## Decision

1. **One-lib-per-format is affirmed.** Each format keeps its own CMake
   target, headers, namespace, fixtures, and tests. No physical merges.
2. **Families are CMake INTERFACE link groups, nothing more**:
   `opennova_terrain_family` and `opennova_audio_family`, defined once in
   `libs/families.cmake` and included by BOTH CMake roots (the repo root and
   `godot/engine`) after every member target exists. They carry no sources,
   no include dirs, no ABI — pure `target_link_libraries(... INTERFACE ...)`
   membership lists.
3. **Adoption is for whole-family consumers only.** The GDExtension links
   the two family targets in place of the twelve member lines. Leaf
   consumers — every ctest, the apps — keep linking exactly the libs they
   use; `OPENNOVA_CORE_TARGETS` (the FFI whole-archive list) keeps naming
   real static archives, since an INTERFACE target has no object files to
   whole-archive.
4. **Families are not an edge laundering mechanism.**
   `scripts/lint/link_graph_check.py` remains authoritative and walks the
   TRANSITIVE closure, so a forbidden consumer linking a family that
   contains a forbidden lib still fails. Nothing under `libs/` links a
   family target; families exist for the shells/apps above the lib layer.
5. **The "fold `libs/renderer`" clause is reversed** (decided in ADR 0023,
   recorded here where the clause originated): REN grew `libs/renderer`
   into the witnessed render library (render_order, light_runtime, uv_anim,
   the object-shader composer) — it is a real domain lib now, not a header
   dodge.
6. **The two consumption models get names** (LIBS-3, operational text in
   `libs/CLAUDE.md`): **Model A** — the flat C ABI via `opennova_shared`,
   exporting only `OPENNOVA_API`-annotated symbols, pinned by the
   `abi_export_identity` baseline; consumers are the Python FFI and the DCC
   plugins. **Model B** — direct C++ static link; the engine, apps, tests,
   and the entire net stack. A lib may mix models: only annotated functions
   are Model A (hidden default visibility keeps everything else off the
   DLL). ADR 0019's "net stays out of the C ABI" is exactly "the net libs
   are Model B only".

## Consequences

- `godot/engine/CMakeLists.txt` names `opennova_terrain_family` +
  `opennova_audio_family`; adding a lib to a family updates every
  whole-family consumer in one place.
- A new format lib joins a family by membership edit in
  `libs/families.cmake` (the new-format-lib scaffold stays unchanged —
  family membership is opt-in, not part of the per-lib template).
- New engine-side surfaces default to Model B; Model A additions are
  deliberate (annotate + same-commit `abi_export_identity` baseline bump,
  logged in docs/maturity-program.md's bump table).
- The link-graph check gains no new rules from this ADR: the forbidden-edge
  set is unchanged, and families introduce no lib→lib edges.
