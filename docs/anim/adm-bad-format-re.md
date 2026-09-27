# .adm / .bad animation formats — reverse-engineering record

The on-disk animation pair: `.adm` (the text animation-definition map binding
anim slots to `.bad` clip names) and `.bad` (the binary skeletal clip
container). Implementing code: `engine/formats/adm` (parser and the
canonical-form writer `adm_write.cpp`), `engine/formats/bad` (parser, the
writer `bad_write.cpp`, and the clip construction seam `bad_build.{h,cpp}` an
authoring front end builds through — ADR 0047, which returns the writers ADR
0038 retired); runtime consumers
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
| `.adm` variant rings at runtime (last token first, one head table per loaded file, the first-person load sequence, the loop-wrap serve and fade) | MATCHING (witnessed 2026-09-27, below); the joiner's replica rows keep entry 0 (D-NET-196) | ctests `anim_adm_ring_table` (the first-person table, load sequence and channel), `infantry` (both body channels' re-init and wrap serves) |
| `.adm` writer | MATCHING (canonical form, not byte identity with hand-edited files) | ctests `adm_write`, `adm_parse`, `adm_variants`; the 82-table corpus sweep below |
| `.bad` container read | MATCHING (retail-corpus parse; layout pinned by the reader) | ctests `bad_parse`, `anim_skeletal_clips_weapon_channel` (weapon-channel resolution over real clips), the asset-gated `anim_positions_from_model_corpus` (every viewmodel `.bad` under `OPENNOVA_JO_ASSETS`) |
| `.bad` writer and the construction seam | MATCHING (field-equal over the 477-clip corpus; byte-exact for our own files; the runtime poses a rebuilt set identically); every event states its `bottom` and `top`, which the seam carries and never derives (the event record section below) | ctests `bad_parse` (357_RST.bad's rows 0..frame_count), `bad_roundtrip` (the fixtures byte-exact, BINOC.bad field-equal, the pad row), `bad_build` (the derivations: BINOC.bad's bind, DT1PRONE's positions through DT1RST's bind), `anim_o3a_commands`, the gated `anim_o3a_retail_roundtrip` and `anim_o3a_runtime_playback` (US01.ADM and both first-person sets through `runtime/anim/skeletal_clips` over US01.3di, Mp5b_1st.3di and 357_1st.3di's bone tables, 1,827 poses, worst 2.4e-6 degrees); the corpus sweep below |
| `.bad` runtime consumption — FP viewmodel rig | MATCHING (model-table rig; rest-carrying composition) | ctest `anim_sample` (`sample_clip(model_bind)` is the reference form; production loaders run the equivalent rest-carrying factorization); ledger D-INF-14 (mechanism witnessed + ported) |
| `.bad` runtime consumption — which clip binds the rig, and the translation gate | MATCHING (witnessed 2026-09-25, below); translation under a cross-fade is not ported | ctest `anim_sample` (the gate in the sampler, and the rig loader over synthetic tables); the 82-table scan below; the gated `anim_o3a_runtime_playback` |
| `.bad` event record — the runtime's read, and what `bottom`, `top` and `velocity` measure | MATCHING (read-only grill of the read, 2026-09-25); the measure WITNESSED on the corpus (below) | ctests `anim_adm_root_motion`, `root_motion`; the 30 person tables and 477 clips below |
| `.bad` runtime consumption — world/body rigs | UNGRILLED, OPEN | ledger D-INF-13 — CORRECTED 2026-08-17: bodies and FP rigs run the SAME loader path (`model_bind=true` has no production caller); what is open is the equivalence proof against `Bone_BuildWorldMatrices @0x40c770` (its table source, padding loop, frame), not an FP-only path to extend |
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
slot past its first five characters, whatever they are
(`AnimMap_FindSlotByName @ 0x40CFA0` compares from the sixth character without
case, so `ANIM_RESET` and `xxxx_reset` name slot 0 as `anim_reset` does; every
retail key is `anim_`). The parser keeps every key longer than five characters
and leaves the lookup to the runtime, whose slot names live in `runtime/world`
and whose every consumer registers a row under the slot its key names
(`adm::adm_slot_key`); a key of five or fewer names no slot, our rule, since
retail's lookup would read on past its end into the rest of the tokenized
line. Every later token is a clip variant registered on that one slot until a
token that starts with `/` ends the row [orig: `AnimMap_ParseConfigLine @ 0x40cb60`, the
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
serves C, B, A, C, and so on. A later row naming the same slot registers onto the same
ring, since each token goes through the slot lookup on its own row (FSldr02
repeats its five `anim_emplaced*` rows, FSldr05 its `anim_burn_2` row); the
root-motion source (`AdmRootMotion`) kept only the first such row until
2026-09-25. The ring heads are ONE table per loaded `.adm`,
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
its primary channel's clip events). Slot 0 is no ring: each reset token
replaces its head, so it serves the last reset token that loaded, and the
first one fills every slot the table does not author; neither moves.
Residuals: the joiner's replica person rows keep entry 0 because the wire
carries no variant (D-NET-196), and registration starts both channels on
entry 0 rather than the reset row's current head (identical for every retail
`.adm`).

**The first-person rings (witnessed 2026-09-27).** The viewmodel `.adm`
registers through the same code, so its rows serve last to first too, and its
heads are one table per loaded file: the first weapon (weapon.def order)
naming an ANIMADM loads it and every later weapon naming the same file gets
the same table back (`AnimMap_LoadAdmFile @0x40CC40`, the cached entry
`@0x40CD45..0x40CD5C`). Three reads serve a slot's head and advance it: an
`auto` action delay (`Anim_GetDurationTicks @0x53EE10`, `@0x53EE20..0x53EE26`;
slot 0 reads 0 ticks), a play (`AnimMap_PlayAnimBySlot @0x40BDA0`,
`@0x40BDB4..0x40BDC1`; a play of slot 0 does nothing, `@0x40BDA7`), and a loop
wrap (below). The load serves before the match does: each weapon's
`Anim_InitActions @0x541FA0` reads its `auto` fields in action order,
DELAYSTART (`@0x5421C5`) before DELAYEND (`@0x5421D8`), and plays slot 241 at
its END (`@0x54225A`); after the whole file `WeaponDefs_PlayIdleAnimAll
@0x53FC10` plays 241 once more for every weapon with a table (`@0x53FC2C`, from
`Game_StartMission @0x5254EB`). So for `anim_wpn_idle "a" "b" "c"` on a table
one weapon names, whose IDLE row alone reads it (`DELAYEND auto`), the bake
reads c, the END play serves b, the mission-start play serves a, and the first
idle in the match serves c; every other read of slot 241 (a second `auto`
field, another weapon naming the table) shifts each later serve by one. 29 of
JOTAC's 49 ANIMADM tables are named by more than one weapon. The port keeps
these heads in the weapon table (`anim::AdmRingTable`, one per ANIMADM;
`build_weapon_table` runs the load sequence and a mount runs the descriptors
it baked), and the first-person channel plays from them (`fp_channel_play`,
`fp_channel_advance`; ctest `anim_adm_ring_table`).

**The loop wrap (witnessed 2026-09-27).** A looping channel that wraps with no
fade running and no end-notify armed serves its latched slot's ring again
(`AnimChannel_AdvancePlayback @0x40B140`: t >= 1 `@0x40B165`, the loop bit
`@0x40B167`, t -= 1 `@0x40B199`, the armed park that skips the rest
`@0x40B19E`, the gate `@0x40B1B5..0x40B1C1`, the one global wrap callback
`@0x40B1C8` -> `AnimMap_AdvanceToNextAnim @0x40BDF0`, the serve
`@0x40BE02..0x40BE07`). The latched node served again (a one-token row, a
reset-filled slot) plays on; any other node, even a repeated token naming the
same `.bad`, fades in from its first frame over eight advances
(`AnimChannel_InitFromParams(ch, clip, 8, 0, 0x1000)` `@0x40BE24`: the blend
half at t = 0, weight 0, step 1/8) and takes the latch (`@0x40BE29..0x40BE33`).
`AnimChannel_AdvanceBlendedPlayback @0x40B1E0` steps the incoming half, then
the outgoing one, which wraps on without a callback, adds 1/8 and promotes the
incoming half when its countdown ends (`@0x40B20C..0x40B224`). The
first-person channel reaches the wrap from its action shims
(`AnimChannel_AdvanceDispatch @0x40B960`), and a play re-inits only its
primary half (`@0x40BDD1`), so a play inside a wrap fade does not cancel it:
the pose blends the played clip toward the incoming variant until the
countdown promotes that variant over it. The body channels reach it from
`AnimMap_UpdateEntity` (`@0x40B7FE`), the secondary first; their wrap tick
already reads through `AnimChannel_BlendKeyframes @0x40B340` at weight 0 (the
wrapped variant's motion, bottom and top, the incoming variant's trigger at
t = 0, `@0x40B38D`), a state change during the fade replaces the incoming half
(`@0x40B761`) while the ring read stays consumed, and the secondary's fade
changes only its pose. The port serves the wrap in `fp_channel_advance` and,
for both body channels, in `advance_primary_channel` and the secondary's
advance (ctests `anim_adm_ring_table`, `infantry`).

The writer emits the canonical stock shape — `<key>\t\t\t\t"<clip>" "<clip2>"`
rows, CRLF line ends, one leading blank line, and a `CRLF×3 + NUL` trailer —
so `.adm` parity is parse-equality over the canonical form, not byte identity
with arbitrary hand-edited retail files (the same writer-policy shape as the
ADR 0021 Avatars writer). Callers order entries; `anim_reset` first by
convention (the reset row doubles as the rig's skeleton source — see the bind
rule below).

Surface: `adm_parse`, `adm_parse_buffer` (VFS byte path), `adm_free`,
`adm_write_buffer`, `adm_write`; POD records
(`AdmEntry{key[64], variant_count, variants[8][64]}`). The `ADM_EXPORT` flat C
ABI is gone for good with the FFI (ADR 0038); the writer returned with
ADR 0047.

## The `.bad` format

Binary little-endian container (layout mirrored by the reader `bad.cpp` and
the from-scratch writer `bad_write.cpp`, never passthrough per ADR 0003):

| Offset | Block | Shape |
|---|---|---|
| 0 | header | 80 bytes (20 × u32): version, counts, offsets, flags |
| 80 | frame (channel) table | bone_count × 12: `num_frames u32, frame_lengths_off u32, rotations_off u32` |
| … | per-channel data | `u16 frame_lengths[]`, pad to 4, then `f32 x,y,z,w` quaternions (rot_stride each) |
| evt | events (optional) | per event `f32 vx,vy,vz,bottom,top`, plus `i32 trigger` when version == 1 |
| bone | bone table | bone_count × stride: `name[32]`, 3 pad + index byte, `num_children`, `child_addr`, `parent_addr`, `length`, `position[3]`, `rotation[9]` (row-major 3×3) |
| trn | translations | when `flags & 2`: `(frame_count + 1) × bone_count` × `f32 x,y,z`, frame-major, then retail's pad row |

Conventions preserved from stock assets and the historical exporter (the
writer's head carries them, with the corpus counts in the section below): channels, events and translation rows carry `frame_count + 1`
entries (the header counts intervals); child/parent are absolute byte addresses recomputed from
`parent_index`; root bones write `parent_offset 0` — a deliberate correction
over the historical exporter's `-1`, which only ever parsed correctly for
bone 0 because the parser force-overrides it.

The on-disk bone record (100 bytes, raw IEEE floats) and the runtime entity
skeleton bone (108 bytes, fp16.16 with `parentIndex @ +40`, pivots
`@ +56/60/64`) are distinct records — ADR 0007's boundary; the runtime record
is consumed by the world-entity builder (`Bone_BuildWorldMatrices
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
  freeze), corrected 2026-07-09 (D-INF-14). Which clip of the table that is,
  and the translation gate the bind also decides, are in the 2026-09-25
  section below.
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
  [orig: `BoneAnim_TransformBones @ 0x410360`]; translations apply only when
  the playing clip AND the bind carry `flags & 2` (below). The blend itself
  is `Math_QuaternionSlerp @ 0x615e20`
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

## The bind clip and the translation gate (witnessed 2026-09-25)

| Component | Verdict | Evidence |
|---|---|---|
| Which clip binds the rig | MATCHING | ctest `anim_sample` (a two-variant reset row, a second reset row, an `anim_resetx` row ahead of the reset row, a reset variant that does not load); the 82-table scan below |
| A table with no reset row | MATCHING (the rig does not load) | ctest `anim_sample` |
| The translation gate | MATCHING | ctest `anim_sample` (the sampler over an untranslated bind (both modes) and a translated one, and the rig loader over an untranslated and a translated reset) |
| A part whose parent is numbered after it | MATCHING (witnessed 2026-09-27, below) | ctest `anim_sample` (a model-table row hung off a later, padded row) |
| Translation under a cross-fade | NOT PORTED (follow-up below) | |

**Which clip binds.** A row names its slot by its key past the first five
characters, compared without case against the 252 slot names, and slot 0's
name is `reset` [orig: `AnimMap_FindSlotByName @ 0x40CFA0`, the table
`g_AnimStateNameTable @ 0x8135F0`, entry 0 `"reset" @ 0x7C3264`]: `anim_reset`
and `ANIM_RESET` name slot 0, `anim_resetx` and `anim_idle_reset` do not. Every
clip registered on slot 0 replaces the slot's head instead of joining a ring
(`AnimMap_RegisterBoneNode @ 0x40C2D0`: a zero slot index takes the jump
`@0x40C365..0x40C367` to the head store `@0x40C38B` and the self-ring
`@0x40C38F`), and a variant whose `.bad` does not load registers nothing
(`AnimMap_ParseConfigLine @ 0x40CB60`, the null test `@0x40CBE7`). The rig's
bind is therefore the last variant that loads, of the last row naming slot 0.
That head is what `AnimMap_LoadAdmFile @ 0x40CC40` pins into the table's own
channel (`@0x40CE19`) and `AnimMap_RegisterEntity @ 0x40BB60` into both
channels of every body (`@0x40BBE3`, `@0x40BCF7`). An unauthored slot still
serves the FIRST reset variant: slot 0's first registration backfills every
empty slot with the head of that moment (`@0x40C39A..0x40C3E2`), and a later
reset variant replaces only the head. `SkeletalClips::load_from_adm` used to
take the first variant of the first row whose key merely contained `reset`,
and the first row's first variant when no row did; it now takes the rule
above (`[orig]`-cited in `engine/runtime/anim/skeletal_clips.cpp`).

**A table with no reset row does not bind.** When clips registered but slot 0
did not, `AnimMap_LoadAdmFile` reads slot 0's head without a test and faults on
the null head (`@0x40CE11..0x40CE16`, the read of `[head + 0x20]`); when nothing
registered, the load fails (`@0x40CE03..0x40CE07`); and
`AnimMap_RegisterEntity` frees the channel it allocated when slot 0 is empty
(`@0x40BBC4`, `@0x40BD8B`). No retail entity animates through such a table, so
`load_from_adm` declines it (the rig does not load) instead of borrowing the
first row's clip, and the authoring seam refuses to write one
(`bad_build_mint_table`, so `opennova-3di anim build`).
`AnimChannel_ComputeBoneMatrices` does fall back to the
playing clip's own bone table when `channel+44` is null (`@0x410DE5`), but no
table reaches that path: every initializer witnessed pins `channel+44` (the
three above, and the menu preview's `Dt1rst.bad`,
`PlayerInfo_InitPreviewModel @ 0x5600D0` `@0x560183`).

**The translation gate.** `BoneAnim_TransformBones @ 0x410360` writes each
bone's translation into a shared scratch (`dword_A78350`) from the PLAYING
clip: its lerped rows when that clip carries `flags & 2` (the test
`@0x41038D`, the rows through `sub_4102D0 @ 0x4102D0`), zeros when it does not
(`@0x4103F6..0x4103FE`). `AnimChannel_ComputeBoneMatrices @ 0x410DA0` then
copies the scratch into each bone matrix only when the BIND carries
`flags & 2`: the test reads header word 4 of `channel+44`'s `.bad`, or of the
playing clip's when that is null (`test byte [ebp+0x10], 2 @0x410DE7`,
`ebp` = `channel+44` `@0x410DD8`, else the playing clip `@0x410DE5`; the copy
`@0x410EA0..0x410EB7`). Its other branch builds every matrix through
`Math_TransposeMatrix3x3ToMatrix4x4 @ 0x616740`, which zeroes the translation
row (`@0x61675A..0x616784`), and `BoneAnim_BuildWorldMatrices @ 0x40C400` adds
that row to the bone's position (`@0x40C6E9..0x40C71D`). A bone therefore
moves only when the clip AND the bind are translated: a translated clip over
an untranslated reset moves nothing, an untranslated clip over a translated
reset moves nothing (the scratch holds its zeros), and with no bind pinned the
clip's own flag decides. `sample_clip` reads the gate from its `bind_source`
in both modes and the rig loader passes the reset `.bad`; it used to gate on
the clip's own flag alone. Over the 5,145 clip registrations of the 82 retail
tables (2026-09-25 scan), 430 translated clips play over a translated reset, 3
untranslated clips over a translated reset, 4,712 neither, and no translated
clip over an untranslated reset, so no retail rig changes; an authored set can
hold that pairing. Every table has exactly one reset row with one variant,
which the old and the new rule both pick. Retail reads the scratch past what
the playing clip wrote in one more case: a bone of a translated bind past the
playing clip's own bone count keeps whatever the scratch last held. No retail
clip has fewer bones than its reset, and the port reads zero there.

**A parent numbered after its child (witnessed 2026-09-27).**
`BoneAnim_BuildWorldMatrices @ 0x40C400` fills one matrix per model part in a
first loop (the clip's rotation with the translation zeroed,
`@0x40C53F..0x40C553`, or matrix 0's copy for a part past the clip's bones,
`@0x40C5B6`) and then runs the FK over the same array in place, reading each
part's parent (the model row's +20, `@0x40C5F6`) as `outputMatrices[parent]`
(`@0x40C674`). A parent with a lower index is finished by then; a parent
numbered after its child is still that raw entry, so the child turns by the
parent's rotation as if the parent sat at the model origin, losing its pivot
carry and its ancestors' motion. Seventeen of the 2,409 models on the JOTAC
base mount ship such rows, first-person models among them (`mmp5_1st.3di`
hangs parts 10, 11, 13, 14, 16, 17, 20 and 23 off later-numbered parts;
`c4_1st`, `hend_1st`, `land_1st`, `mor_1st`, `mwr_1st`, `oicw_1st` and
`oicw_1stg` as well). `sample_clip` computes every part's
rotation before any position and places such a child by the parent's raw
rotation alone; a self-parent (the shipped root) stays the root. The PANM path
reads a later parent's slot from what the previous model left in the shared
array (`Model_TransformBoneMatrices @ 0x58E390`, `@0x58E46B`), which no port
can reproduce.

## The writers and the construction seam (2026-09-24)

ADR 0038 retired the `.bad` and `.adm` writers; ADR 0047 brings them back for
the authoring route, from scratch against the readers (ADR 0003). The pair is
`engine/formats/bad/bad_write.{h,cpp}` and `engine/formats/adm/adm_write.cpp`,
under one construction seam, `engine/formats/bad/bad_build.h`, which every
front end builds a clip through (`opennova-3di anim build` reads the `.o3a`
clip-set text into it; `docs/anim/o3a-scene-format.md`).

Retail ships no `.bad` writer, so the on-disk shape is the loader's
[orig: `BoneFile_Load @0x40fff0`] plus what the shipped corpus carries. A sweep
of all 477 `.bad` clips and 82 `.adm` tables under `OPENNOVA_JO_ASSETS`
(2026-09-24, refreshed 2026-09-25) pins these. The seam derives the bone
table, the header words and the translation pad row rather than asking an
author for them; what an author supplies (the keys, events and translation
rows, `frame_count + 1` of each) it counts and refuses when short:

- **The bone table's `rotation[9]` is the TRANSPOSE of the bone's first
  channel key as a matrix**, in 13,517 of 13,517 bones (worst deviation
  5.0e-7). This is the same relation the runtime reads from the other side —
  `mat3(stored) x channel-at-reset == identity` — but the runtime reads it
  from the rig's RESET clip: every clip of a rig composes against the bind of
  the `.adm`'s slot-0 clip (the bind rule above), and a clip's own bone table
  is read only when no reset is pinned [orig: `AnimChannel_ComputeBoneMatrices
  @ 0x410da0`, the bind from `channel+44` `@0x410dd8`, else the playing clip
  `@0x410de3`]. Nothing authors a bind. The production loaders carry the reset
  clip's bind as the SKELETON's rest and pose it with the channel, so what a
  bone deforms by is `key * bind_reset^-1`; an authoring front end that poses a
  rig with the key over a rest set to that bind shows exactly what the game
  draws. The seam picks the set's reset clip the way the table registers it:
  the row whose key names slot 0 (`reset`) past its first five characters, and
  of its variants the LAST, since each reset variant replaces the slot's head
  rather than joining a ring [orig: `AnimMap_FindSlotByName @ 0x40cfa0`;
  `AnimMap_RegisterBoneNode @ 0x40C2D0` `@0x40c38b`].
- **`position[3]` follows the paired model's pivots through the SET's reset
  bind**: `position[i] = rotation_reset[parent(i)] . clip(pivot[i] -
  pivot[parent(i)])`, the relation `positions_from_model` rebuilds a rig's
  table with. Over the retail tables' other clips it holds in 30,358 of 32,011
  non-junk bones, and through the clip's OWN rotation in only 365: every clip of
  a set stores the reset clip's positions. The seam derives it that way, and
  `anim scene` recovers the pivots through the same bind, so every clip of a
  table recovers the rig's one set of pivots. A bone past the reset clip's
  bones (G17_1st's `g17_1f` and `g17_1d` carry one bone more than their reset)
  turns its children through the clip's OWN first key, transposed: that is the
  seam's rule, not a witness, since what retail reads for such a bone is
  unwitnessed (`bad_derive_bind_rows`, `engine/formats/bad/bad_build.h`; a lone
  clip, which no table binds, turns through its own first key the same way).
- **`fps` is 30 in every clip.** `version` is 1 in 474 and 0 in 3 (a 20-byte
  event record with no trigger word), so the seam refuses any other version and
  a trigger on a version 0 event.
- **The loader refuses a file over 500,000 bytes** [orig: `BoneFile_Load
  @ 0x40fff0`, the `0x7A120` gate]; the largest shipped clip is 298,172 bytes
  (`M60_1i`), and the seam refuses to mint a larger one.
- **Every bone buffer the game samples a clip into holds 64 bones**, and
  nothing clamps a clip's own count to it [orig: `BoneSystem_Init @ 0x410170`,
  the 64-bone scratch, from `AnimMap_Init @ 0x40BE52`; `BoneAnim_TransformBones
  @ 0x410360`; `BoneAnim_BuildWorldMatrices @ 0x40C400`, its 64-matrix locals];
  the largest shipped clip carries 61 and the seam refuses more than 64
  (`kBadMaxBones`).
- **A loop steps `fps / (62 * frame_count)` of its cycle a tick** and takes one
  away once at the wrap [orig: `AnimChannel_InitFromData @ 0x410560`, the step
  `@0x4105BA`; `AnimChannel_AdvancePlayback @ 0x40B140`, `@0x40B199`], so a
  step of a whole cycle or more never plays: at exactly one it holds its first
  frame, past one its time runs on past the clip's rows. Every shipped clip is
  30 fps over one frame or more; the seam refuses a loop at 62 times its frame
  count or more.
- **Across a table** (`bad_build_check_set`): every clip's file name packs, at
  most 15 bytes with its `.bad` and ASCII, as every shipped name does (the
  longest is `avenger_025.bad`) [orig: `PFF_FindEntry @ 0x7685D0`, a strcmp
  over the entry's 16-byte name field]; and a clip carries translations only
  when the reset clip does, since a bone moves only when both do (the
  translation gate above; no shipped table pairs a translated clip with an
  untranslated reset).
- **The header words the reader never names are constant**: word 8 = 0,
  9 = 0, 10 = 8, 14 = 1, 17 = 1, 18 = 0, 19 = 0 across all 477.
- **`event_count == frame_count + 1`** wherever a clip carries events, as the
  channel key lists do: the header counts intervals.
- **The translation block holds rows 0 to `frame_count`**, frame-major, the
  fence-post count the keys and events carry. The runtime reads row
  `trunc(frame_count * t)` and lerps it with the NEXT row by the remainder
  [orig: `sub_4102D0 @ 0x4102d0`, the row base `[6] + [5] * (100 + 12 * row)`
  `@0x410327..0x41033b`, called from `BoneAnim_TransformBones @ 0x410360`
  `@0x41048b`, the next row at `+12 * bone_count` and the `(1 - w, w)` weights
  `@0x410497..0x4104cd`], so the last interval of every cycle reaches row
  `frame_count` (one-shots park at 0.99999, loops wrap, so `t` never reaches
  1.0 [orig: `AnimChannel_AdvancePlayback @ 0x40B140`]). A `frame_count * t` of
  exactly 0 is read as 1.0 (`@0x4102f7..0x410304`), so the sample at `t == 0`
  takes row 1 and weights row 2 by zero. Row `frame_count` is real data, the
  terminal key's translation: it equals row `frame_count - 1` (within 1e-6) in
  118 of the 202 translated clips and carries the clip's last step in 83
  (G17_1f's continues its 4.2 cm per frame; RPG7_1f's is 0.62 m). Retail's
  exporter wrote a row per KEY and one row more: 200 clips key every frame and
  hold rows 0 to `frame_count + 1`, M60_1i (181 keys over 150 frames) holds 182
  rows and stgr_RST (two keys over two frames) 3. That extra row holds exporter
  memory that matches no row of the clip (357_1f's puts bones whose
  translation is zero 1.7 m out; stgr_RST's, its row 2, reaches 6.8e22), and
  past row `frame_count` the only read that reaches it is a one-frame clip at
  `t == 0`, at weight zero. The writer repeats row `frame_count` there, so
  that read stays finite. The runtime's pose table bakes row `f` at frame `f`,
  which is the original's read everywhere but the `t == 0` instant: frame 0 is
  also the left end of the first window the pose evaluator interpolates, where
  the original reads the lerp of rows 0 and 1.
- **A bone may key fewer times than the frame count.** 476 of 477 clips key
  every bone once per frame, but the count is the duration table's business,
  not the header's [orig: `BoneAnim_FindKeyframeAtTime @ 0x410220`]: `DT1RST`
  and `stgr_RST` key twice over two frames, `M60_1i` 181 times over 150, and
  `DVFLEE1E.BAD` alone keys its bones sparsely and unevenly (36 to 54 keys over
  71 frames, the only file with a duration other than one).
- **Neighbouring keys may sit in opposite hemispheres** — 4,573 of 657,788 key
  pairs, over 218 files — so nothing re-signs a channel; the slerp short-arcs
  either way [orig: `Math_QuaternionSlerp @ 0x615e20`]. Keys are unit only to
  2.5e-7, which is a twentieth of a degree of spurious angle if a comparison
  forgets to normalize.
- **`flags` bit 3 (0x8) rides 73 clips** — the viewmodel `_1d`/`_1e` draw
  clips, the `CycDr*` bike set, `avenger_025` — and is unwitnessed. It is
  carried, never read.
- **The bone table's bookkeeping is dead data.** `num_children` agrees with the
  parent links in all 13,517 rows, but `child_offset` is inconsistent: 8,830
  rows point at the first child and 8,626 at the last (bone 0 in 209 files, the
  third bone in 268), and 1,047 leaves carry a stale address instead of zero.
  Bone 0's `parent_offset` is 0 in only 368 of 477 files; the reader forces it
  to -1 regardless. The writer emits the first child, zero for a leaf and zero
  for the root's parent, and `anim compare` reads none of these fields.

Round-trip results (`opennova-3di anim scene` -> `anim build` -> `anim
compare`, re-run 2026-09-25 with `compare` reading the bind and absent clips):
**477 of 477 clips and 81 of 82 tables are the same animation.** The one
table, `ESTAND02.ADM`, names `AttckAct1.bad`, which the corpus does not ship;
`scene` drops that variant with a note and `compare` reports it. Our own files write back byte for byte;
retail's differ only where nothing can reproduce them — the uninitialized bytes
its exporter left after each name's NUL, the stale child addresses, and the
bind's low mantissa bits (the stored 3x3 carries more than the float key it is
derived from).

## The event record: ground, hips and head (witnessed 2026-09-25)

| Component | Verdict | Evidence |
|---|---|---|
| The runtime's read of an event (root motion, the body's settle, the capsule top, the trigger) | MATCHING (read-only grill) | the cites below; ported in `engine/runtime/anim/adm_root_motion` (ctests `anim_adm_root_motion`, the gated `root_motion`) |
| What `bottom`, `top` and `velocity` measure | WITNESSED on the corpus (retail's exporter does not survive; the relation is measured, not read) | the 30 person tables below |
| The terminal events | WITNESSED on the corpus | 477 of 477 clips |
| The construction seam's events | MATCHING (every event states its `bottom` and `top`; the seam carries them and derives neither) | ctests `bad_build`, `anim_o3a_commands`; the gated `anim_o3a_retail_roundtrip` |

**The read.** `AnimChannel_InterpolateKeyframe @ 0x40B230` lerps event record
`trunc(frame_count * t)` with the next one (the record address `@0x40B2AA`,
the five floats `@0x40B2F0..0x40B329`) and takes the trigger word unlerped
from the lower record (`@0x40B32F`); a stopped channel (flag 0x10000) zeroes
the velocity and the trigger and still lerps `bottom` and `top`
(`@0x40B2B6..0x40B2DE`). `AnimMap_UpdateEntity @ 0x40B5F0` turns that sample
into the entity's root frame (the out-transform `@0x40B82F..0x40B8A3`, with
`flt_7C32B4` = 32768, `flt_7C32B8` = -65536 and `flt_7C32BC` = 65536):

- `out[3] = bottom * 65536` (`@0x40B836`, stored `@0x40B84D`): the body's
  settle, the entity origin's height above the ground it stands on;
- `out[4] = 0x2000 + top * 65536` (`@0x40B845`, `@0x40B85F..0x40B86A`): the
  capsule top;
- `out[0] = vel.z * 32768`, forward (`@0x40B855..0x40B878`), and
  `out[1] = vel.x * 32768`, lateral (`@0x40B872..0x40B883`): half a clip
  frame's step per 62 Hz tick, a 30 fps frame lasting about two ticks, so a
  velocity is metres per clip frame;
- `out[2] = vel.y * 32768` (`@0x40B87F..0x40B88B`), OVERWRITTEN with the
  change in `out[3]` since the channel's previous update whenever one is held
  (slot+0x4C, the test `@0x40B88E..0x40B899`, the new value kept `@0x40B8A0`;
  the pending climb and death states 32..35 and 176..179 clear it `@0x40B637`).
  The vertical root motion is the change in `bottom`; `vel.y` is read only on
  a channel's first update after that reset;
- the trigger word goes to `g_AnimEventTriggerBits @ 0xA2ED08` (`@0x40B8A3`).

**How the port consumes it.** `anim::AdmRootMotion`
(`engine/runtime/anim/adm_root_motion.{h,cpp}`) emits the out-transform as the
`RootMotionFrame` of `engine/runtime/world/infantry.h` (`capsule_bottom` =
`out[3]`, `capsule_top` = `out[4]`; the witness is
[world/world-wac-ai-re.md](../world/world-wac-ai-re.md) §3.4), and every
consumer reads the pair as heights over the ground under the model origin,
which is the hips:

- the ground settle puts the origin `capsule_bottom` above the ground, so the
  feet rest on it (D-INF-6 in the world record §5: the movement resolver
  `@0x4b2bd0` resettles `entity[3] = entityRadius + groundHeight` `@0x4b3da3`,
  fed `out[3]` by both on-foot callers; ported in
  `engine/runtime/world/collision_resolve.cpp`'s ground-settle tail, `feet_z =
  pos[2] - capsule_bottom`, and `infantry.cpp`'s foot clearance);
- the vertical root motion is `capsule_bottom` minus the previous tick's
  (`infantry.cpp`, `prev_capsule_bottom` = anim_slot[19], cleared by the same
  climb and death states);
- `capsule_top - capsule_bottom`, the head's height above the hips plus the
  0x2000, is the eye: the org1 CameraOffset `max(top - bottom, 0x9000)`, the
  player's `min(top - bottom, 0xD000)` (`infantry.cpp`, `[orig]`-cited there),
  and the resolver's capsule, `halfRadius = bottom >> 4`, `collisionRadius =
  halfRadius + |top - bottom| / 2`, `outerRadius = bottom >> 1` (world record
  §15.3, D-COL-4; `collision_resolve.cpp`);
- a footstep sounds at `pos.z - out[3]`, the ground under the body (world
  record §17.4b; `engine/runtime/world/infantry_sound.cpp`), and the float and
  carrier legs fold the same bottom (§29.1 `infantry_water.cpp`, §29.2
  `infantry_carrier.cpp`).

The measure below agrees: `bottom` is the hips' height above the ground and
`top` the head's, so `top - bottom` is the head over the hips.

**What the fields measure.** Nothing of retail's exporter survives, so the
relation is read off the corpus: every clip of every person table posed the
way the runtime draws it (each key composed as `key * bind_reset^T` about the
reset clip's rest offsets, translation rows added, the clip frame's y up). The
rig's rest positions are the MODEL's, never the `.bad` table's:
`BoneAnim_BuildWorldMatrices @ 0x40C400` reads the bone count at
`modelDef+52`, the bone table at `+56`, and per row the parent at `+20` and
the rest position at `+36..+44` (`@0x40C5EF..0x40C619`).

- **`top` is the head bone's height above the ground**, `top = bottom +
  y(head)`. Over the 30 person tables (every one binds US01's reset clip
  DT1RST.BAD; DEFAULT.ADM's `default.bad`, whose top 0.1201 sits below its
  bottom 0.8688, is left out) and 184,515 posed frames, the median error is
  0.2 mm: 65.7% of frames within 1 mm, 91.3% within 1 cm, 97.3% within 3 cm,
  the worst 16 cm (DT1DEATR.BAD's fall, frame 30). By US01's clip families
  (medians): idle and held poses 0.1 mm, the run 4.6 mm, the prone crawl
  1.6 cm, the crouch-move 2.4 cm.
- **`bottom` is the hips' height above the ground**: bone 0, the model's
  origin, over the planted foot's sole. On US01's three idle clips the lower
  foot sits 0.1251 above the ground on every frame (the reset pose's ankle,
  0.1259); the run's and the crouch-move's lowest tenth of frames reach 0.131
  and 0.125. The hip bob lives in `bottom` (0.90 to 1.01 over DT1runF).
- **`velocity` is the hips' ground step per frame**, x lateral and z forward in
  the clip frame: event `f` is the step from frame `f` to `f + 1`. A planted
  foot slides by `-velocity` in the hips-origin frame: `|slide| / |velocity|`
  is a median 1.000 over US01's 8 run clips and 0.951 over its 8 crouch-moves.
- **The hips never translate.** 0 of the 202 translated clips carry a nonzero
  bone-0 row: the body animates in place about its hips, and the ground travel
  is the events'.
- **The ground is in the model.** US01's skinned mesh part (part 19, the mesh
  object's pivot) sits at z -1.0185 in the model frame, which is the reset
  pose's `bottom` (DT1RST.BAD, 1.0185 on every event).

**First person.** Over the 44 `_1st` tables' 204 clip registrations the
velocity is zero on every event, and `bottom` is the viewmodel rig's origin
height, 1.0692 or 1.0699, on every event of 201 (the Vmne mine's three clips
stand 0.107). The rig has no head, and `top = bottom` in 137 registrations
(every clip of `ak47_1st` and `357_1st`, three of `mp5_1st`'s five). The
other 64 carry a `top` above it that no pose of the head-less rig reproduces
(1.6578 in 21 reset-clip registrations, `bottom` + 0.5886, near the
shoulders' height; 1.036 to 1.972 in 43 draw, fire and reload clips): that
rule of the exporter's is unwitnessed. The viewmodel rig overlays
the third-person body in the same hips-origin space: ArmsG's shoulders sit
0.587 and 0.585 above its origin where US01's upper arms sit 0.593, and the
elbows sit 0.445 to either side against US01's 0.438.

**The terminal events are the exporter's, not samples of the last poses.**
Every one of the 477 clips carries `frame_count + 1` events, and:

- event `frame_count` repeats event `frame_count - 1` (velocity, `bottom` and
  `top`) in 477 of 477;
- in the 273 loops (`flags & 1`) both equal event 0 in 273 of 273, though key
  `frame_count - 1` equals key 0 in only 30 of the 269 densely keyed loops
  (key `frame_count` in 127): DT1runF's events 37 and 38 copy event 0, where
  frame 38 is frame 0's pose again and frame 37 a real in-between one;
- in the 204 one-shots both carry zero velocity in 204 of 204, though 32 of
  the 43 moving one-shots still step at event `frame_count - 2` (CLIMTOP.BAD:
  0.083, 0.070 and 0.033 m, then 0 and 0); their `bottom` and `top` are frame
  `frame_count - 1`'s own (over US01's 28 one-shots, event `frame_count - 1`'s
  `top` fits that frame's head to a median 0.5 mm, frame `frame_count - 2`'s
  to 2.7 mm);
- the trigger word is not copied: event `frame_count`'s equals event
  `frame_count - 1`'s in 246 of the 273 loops and 176 of the 201 version-1
  one-shots, and is zero in 257 and 195 of them.

The channel never reaches `t == 1.0` (above), so event `frame_count` is read
only as the lerp partner of the last interval, which it holds constant.

**The seam carries the events as given.** The ground is not in a clip: a clip
poses the body about its hips, so nothing in its keys says how high the hips
stand. `bad_build` therefore takes every event's `bottom` and `top` as the
author states them, and the `.o3a` event record requires both
(`docs/anim/o3a-scene-format.md`). An authoring front end that places the
ground (the Blender rig's `Root`, ADR 0047) measures them the way the corpus
shows; `anim scene` writes every clip's stored pair, which is how a retail
clip survives an authoring round trip.

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
- `flags` bit 3 and the `top` of the head-less viewmodel clips that stand above
  their `bottom` are the two unwitnessed corners above: the first wants a look
  at the loader, the second at whatever retail's exporter was. A third is the
  bind a clip's bone past its reset clip's bones turns through (the writers
  section): the seam uses the clip's own first key until the runtime's read
  for such a bone is witnessed.
- Translation under a cross-fade (witnessed 2026-09-25, not ported).
  `AnimChannel_BlendTwoChannels @ 0x410740` always blends the rotations, but
  fills the translation scratch by the two clips' flags: both translated, their
  lerped rows blend by the channel weight (`@0x410C71..0x410D61`); one, that
  clip's rows go in whole and unweighted (`@0x41099B..0x4109E1`,
  `@0x410B12..0x410B58`); neither, the scratch keeps what it last held (the
  rotation-only loop `@0x4107BA..0x410894`). The bind gate above then applies.
  `SkeletalClips::eval_pose_blended` lerps the two sampled poses' origins, so a
  translated clip cross-faded with an untranslated one reaches its translation
  over the window instead of at its start.

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
