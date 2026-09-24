# .adm / .bad animation formats — reverse-engineering record

The on-disk animation pair: `.adm` (the text animation-definition map binding
anim slots to `.bad` clip names) and `.bad` (the binary skeletal clip
container). Implementing code: `engine/formats/adm` (parser), `engine/formats/bad`
(parser); the `adm_write`/`bad_write.cpp` writers and their flat C ABI were
retired 2026-08-26 (ADR 0038); runtime consumers
`engine/runtime/anim` (clip sampling, the clip index, root motion and the
shared skeletal rig), `engine/runtime/assets` (the shared asset store), and
`godot/src/object/skeletal_anim.cpp`.
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
| `.adm` writer | RETIRED (ADR 0038, 2026-08-26): the canonical-form writer and ctest `adm_write` are gone; grammar parity is read-side | ctests `adm_parse`, `adm_variants` |
| `.bad` container read | MATCHING (retail-corpus parse; layout pinned by the reader) | ctests `bad_parse`, `anim_skeletal_clips_weapon_channel` (weapon-channel resolution over real clips), the asset-gated `anim_positions_from_model_corpus` (every viewmodel `.bad` under `OPENNOVA_JO_ASSETS`); the byte-exact write round-trip (`bad_roundtrip`; the FFI twin `test_bad_write_ffi.py` retired with ADR 0038) retired with the writer, ADR 0038 |
| `.bad` runtime consumption — FP viewmodel rig | MATCHING (model-table rig; rest-carrying composition) | ctest `anim_sample` (`sample_clip(model_bind)` is the reference form; production loaders run the equivalent rest-carrying factorization); ledger D-INF-14 (mechanism witnessed + ported) |
| `.bad` runtime consumption — world/body rigs | UNGRILLED, OPEN | ledger D-INF-13 — CORRECTED 2026-08-17: bodies and FP rigs run the SAME loader path (`model_bind=true` has no production caller); what is open is the equivalence proof against `build_world_bone_matrices @0x40c770` (its table source, padding loop, frame), not an FP-only path to extend |
| `BadBone.position` | dead at runtime (original never reads it) | correspondence `BoneAnim_BuildWorldMatrices @ 0x40c400` row; ctest `anim_sample` (synthetic) + the asset-gated ctest `anim_positions_from_model_corpus` (retail rigs) |

## The `.adm` format

Line-oriented text read through the engine's shared ASCII config reader
(`io/ascii_config.h`, also the `.tsd` reader). Lines split only on a CR LF
pair, and a tail line without one loses its final byte to the in-place
terminator [orig: `File_ParseASCIIFile @ 0x53D8C7..0x53D8F5`]. Each line is
tokenized on space, comma and tab; `"` toggles quoting and ends a token, so a
token is a quoted run's contents; an unquoted `//` or `;` ends the line; at
most 30 tokens [orig: `Terrain_TokenizeConfigLine @ 0x53CB60`, the comment
cuts `@0x53CC16..0x53CC31`]. A line with no token or whose first token starts
with `/` is skipped [orig: `@0x53D90D..0x53D91E`]. Token 0 names the anim
slot (`AnimMap_FindSlotByName @ 0x40CFA0` compares from its sixth character;
the parser keeps `anim_` keys and leaves the lookup to the runtime), and every
later token is a clip variant registered on that one slot until a token that
starts with `/` ends the row [orig: `AnimMap_ParseConfigLine @ 0x40cb60`, the
break `@0x40CBD0..0x40CBD2`]. A row with no clip registers nothing; it never
fails the file. The engine serves the variants as a circular ring —
`AnimMap_PlayAnimBySlot @ 0x40bda0` and `Anim_GetDurationTicks @ 0x53ee10`
both read the head and advance it — so repeated plays of one slot rotate
through its clips. The widest shipped row is 6 variants (`anim_cover_idle`
across the JOX/REVX corpora); the parsed model caps at 8
(`ADM_MAX_VARIANTS`).

**Inline comments (corrected 2026-09-23).** JOTAC's US01 walk rows include
`"Dt1RunF.bad"\t//\t"D4WLK_F.bad"`: the tokenizer's unquoted `//` cut ends
that line, so the latter is not a variant. Loading it selected a slower clip
with no footstep bits. The `/`-led token break is a separate rule: it ends
BIRD1.ADM's `"B_FORWA1.bad"/no target or near goal` row at the unquoted
`/no` token, and it would end a row at a quoted `"/..."` token too. JOTAC
DELTA01.ADM's bare `anim_emote_10` row registers nothing; the port once failed
the whole file on it. `adm_variants` pins the grammar, and `training_gameplay`
checks actual movement and emitted footsteps.

