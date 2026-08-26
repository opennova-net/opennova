# .adm / .bad animation formats — reverse-engineering record

The on-disk animation pair: `.adm` (the text animation-definition map binding
anim slots to `.bad` clip names) and `.bad` (the binary skeletal clip
container). Implementing code: `engine/formats/adm` (parser + writer, flat C
ABI), `engine/formats/bad` (parser + writer, flat C ABI); runtime consumers
`engine/runtime/anim` (clip sampling), `engine/runtime/simassets` (clip
index / root motion / skeletal clip resolution), and
`godot/src/object/nova_skeletal_anim.cpp`.
Binary: retail Jointops.exe; all addresses are that binary's.

This is a CONSOLIDATION record (2026-08-08): the findings below were witnessed
across earlier grills (the 2026-07-08/09 FP-rig passes recorded in
[net/novaworld-net-re.md](../net/novaworld-net-re.md) §5.40 and its
corrections, ADR 0007, and the correspondence matrix) and are gathered here
into the formats' missing home. No new IDA session; witness sources are cited
in place.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| `.adm` grammar (rows, comments, variant rings) | MATCHING | ctests `adm_parse`, `adm_comment`, `adm_trim_value`, `adm_variants`; 3 `[orig]` cites in `adm/adm.h` |
| `.adm` writer | MATCHING (canonical form, parse-equality) | ctests `adm_write`, `adm_variants` |
| `.bad` container read/write | MATCHING (byte-exact roundtrip) | ctests `bad_parse`, `bad_roundtrip` (parse→write→parse equality + self byte-stability); `tests/test_bad_write_ffi.py` |
| `.bad` runtime consumption — FP viewmodel rig | MATCHING (model-table rig; rest-carrying composition) | ctest `anim_sample` (`sample_clip(model_bind)` is the reference form; production loaders run the equivalent rest-carrying factorization); ledger D-INF-14 (mechanism witnessed + ported) |
| `.bad` runtime consumption — world/body rigs | UNGRILLED, OPEN | ledger D-INF-13 — CORRECTED 2026-08-17: bodies and FP rigs run the SAME loader path (`model_bind=true` has no production caller); what is open is the equivalence proof against `build_world_bone_matrices @0x40c770` (its table source, padding loop, frame), not an FP-only path to extend |
| `BadBone.position` | dead at runtime (original never reads it) | correspondence `BoneAnim_BuildWorldMatrices @ 0x40c400` row; ctest `anim_sample` (synthetic) + the asset-gated ctest `anim_positions_from_model_corpus` (retail rigs) |

## The `.adm` format

Line-oriented text; the parser normalizes NUL bytes to newlines, skips `//`
comments, and considers only rows containing `anim_`. A row's key is the text
before the first `"`; then EVERY quoted token on the row is a clip variant
registered on that one anim slot [orig: `AnimMap_ParseConfigLine @ 0x40cb60`
registers every token]. The engine serves the variants as a circular ring —
`AnimMap_PlayAnimBySlot @ 0x40bda0` and `Anim_GetDurationTicks @ 0x53ee10`
both read the head and advance it — so repeated plays of one slot rotate
through its clips. The widest shipped row is 6 variants (`anim_cover_idle`
across the JOX/REVX corpora); the parsed model caps at 8
(`ADM_MAX_VARIANTS`).

The writer emits the canonical stock shape — `<key>\t\t\t\t"<clip>" "<clip2>"`
rows, CRLF line ends, one leading blank line, and a `CRLF×3 + NUL` trailer —
so `.adm` parity is parse-equality over the canonical form, not byte identity
with arbitrary hand-edited retail files (the same writer-policy shape as the
ADR 0021 Avatars writer). Callers order entries; `anim_reset` first by
convention (the reset row doubles as the rig's skeleton source — see the bind
rule below).

Surface: `adm_parse`, `adm_parse_buffer` (VFS byte path), `adm_free`,
`adm_write` — flat C ABI (`ADM_EXPORT`), FFI-safe PODs
(`AdmEntry{key[64], value[256], value_count, values[8][64]}`).

## The `.bad` format

Binary little-endian container (layout mirrored by the reader `bad.cpp` and
the writer `bad_write.cpp`, which reconstructs it from scratch — never
passthrough, ADR 0003):

