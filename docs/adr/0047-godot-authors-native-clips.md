# ADR 0047: Godot authors native clips

- **Status**: accepted (2026-09-06)
- **Owners**: the Godot layer (the `opennova_model` editor plugin, `godot/src/model/`),
  the `.bad` and `.adm` format libraries
- **Supersedes/updates**: extends [ADR 0046](0046-godot-authors-native-models.md)
  from models to the clips that animate them and retires its decision 5's
  interim (the retail reset clips' row order was the rig's contract only
  until our own clips existed); flips [ADR 0038](0038-native-runtime-assets-glb-editor.md)'s
  retirement of the `.bad`/`.adm` writers (they return, from scratch, as the
  clip tool's output path); [ADR 0003](0003-no-raw-passthrough-create-from-scratch.md)
  and [ADR 0021](0021-avatars-def-writer-policy.md)'s writer-policy shape stand.

## Context

ADR 0046 gave models a Godot source and left the clips under `assets/`'s
`TEMPORARY` banner: 156 retail `.bad` clips and three `.adm` tables, the last
retail bytes the minimal set needed beside two definition tables. The rigs
authored under ADR 0046 kept the retail reset clips' bone-row order so those
clips could still drive them. A clip is the other thing an artist shapes, and
the thing that shapes it in Godot already exists: an `AnimationPlayer` over
the rig's `Skeleton3D`, keyed in the editor or imported with the mesh.

The runtime pins what a clip means (`docs/anim/adm-bad-format-re.md`): a
rig's skeleton is the model's bone table, the `.adm` slot-0 clip's bone
records are the bind every clip is measured against, a channel is the row's
model-space rotation composed against that bind, and `BadBone.position` is
dead data the corpus derives from the model. The question this record
settles is how a Godot Animation becomes that file, and what our own clips
carry as their bind.

## Decision

1. For clips, the Animations in the rig's authoring scene are the source. A
   typed record beside the scene (`ClipSetSource`: the scene, the frame rate,
   the ground-proxy bone, one `ClipSpec` per clip and one `AnimSetRow` per
   `.adm` row) names what a scene cannot spell: the clip's file stem, its
   loop flag, the root motion its events carry (metres per second, forward /
   lateral / vertical; the body is animated in place), the per-key trigger
   words, capsule overrides, and the table binding anim keys to clip rings.
   The exported `assets/<clip>.bad` files and `assets/<set>.adm` are built
   artifacts, byte-guarded by re-export (`godot/tests/clip_export_guard_test.gd`).
2. The projection is the inverse of the runtime's load. `ClipProjector`
   samples the scene's `Skeleton3D` through the Animation's bone tracks at
   every key tick (`frame_count + 1` keys, the header counting intervals),
   keys each BN## row's model-space rotation relative to its rest, stores the
   presentation-frame parent-relative pivots as the bone positions (what
   `positions_from_model` reconstructs), and emits a per-frame translation
   block only when a bone leaves the pivots' forward kinematics.
3. Our rigs carry an identity bind: every clip's bone records hold the
   identity rotation and the reset clip holds the rest pose, so
   channel-at-reset is the identity the stored bind inverts to. The runtime's
   composition (`Transpose(bind) x channel`) is then the channel itself, and
   the Skeleton3D rests it builds are translation-only, the original's own
   `T(-pivot)` bind. A clip set switches whole: the `.adm` names our reset, so
   nothing composes a retail clip against our bind or ours against retail's.
4. The `.bad` writer emits the loader's layout with retail's conventions
   (absolute child/parent addresses, zero for a root's parent and a leaf's
   child, the bone index in its spare byte, the header words every retail
   clip ships); the `.adm` writer emits the canonical row shape. Parity is
   the reader's: our fixtures write back byte-exact, retail's own files
   write back field-equal (their name fields carry uninitialized bytes past
   the NUL no parse can reproduce).
5. The infantry default clip set is the authored body set
   (`kDefaultInfantryAdm = "person.adm"`); `failsafe.bad` is the body reset
   under the literal name every mission start loads; `weapon.def`'s
   `ANIMADM` and `items.def`'s `anim_def` name the authored tables.

## Consequences

- The formats: `engine/formats/bad/bad_write` and `engine/formats/adm`'s
  `adm_write_buffer` / `adm_write`, with ctests `bad_roundtrip` (the fixtures
  byte-exact, a from-scratch translated clip field-equal and re-write-stable,
  the shipped `BINOC.bad` as the asset-gated leg) and `adm_write`.
- The bindings under `godot/src/model/`: `ClipDocument`, `AnimDefDocument`,
  `ClipSpec` / `AnimSetRow` / `ClipSetSource`, `ClipProjector`; the workflow
  `godot/tools/clip_export.gd` with the headless `--export-clips [--verify]`
  command and the plugin's **Export all authored clips**.
- The first authored sets: the body's (`godot/authoring/person/person_clips.tres`
  over `person.tscn`'s AnimationPlayer: reset, idle, one walk cycle exported
  in eight directions by turning its root motion, a run, a jog, crouch and
  prone holds with their eight-direction walks, jump, reload and one fall
  every death key plays) and the rifle's (`godot/authoring/akm/akm_clips.tres`
  over `fp_rig.tscn`, a 46-row skeleton with the rifle's pivots: reset,
  idle, fire with the bolt's translation, reload with the magazine's drop;
  the nine keys of retail's AKM table). The rifle's 45 parts and the arms'
  38 rows read their prefix of the 46 rows, as retail's did.
- Retired from the banner: 156 `.bad` clips and `US01.ADM`, `E_STAND.adm`,
  `AKM_1ST.adm` (161 -> 2: `charattr.def` and `SndProf.def` remain, the
  definition slice's).
- The coupling guard (`minimal_model_validate`) measures every rig against
  its own reset clip now; the retail reset clips are gone.
- Not in this record: clip preview in the editor beyond Godot's own
  AnimationPlayer over the authoring scene; `.adm` variant rings wider than
  one clip (the record supports them, the first sets use aliases); an
  AnimationPlayer-driven root-motion track (the spec carries a constant).

## Verification

- ctest: `bad_roundtrip`, `adm_write`, `bad_parse`, `anim_sample`,
  `root_motion`, `minimal_model_validate` (the rigs against
  `person_rst.bad` / `akm_rst.bad`), `minimal_pff_package`,
  `minimal_def_validate`, `minimal_runtime_loadout`.
- GUT: `clip_document_test.gd`, `anim_def_document_test.gd`,
  `clip_projector_test.gd` (the rest hold, the mirrored-axis crossing, and
  the runtime's own loader playing a projected clip back to the Godot pose),
  `clip_export_guard_test.gd` (every clip set re-projects byte-equal).
- The retail A/B of `assets/README.md`: the player walks and fires with our
  clips on our rigs in `Jointops.exe /w /d /FRISK`.
