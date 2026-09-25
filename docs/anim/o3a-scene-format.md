# The `.o3a` clip-set text

`opennova-3di anim build <set.o3a> -o <out.adm>` mints a `.adm` table and every
`.bad` clip it names from this text, through the engine's clip construction
seam (`engine/formats/bad/bad_build.h`) and the writers
([ADR 0047](../adr/0047-blender-3di-exporter.md)).
`opennova-3di anim scene <in.adm> -o <set.o3a>` writes a clip set, retail ones
included, back out as this text: it is build's exact inverse, so
`build(scene(x))` re-mints a builder-made set byte for byte. A lone clip works
the same way (`anim scene x.bad`, `anim build set.o3a -o x.bad`). The Blender
add-on (`tools/blender/opennova_3di`) writes it to export and reads it to
import; any other front end may.

One file is one clip SET: a rig's table and every clip it names. A clip's
channels pair with the MODEL's parts by index
([orig: `BoneAnim_BuildWorldMatrices @ 0x40c400`]), so a set belongs to the rig
it was authored on and to any rig that matches it.

## Conventions

- One record per line, its fields separated by whitespace (space, tab, `\r`,
  `\v`, `\f`). `#` starts a comment at the start of a line or after
  whitespace. The first record is `o3a 1`. The tokenizer, the name fields and
  the number spelling are the `.o3d` scene text's
  ([o3d-scene-format.md](../threedi/o3d-scene-format.md); one implementation,
  `apps/threedi_cli/scene_text.h`), except that no number here may be `nan` or
  `inf`.
- A name field (the table, a slot key, a clip, a bone) is a bare token, or
  `"quoted"` when it holds whitespace (a bone is named `BN01 Pelvis`). A quoted
  field runs to the next `"` and ends at whitespace, so a name cannot hold `"`:
  `scene` writes such a name without it and says so.
- A number is the whole token and finite (no `nan` or `inf`); a whole-number
  field (`fps`, `frames`, `version`, `flags`, a parent, a trigger word, a
  duration) is decimal or `0x` hex and within its field's range, so nothing
  wraps. A record takes exactly its fields: a trailing token is an error.
- Pivots, translations and event velocities are **mission axes**: x forward,
  y left, z up, metres. A rotation is a quaternion `x y z w` in the same axes.
  The seam converts to the clip's own frame (x side, y up, z forward), which is
  the model frame mirrored on x — the frame the runtime's skeleton is built in.
- A clip's `frames` is its INTERVAL count: every key list holds one more, and
  the event list and the translation rows likewise. The runtime reads
  translation row `trunc(frames * t)` and lerps it with the next one, so the
  last interval of every cycle reaches row `frames`
  [orig: `sub_4102D0 @ 0x4102d0` via `BoneAnim_TransformBones @ 0x410360`];
  the pad row retail's files carry past it is the writer's, never authored.
- A key is the bone's rotation in the model's frame (a world rotation, not a
  parent-relative one). The runtime deforms a bone by `key * bind^-1`, and the
  bind is the first key of the set's RESET clip (the `anim_reset` row's last
  variant), pinned once per entity; only a clip that plays with no reset pinned
  composes against its own first key
  [orig: `AnimChannel_ComputeBoneMatrices @ 0x410da0`;
  `AnimMap_RegisterEntity @ 0x40bb60`]. The reset clip's first key is the pose
  the rig's rest stands in, and every key of every clip turns from there.

## Records