| Offset | Block | Shape |
|---|---|---|
| 0 | header | 80 bytes (20 × u32): version, counts, offsets, flags |
| 80 | frame (channel) table | bone_count × 12: `num_frames u32, frame_lengths_off u32, rotations_off u32` |
| … | per-channel data | `u16 frame_lengths[]`, pad to 4, then `f32 x,y,z,w` quaternions (rot_stride each) |
| evt | events (optional) | per event `f32 vx,vy,vz,bottom,top`, plus `i32 trigger` when version == 1 |
| bone | bone table | bone_count × stride: `name[32]`, 3 pad + index byte, `num_children`, `child_addr`, `parent_addr`, `length`, `position[3]`, `rotation[9]` (row-major 3×3) |
| trn | translations | when `flags & 2`: `num_translations` × `f32 x,y,z`, frame-major |

Conventions preserved from stock assets and the historical exporter (recorded
at the writer head): channels and events carry `frame_count + 1` entries (the
terminal duplicate); child/parent are absolute byte addresses recomputed from
`parent_index`; root bones write `parent_offset 0` — a deliberate correction
over the historical exporter's `-1`, which only ever parsed correctly for
bone 0 because the parser force-overrides it.

The on-disk bone record (100 bytes, raw IEEE floats) and the runtime entity
skeleton bone (108 bytes, fp16.16 with `parentIndex @ +40`, pivots
`@ +56/60/64`) are distinct records — ADR 0007's boundary; the runtime record
is consumed by the world-entity builder (`build_world_bone_matrices
@ 0x40c770`), never parsed from disk by this lib.

## Runtime consumption semantics (witnessed elsewhere, summarized)

These rules live with their grills — the §5.40 correction chain in
[net/novaworld-net-re.md](../net/novaworld-net-re.md) and the correspondence
rows — and are summarized here because they define what the FORMAT's fields
actually mean:

- **The bind rule.** A rig's skeleton bind is the `.adm` slot-0
  (`anim_reset`) `.bad`'s records, pinned into `channel+44` at registration
  [orig: `AnimMap_RegisterEntity @ 0x40bb60`, the pin `@ 0x40bbe3`]; every
  clip composes `Transpose(bind 3×3) × channel`
  [orig: `AnimChannel_ComputeBoneMatrices @ 0x410da0`]. Per-clip self-bind
  was a 2026-07-08 misreading (it self-cancels at clip start — the T-pose
  freeze), corrected 2026-07-09 (D-INF-14).
- **The rig is the model table.** Bone count, hierarchy, and pivots come from
  the MODEL's bone table (`modelDef+52/+56`), never from the `.bad`'s bone
  table; `.bad` channel rows pair with model rows BY INDEX, and rows past the
  anim's bone count take bone 0's composed matrix (the padding loop
  `@ 0x40c5a1`) [orig: `BoneAnim_BuildWorldMatrices @ 0x40c400`].
- **`BadBone.position` is dead data.** The runtime never reads it; it is a
  lossy export artifact (257/477 retail `.bad`s triplicate X into all three
  components; 12/43 JO viewmodel rigs ship zeroed/stale positions and retail
  renders them all). It is reconstructible from bind + model
  (`positions_from_model`; synthetic pin in `tests/anim/anim_sample_test.cpp`, retail pin in the `OPENNOVA_JO_ASSETS`-gated `anim_positions_from_model_corpus` ctest).
- **Channel evaluation** slerps the quaternion keyframes per bone
  [orig: `BoneAnim_TransformBones @ 0x410360`]; translations apply only under
  `flags & 2`.

## Divergences

All existing ledger IDs — this record mints none:

| ID | Where tracked | One line |
|---|---|---|
| D-INF-13 | ledger (OPEN) | the world builder `@0x40c770` is ungrilled; the loaders' asserted equivalence with the composed builders (same path for FP and body rigs) awaits an IDA read of its table source, padding loop, and frame — see the ledger row for the four questions and the body-shaped oracle it needs |
| D-INF-14 | ledger (OPEN, mechanism ported; narrowed 2026-08-19) | FP `model_bind` composition corrected to the skeleton-`.bad` bind; def `rot` signs hip-idle-confirmed vs the registered retail pairs, velocity lead + the 4:3 framing drop ported (net-re §5.40 seventh pass); reload-direction + finger/left-hand footage remains |
| D-INF-15 | ledger (PERMANENT) | padded model-table rows: the original's flag-2 translation add reads uninitialized stack floats; ours zeroes them |

## Follow-ups

- The body-rig unification (D-INF-13) is the one open consumer-side port; the
  formats themselves are closed.
- `.adm` write parity is canonical-form by design; if a byte-exact need ever
  appears (none known — no tool round-trips hand-edited `.adm`s), it becomes
  a writer-policy ADR, not a parser change.
