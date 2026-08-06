# ADR 0027: 3DI3 is the only model format, consumed directly — no model IR

- **Status**: accepted (2026-08-06)
- **Owners**: threedi/model pipeline
- **Supersedes/updates**: retires the `ThreediModelIR` unification layer and the
  GP-era (GPM/GPS/GPP) reader/writer that motivated it.

## Context

`libs/threedi` carried two on-disk model families — modern `3DI3` (JO and
newer) and the GP-era `.3di` container (GPM/GPS/GPP) — and a hand-rolled
"common IR" (`ThreediModelIR`) both promoted into so one scene builder and one
runtime could serve either. Every consumer (the Godot document/runtime, the
DCC importers, the `.3dp` generator) read the IR, and editors wrote it, with
copy/sync steps translating between the IR and the parsed `Threedi3di3` at
open and export time.

That layer earned its keep only while GP support mattered. JO — the title
being brought up — ships exclusively 3DI3 models, and the project's charter is
a faithful reimplementation of the JO-generation engine ([GOALS.md]): the
original engine has no such intermediate representation; its loaders fix up
the parsed records in place and the runtime consumes them directly. Keeping
the IR cost us:

- a full redundant copy of every parsed model (plus IR→raw copy-back sync
  steps at export, and IR→raw conversions at every runtime evaluation seam,
  since `libs/renderer` and the PANM runtime already consume raw structs);
- real fidelity loss at the seam (the IR clamped MTRL to 8 texture slots,
  truncating animated-frame sets; edited IR light types never copied back);
- a second struct vocabulary mirrored twice into Python and re-learned by
  every contributor.

## Decision

1. **3DI3 is the model format, and it is first class.** `Threedi3di3` (the
   typed parse of the file) is the in-memory model every consumer walks:
   the Godot document (`NovaObjectData` edits it in place), the simulation's
   runtime collision/occlusion builders, `libs/tdp`'s `.3dp` generator
   (`tdp_from_3di`), and the Python/DCC importers (ctypes mirrors of the raw
   packed structs).
2. **GP-era formats are not supported.** `threedi_gp_read`/`threedi_gp_write`
   and `ThreediGpFile` are deleted. The GP on-disk format knowledge remains in
   the RE record (`docs/threedi/3di-gp-format-re.md`) — it documents the
   original engine, not our surface.
3. **`ThreediModelIR` is deleted.** Derivations consumers still need live as
   3DI3-native helpers where they are shared, or at the consumer where they
   are load-time fixups the original engine also performs at load:
   - userpoint 16.16 → model-space decode and the placement ground anchor:
     `threedi_user_point_position`/`_direction`, `threedi_3di3_ground_anchor`
     (libs/threedi);
   - per-COBJ collision run prefix sums and runtime-safety validation:
     `threedi_collision_object_runs`, `threedi_3di3_collision_is_runtime_safe`
     (header-inline, off the ABI);
   - the CFAC normal-run resolve and BVOL plane-window prefixes happen in the
     runtime collision build, exactly where retail's loader performs them
     [orig: the per-COBJ normal-run fixup in the collision builder @ 0x5b3bf0];
   - the collision-face → per-material surface-type/pattrib reconstruction
     exists only to generate `.3dp` projects and moved into `libs/tdp`.
4. **C ABI**: the `threedi_ir_*` exports are removed; `threedi_3di3_read` /
   `threedi_3di3_free` are exported for the Python FFI, and `tdp_from_ir`
   becomes `tdp_from_3di` (baseline bumped in the same change, logged in
   docs/maturity-program.md).

## Consequences

- One representation end to end: what the parser produces is what editors
  mutate, what exports write, and what the runtime consumes — no sync steps,
  no conversion allocations, no dual struct vocabulary.
- Fidelity improvements fall out: the full 24-slot MTRL texture table reaches
  editors (animated frame sets past 8 entries survive), authored fixed-point
  collision values reach the runtime without float re-quantization detours,
  and light-type edits persist.
- A future pre-JO title bring-up would add its format as its own lib and its
  own consumers per the one-lib-per-format rule (ADR 0024) — not by
  resurrecting a shared IR.
- Land Warrior `.3di` v10 exploration (origin/lw-3di-import) predates this
  ADR; if revived it rebases onto the 3DI3-native surface.