| Record | Fields | Meaning |
| --- | --- | --- |
| `adm` | name | the table this set writes (the file name alone; `-o` names the file `build` writes, and a different `adm` name is noted, not used) |
| `row` | key variant [variant ...] | a table row: the slot and its clip ring, in the order the file stores. The key names its slot past its first five characters, whatever they are and in any case (`anim_reset`, `ANIM_RESET` and `xxxx_reset` all name slot 0) [orig: `AnimMap_FindSlotByName @ 0x40cfa0`]; `anim_<name>` is the convention every retail table keeps, and `opennova-3di catalog` prints the slot keys the runtime names. A variant names a clip with or without the `.bad` extension (440 of 5146 retail variants omit it). The engine serves a row from its LAST variant back [orig: `AnimMap_RegisterBoneNode @ 0x40C2D0`] |
| `clip` | name | opens a clip: the `.bad` file stem `build` writes beside the table |
| `fps` | n | the clip's own rate; every retail clip ships 30 |
| `flags` | word | 1 loop, 2 translations, 8 unwitnessed (73 retail clips carry it) |
| `frames` | n | the clip's length in intervals |
| `version` | n | the record version; 1 (a 24-byte event) unless stated, and 3 retail clips ship 0 (20 bytes, no trigger) |
| `capsule` | bottom top | one capsule pair for every event, the shape retail's viewmodel clips carry (0.0/0.6 or 1.07/1.07 across the JO `_1st` sets) |
| `bone` | parent x y z length name | opens a bone: its parent (a lower index, -1 for the root), its pivot (the paired model part's, absolute), its length and its name. The name is the clip's own: a model's part table carries none |
| `k` | qx qy qz qw [duration] | a key of the open bone, `frames + 1` of them. A `duration` (in frames, 1 to 65535) states how long the key holds; a bone that gives one gives it on every key, and may then key any number of times, which is how `DT1RST`, `stgr_RST`, `M60_1i` and the sparsely keyed `DVFLEE1E` are shaped |
| `tr` | x y z | a frame's displacement of the open bone, `frames + 1` of them (rows 0 to `frames`), under `flags & 2` |
| `bonepos` | x y z | the open bone's stored `position[3]`, verbatim and in the clip's own frame. `build` derives that field from the pivots, and the field is dead at runtime; `scene` writes this only where the derivation cannot reproduce the bytes (retail's exporter left junk in 6720 of 13517 bones) |
| `event` | vx vy vz trigger [bottom top] | a frame's event, `frames + 1` of them: the root's step for that frame (the body animates in place and the engine moves the entity by these), the event bit word (`opennova-3di catalog` prints the bits), and the capsule pair when the clip carries its own |

## What `build` derives

Nothing below is authored; the seam recomputes all of it on every build, from
the values the text carries, so `build(scene(x))` stays exact:

- the bone table's `rotation[9]`, the transpose of the bone's first key as a
  matrix (13,517 of 13,517 retail bones);
- `position[3]` from the pivots, through the parent's bind in the set's reset
  clip (a lone clip: its own):
  `position[i] = rotation_reset[parent(i)] . clip(pivot[i] - pivot[parent(i)])`,
  unless a `bonepos` overrides it;
- `num_children`, the child and parent addresses, and the bone's own index byte;
- the translation block's pad row after row `frames` (a repeat of it: retail's
  pad holds exporter memory that no read weights);
- the header words no field names (0, 0, 8, 1, 1, 0, 0 in every retail clip);
- an event's capsule pair, when neither the event nor a `capsule` record gives
  one, by OUR rule: the lowest and highest bone origin about bone 0 over the
  pose the runtime draws, each key composed against the set's reset clip (a
  lone clip: its own first key). Retail measured these by a rule nothing has
  witnessed, and this one does not reproduce the shipped numbers (a clip's
  worst frame is a median 0.6 m off over the retail tables;
  `docs/anim/adm-bad-format-re.md`), so a clip that must keep retail's values
  states them.

The keys themselves are kept verbatim: retail stores neighbouring keys in
opposite hemispheres (4,573 of 657,788 pairs) and unit only to 2.5e-7, and the
runtime's slerp short-arcs either way, so nothing re-signs or renormalizes a
channel.

## Validation

The build fails, naming the line (a clip the seam refuses is named by the line
it opens on), on an unknown record, a malformed or trailing field, a quote
that never closes or runs into the next field, a `"` inside a bare field, an
empty row variant, an event capsule with one value, a
set with no clip, a row key of five characters or fewer (it names no slot) or
a row naming a clip the set lacks, two clips under one name, a clip name or
row variant that is not a bare file name (`/ \ : | * ? < > "`, a control character, `.` or `..`: `build`
writes each clip beside the table), a bone whose parent is not a lower index, a
key list that is neither `frames + 1` long nor accompanied by durations (a
bone states a duration on every key or on none), a key
that is not a unit quaternion, a zero duration, a translation block a flag
promises and the clip lacks or that the flags do not carry, an event list that
is not `frames + 1` long, a `version` other than 0 or 1, a trigger word on a
version 0 event (its record has none), a frame count of zero, a bone name over
31 characters, or a clip over the 500,000 bytes the loader accepts
[orig: `BoneFile_Load @ 0x40fff0`]. Every clip and the table are minted in
memory, each clip read back through the loader's own reader and the table
through the `.adm` parser (every row must come back as written), before any
file is written, so a set that fails anywhere writes
nothing. `scene`, `info` and `compare` refuse a table whose variant names a
path.

## What `scene` cannot carry

`scene` comments these (`# note: ...`) and lists them on stderr: a variant
whose clip is absent or does not parse, which is dropped from its rows (a row
left with none is dropped whole), and a clip `build` could not mint again from
its text (an event count other than `frames + 1`, a `version` other than 0 or
1, a bone name filling all 32 bytes or holding a quote, a parent that is not a
lower index, a key that is not a unit quaternion, a zero duration, a sparse
channel with no duration table), which is left out with its variants. A
version 0 clip's events carry trigger 0: its record has no trigger word. Values
build derives are not carried, except where it cannot reproduce them
(`bonepos`).
Over the corpus under `OPENNOVA_JO_ASSETS`, `build(scene(x))` is the same
animation as `x` (`opennova-3di anim compare`) for all 477 clips and 81 of the
82 tables; the one exception, `ESTAND02.ADM`, names a clip the corpus does not
ship.