**Ring order and ownership (witnessed 2026-09-23).** A row serves from its
LAST token back: `AnimMap_RegisterBoneNode @0x40C2D0` inserts each token ahead
of the head and points the table at the new node (node->next = head
`@0x40C37F`, tail->next = node `@0x40C382`, table = node `@0x40C385`; the first
token and every `anim_reset` token self-ring, `@0x40C38B..0x40C38F`; slot 0's
backfill of the empty entries `@0x40C39A..0x40C3E2`), so a row `"A" "B" "C"`
serves C, B, A, C, and so on. The ring heads are ONE table per loaded `.adm`,
not per entity: `AnimMap_LoadAdmFile @0x40CC40` reuses an already loaded entry
by name (the `AnimMap_FindByName` call `@0x40CD2F`), and
`AnimMap_RegisterEntity @0x40BB60` allocates the primary (+0x188) and
secondary (+0x18C) slots and links both through `AnimMap_LinkEntity @0x40BA10`
(slot+0x48 = &entry+0x44 `@0x40BA77`), so every channel of every body on that
`.adm` advances the same heads. Registration starts both channels on table[0]'s
node without advancing it (`@0x40BC16`, `@0x40BD20`) and fills every unauthored
entry with table[0]'s node, so an unauthored state serves the reset clip. The
re-init (`AnimMap_UpdateEntity @0x40B5F0`, `@0x40B737..0x40B778`) takes node =
table[S], advances table[S] = node->next and inits the channel from the node;
there is no re-init when the request equals the playing id (`@0x40B645`).
`AnimMap_UpdateDualChannels @0x40B8C0` runs the secondary (`@0x40B908`) before
the primary (`@0x40B94E`), so a body whose channels re-init onto one state in
the same tick gives the secondary the head and the primary the entry after it
in ring order. Shipped JO body `.adm` files author multi-clip attack and death
rows but never a multi-clip `anim_reset`. The port's `AiSystem::anim_rings`
(`AnimVariantRings`, restored with the spawn baseline) serves both channels
that way (2026-09-23; it had kept per-entity heads served in file order, which
the CP01 `ai_threat` and `ai_corpse` regressions caught: an org1 body fires on
its primary channel's clip events). Residuals: the joiner's replica person rows
keep entry 0 because the wire carries no variant (D-NET-196), and registration
starts both channels on entry 0 rather than the reset row's current head
(identical for every retail `.adm`). Follow-up, not yet witnessed on its own
path: the first-person weapon clip ring (`player_weapon.cpp`
`weapon_ring_take_length` / `weapon_ring_take_variant`) and the weapon table's
`auto` duration ring (`weapon_table_build.cpp` `table_clip_seconds`) still serve
first to last; the viewmodel `.adm` registers through the same
`AnimMap_RegisterBoneNode`, so retail most likely serves them last to first
too.

The retired writer emitted the canonical stock shape — `<key>\t\t\t\t"<clip>" "<clip2>"`
rows, CRLF line ends, one leading blank line, and a `CRLF×3 + NUL` trailer —
so `.adm` parity is parse-equality over the canonical form, not byte identity
with arbitrary hand-edited retail files (the same writer-policy shape as the
ADR 0021 Avatars writer). Callers order entries; `anim_reset` first by
convention (the reset row doubles as the rig's skeleton source — see the bind
rule below).

Surface: `adm_parse`, `adm_parse_buffer` (VFS byte path), `adm_free`; C-linked
POD records (`AdmEntry{key[64], variant_count, variants[8][64]}`). `adm_write`
and the `ADM_EXPORT` flat C ABI retired with the FFI (ADR 0038).

## The `.bad` format

Binary little-endian container (layout mirrored by the reader `bad.cpp`; the
from-scratch writer `bad_write.cpp`, never passthrough per ADR 0003, retired
2026-08-26 with the FFI, ADR 0038):

| Offset | Block | Shape |
|---|---|---|
| 0 | header | 80 bytes (20 × u32): version, counts, offsets, flags |
| 80 | frame (channel) table | bone_count × 12: `num_frames u32, frame_lengths_off u32, rotations_off u32` |
| … | per-channel data | `u16 frame_lengths[]`, pad to 4, then `f32 x,y,z,w` quaternions (rot_stride each) |
| evt | events (optional) | per event `f32 vx,vy,vz,bottom,top`, plus `i32 trigger` when version == 1 |
| bone | bone table | bone_count × stride: `name[32]`, 3 pad + index byte, `num_children`, `child_addr`, `parent_addr`, `length`, `position[3]`, `rotation[9]` (row-major 3×3) |
| trn | translations | when `flags & 2`: `num_translations` × `f32 x,y,z`, frame-major |

Conventions preserved from stock assets and the historical exporter (recorded
at the retired writer's head, 5820432c1): channels and events carry `frame_count + 1` entries (the
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
  `flags & 2`. The blend itself is `Math_QuaternionSlerp @ 0x615e20`
  (witnessed 2026-09-10): no input or output normalization; a negative dot flips
  B onto the short arc `@0x615e51`; `1 - dot <= 0.01` (float `0x3C23D70A
  @0x7c56a8`, tested `@0x615ea6`) takes the LINEAR path with plain weights
  `(1-t, t)`, everything else the acos/sin weights `@0x615eaa..0x615ecd`; the
  linear result goes to `Math_QuaternionToMatrix3x3 @ 0x615a70` unnormalized.
  Ported in `engine/runtime/anim/anim_sample.cpp quat_slerp` (the earlier 0.9995
  nlerp threshold with renormalization was a stand-in); the reimpl's quaternion
  pose chain still normalizes at the bind compose / parent-local extraction, so
  the sub-unit linear-path length (|q|² ≥ 0.995) never becomes a bone scale —
  a presentation residual below visibility, not a motion-timing one.

## Divergences

All existing ledger IDs — this record mints none:

| ID | Where tracked | One line |
|---|---|---|
| D-INF-13 | ledger (OPEN) | the world builder `@0x40c770` is ungrilled; the loaders' asserted equivalence with the composed builders (same path for FP and body rigs) awaits an IDA read of its table source, padding loop, and frame — see the ledger row for the four questions and the body-shaped oracle it needs |
| D-INF-14 | ledger (FIXED 2026-08-22; condensed to a closure line) | FP `model_bind` composition corrected to the skeleton-`.bad` bind with the model-table rig source; def `rot` signs hip-idle-confirmed vs the registered retail pairs, velocity lead + the 4:3 framing drop ported (net-re §5.40 seventh pass); closed 2026-08-22 with the placement bone-exact and rendered-identity and the reload/left-hand full-clip sweep exact (the live proof is `godot/probes/runtime/vm_bone_dump_probe.gd`; net-re §5.40 eighth/ninth pass); the per-weapon `gfx1`-table-vs-`.adm`-bind equivalence rides D-INF-13 |
| D-INF-15 | ledger (PERMANENT) | padded model-table rows: the original's flag-2 translation add reads uninitialized stack floats; ours zeroes them |

## Follow-ups

- The body-rig unification (D-INF-13) is the one open consumer-side port; the
  formats themselves are closed.
- `.adm` write parity is canonical-form by design; if a byte-exact need ever
  appears (none known — no tool round-trips hand-edited `.adm`s), it becomes
  a writer-policy ADR, not a parser change.

## Playback clock follow-up (2026-09-11)

| Component | Verdict | Evidence |
| --- | --- | --- |
| Normalized channel timeline and terminal sample | MATCHING | anim_sample, anim_adm_playback, anim_adm_root_motion, mission_infantry_anim; skeletal_anim_test GUT |
| FP viewmodel timeline divisor | MATCHING | player_viewmodel_rig.cpp and shared native timeline |
| Automatic action delay conversion | MATCHING, separate clock | npruntime_weapon_table and weapon_fsm fixtures retain 62.5 plus one |

ClipTimeline advances a float32 channel by the stored float32
fps/(62*frame_count) delta. It compares the extended-precision sum before
storing, subtracts one once at a loop seam, and parks one-shots at 0.99999.
Sparse checkpoints preserve repeated-addition rounding during independent
playheads and seeks. The root-motion terminal sample clears XYZ and trigger
while retaining the capsule. The FP viewmodel uses divisor 62 for this channel;
automatic ACTION milliseconds retain their separate 62.5-tick conversion.
No fps test guards the delta: an fps of 0 is a channel frozen at frame 0 with
live capsule extents, and the port keeps such a clip rather than dropping it.
[orig: AnimChannel_InitFromData @ 0x410560, delta store @ 0x4105BA (fps load
@ 0x41058E, no test); AnimChannel_AdvancePlayback @ 0x40B140, add @ 0x40B156,
compare @ 0x40B15E]

The loop seam differs once an end-notify is armed. AnimMap arms flag 0x40000
on the channel every tick a deferred state waits and promotes on the latched
0x20000 BEFORE the advance and the keyframe sample. AnimChannel_AdvancePlayback
then wraps the time and, with 0x40000 set, overwrites it with the 0.99999 park
and latches 0x20000 without the 0x10000 stop, so the wrap tick samples the clip
end (rec[frames-1]..rec[frames], trigger[frames-1]) and the promoted clip's
frame 0 lands on the next tick. `ClipTimeline::normalized_at(ticks,
armed_boundary)` and `AdmRootMotion::advance_armed` carry that park (ctest
`anim_adm_playback`), and both consumers take it through the
`IRootMotionSource::advance_armed` seam: the local primary channel arms its
step-3b promotion clock (`clip_length_ticks`, the clip's first end) and the
replica channel arms its lazily computed `net_anim_pending_boundary`; each
promotes on the next tick.
[orig: AnimMap_UpdateEntity pending check @ 0x40B77B..0x40B7E1 (promote
@ 0x40B795 / @ 0x40B7C3, arm @ 0x40B7AD / @ 0x40B7DB), advance @ 0x40B7FE,
sample @ 0x40B82A; AnimChannel_AdvancePlayback @ 0x40B193 (0x40000 test), wrap
@ 0x40B199, park @ 0x40B1A2..0x40B1B1]
