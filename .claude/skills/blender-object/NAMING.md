# ONED object naming convention (reference)

An object's 3DI role is encoded in its **scene-object name**. The importer assigns these
names (`apps/importer/scene_builder/`), the exporter classifies by them
(`blender/ase_exporter.py`), and the authoritative parser is `classify_name()` in
`libs/oed/src/convert_internal.cpp` (a port of `[orig: ConvertToInternal @ 0x4268B3]`).
Keep names exactly as below. **Indices are two-digit and 1-based** in the name (`01` is the
first), stored 0-based internally. The artist-facing source is
`godot/modtools/object/README.md`.

## Role → name table

`classify_name` keys on the **first one or two characters** (uppercased) of the name and
returns an `objType`. The full set:

| Role | Blender object | Name pattern | Example | objType |
|---|---|---|---|---|
| Part / subobject node | Empty | `PN` + 2-digit | `PN01` | (writer-only; see below) |
| Render mesh | Mesh (under its part) | 2-digit part index + ` Mesh` + n | `01 Mesh0` | 4 (leading digit) |
| Center marker | **Mesh** (small cube) | `_` + 2-digit + ` center` | `_01 center` | 1 (`_`) |
| Attach point | **Mesh** (small cube) | `~` + 2-digit *parent* + letter + ` attach` | `~01a attach` | 2 (`~`) |
| Stretch box | **Mesh** | `#` + 2-digit | `#01` | 3 (`#`) |
| Collision volume | Mesh | `<code>` + index + `-colonly` | `CB01-colonly` | 5 |
| Occlusion volume | Mesh | `<prefix>` + index + `-occonly` | `OB01-occonly`, `OP01-02-occonly` | 5 (types 20–23) |
| User point | **Mesh** (small cube) | `USR` + index, or `UP<c>` + index | `USR01`, `UPm01 muzzle` | 6 (`U`) |
| Light | Light object | `LP` + index | `LP01` | (type `LIGHT`, not name-classified) |
| Bone | Armature bone | `BN` + index | `BN01` | (skin; excluded from collision) |
| Material | material slot | `Material_<index>_<shader tag>` | `Material_0_FF_ST_OP` | — |

Only the `PN##` **part node is an Empty**. Centers, attach points, stretch boxes, and user
points are **Mesh** helpers (the importer makes them tiny cubes) so they survive export
through the helper-mesh path (`blender/ase_exporter.py` exports meshes/PN-dummies/lights/
bones; a non-PN Empty would be dropped).

**Name modifiers** (from `classify_name`): a leading `!` makes the object **ignored
entirely**; a `$` anywhere in the name sets a per-face `hasDollar` special flag.

Classifier regexes the exporter enforces: part empty `^PN\d{2}$`; render mesh
`^\d{2} Mesh\d+$`; source material name must start `Material_<int>_`. A leading `PN` is
stripped on export only for geometry/marker nodes (`PN01` → parent `"01"`); the PN *dummy
node itself* exports as `"PN01"`. Blender `.001` duplicate suffixes are stripped before
classification.

## Parts and render meshes

- Every subobject is a `PN##` **Empty** (the part node) with its render geometry as a
  child **Mesh** named `## Mesh<n>` whose leading two digits equal the part index.
- A static prop is usually one part: `PN01` + `01 Mesh0`.
- `subobject_0` / index `00` parses as zero and is silently dropped — always start at `01`.

## Centers, attach points, stretch boxes (placement matters)

These are **not** decorative and they are **not** all at the origin — they carry the
subobject's transforms. Position them in part-local space the way the importer does
(`apps/importer/scene_builder/overlays.py` `create_scene_markers`):

- **Center `_NN center`** — parented to its `PN##`, sitting at the **part node origin** (the
  subobject's pivot/center). If the part is part-animated it also carries the rotation from
  its PANM matrix; a static part with no matrix carries `opennova_zero_axis=True` (the
  exporter then writes an all-zero TM → PANM matrix index `0xFF`). So the center is only at
  world origin when the part node itself is at world origin — for a real multi-part model
  each `PN##` sits at that part's pivot, so each center lands at a different place.
- **Attach `~PPx attach`** — parented to the **child** part node; the name's two digits are
  the **parent** subobject index (`PP`), the duplicate letter (`a`,`b`,…) disambiguates
  siblings. Its position is the child→parent connection point. The export writes its
  `*NODE_PARENT` as the child subobject, so the pair `(name=parent, node-parent=child)`
  rebuilds the tree. Root parts get **no** attach.
- **Stretch box `#NN`** — objType 3; a deformation/stretch helper for part `NN`.

## Collision & occlusion volumes (`<code>NN…-colonly` / `-occonly`)

Both are helper **meshes** classified by `classify_name` objType 5: leading char in
`C B L D O V`, second char `A`–`Z` (excluding `BN*` bones and `BIP*`). The two letters map
to a numeric **collidableType**; `libs/oed/src/export_3di.cpp` then routes types **20–23
(0x14–0x18)** to *occlusion* objects and all others to the *collision* BVOL list. Use the
`-colonly` suffix for collision, `-occonly` for occlusion. Repeated same-kind volumes get a
base-26 letter suffix (`CB01`, `CB01a`, `CB01b`, …).

> **The primary names come from the bundled Super OED Manual v1.1, §1.1.3.4; runtime
> behavior is separately witnessed** (docs/world/world-wac-ai-re.md §15.4 [orig:
> Entity_ComputeBoneCollisionForce @0x4ae150 dispatch]): 1 `CB` generic solid (the only
> BVOL type generic rays clip), 4 `CL` ladder contact/alignment (the reimpl decodes the
> frame but has no climb motor), 5 contact-no-force, 6 `CA` armory zone (gates the
> in-game weapon.mnu armory), 7 `VC` vehicle-collision solid on the vehicle mask,
> 8 `BB` blink box (interior detection — accum bit 2 sets entity flag 0x800000),
> 9 `CD` door activation (section-touch callback), 10 `CT` change-team touch,
> 11 vehicle-loadout zone (gates vehicle.mnu), 12 masked, 13 `CF` flag/special-function
> touch (grounded only), 16/17/18 `DH`/`DM`/`DL` contact damage (-50/-6/-1 HP),
> 19 `CP` player collision (not AI), 20..23 occlusion. The "hint" column below
> predates that witness; trust §15.4 where they differ.

| Code | collidableType | Routing | Authoring meaning / note |
|---|---|---|---|
| `CB` | 1 | collision | Generic Collision Box (manual) |
| `CS` | 2 | collision | sphere? |
| `CC` | 3 | collision | cylinder? |
| `CL` | 4 | collision | Collision for Ladder (manual) |
| `CV` | 5 | collision | — (convex? vehicle? **unverified**) |
| `CA` | 6 | collision | Collision Box for Armory (manual) |
| `VC` | 7 | collision | Collision for Vehicles (manual) |
| `BB` | 8 | collision | Blink Box (manual); `W`/`S`/`V` preserve water/sky/voxels. Export also accepts reconstructed `L`/`O`; all suffixes clear bits of `bvolFlags` from `0x3E` until a digit. |
| `CD` | 9 | collision | Door / moving-part activation; manual: Collision for Doors |
| `CT` | 10 | collision | Change Team Box |
| `CM` | 11 | collision | — |
| `VK` | 12 | collision | — |
| `CF` | 13 | collision | Flag (project term); manual: activates special functions such as FARPs |
| `LP` | 14 | collision | — (`LP` is also the light prefix; a LIGHT object is a light, a `LP##-colonly` **mesh** is this collidable) |
| `CP` | 19 | collision | Player Collision; affects players, not AI |
| `DH` `DM` `DL` | 16 / 17 / 18 | collision | Damage High / Medium / Low |
| `OB` | 20 | occlusion | box? |
| `OS` | 21 | occlusion | sphere? |
| `OP` | 22 | occlusion | portal — reads a connecting subobject after `-`: `OP01-02-occonly` |
| `OH` | 23 | occlusion | — |

Writer side (`pyopennova.scene_naming.CollisionType`) emits `CB CS CC CL CV CA VC BB CD CT CM VK CF
LP CP` and assigns each a debug color; occlusion is emitted via `OcclusionType`
(`OB OS OP OP2`) with the `-occonly` suffix.

## User points (`UP<c>NN <label>`)

User points are named, oriented points the rest of the engine looks up **by label** — e.g.
`def.cpp` `launchuserpoint` / `particleuserpoint`, and the engine's entity-placement
`"Ground"` lookup and weapon-bone scan (`prim`/`bullet01`/`flare`) in
`docs/world/world-wac-ai-re.md`. They are Mesh helpers (small cubes) carrying position +
rotation; the 3DI `UserPoint` is `{name[32], pos[3], subObj, type, axis[4][3]}`.

Name format `UP<c>NN <label>` (`convert_internal.cpp` objType 6 parse):

- **Must start with `UP`.** `convert_internal` skips any name whose first two chars aren't
  `UP` (`if (up0!='U' || up1!='P') continue;`). The importer emits `USR##` when a point's
  type byte is non-printable, but a **`USR`-named point is dropped on ASE→3DI export** — so
  for authoring, always use `UP<c>`.
- `<c>` = `name[2]` = the **type byte** (a category; its taxonomy is not fully witnessed —
  the README uses `UPm01 muzzle` for a muzzle, but don't assume a complete table).
- `NN` = `name[3..4]` = the **owning subobject** (1-based), same as the part it rides.
- The **label is the text after the first space** and is the engine lookup key
  (`UPg01 Ground` → name `"Ground"`; no space → `"Noname"`). Multiple points may share a
  subobject — they're distinguished by label.
- Position is the marker's centroid; **orientation comes from the object's TM** — the
  importer maps a point's local **+Z to its facing direction**, so an unrotated marker faces
  straight up. Orient direction-bearing points (seats, muzzles, exhausts) deliberately.

### Vehicle seating user points

Mountable vehicles/emplacements expose seats as user points whose **label** the engine
classifies by name (`[orig: Entity_FindBestSeatSlot @0x4351f0]`, `libs/world/.../entity.h`
`SeatType`):

| Label prefix | SeatType | Notes |
|---|---|---|
| `site` | Passenger (1) | one per passenger seat |
| `ctrl` | Controller (2) | vehicle entity only |
| `drvr` | Driver (5) | vehicle entity only |
| `UseGun` | Gunner (3) | emplaced gun offers a single gunner |

So a driver position is `UP<c>NN drvr0` (or `ctrl0`) and passengers are `UP<c>NN site0`,
`site1`, … — the `drvr`/`ctrl`/`site`/`UseGun` text is the engine key; the trailing index
disambiguates multiples (its exact 0- vs 1-based scheme is not pinned in our docs). Each
seat's **+Z should face vehicle-forward** so the occupant faces the right way. The occupant
pose ultimately reads the seat transform (`Entity_GetBoneTransformAndOrientation @0x4b0c50`).

## Lights (`LP##`) and bones (`BN##`)

- Lights are `LP` + index; they are `LIGHT` objects (or empties) so they never reach the
  mesh-name classifier.
- Bones are armature bones `BN` + index (`BN01`); the classifier explicitly excludes `BN*`
  (and `BIP*`) from collision so a rig isn't misread as a collision volume.

## Materials (`Material_<index>_<shader tag>`)

The leading integer sets export order (so ASE order matches `.3dp`); the shader tag must
be one of the canonical tags from `libs/oed/include/oed/types.h` `kMaterialInfoTable` (OED
keeps this table rather than parsing `.fx`). Common tags:

- `FF_ST_OP` — fixed-function single-texture **opaque** diffuse (default static prop).
- `FF_ST_AB` — single-texture **alpha-blended**. `FF_ST_AD` — alpha-tested.
- `FF_ST_OP_LUM` — opaque + self-illumination (luminance/emissive).
- `FF_MT_OP` / `FF_MT_AB` — multi-texture (secondary) opaque / alpha.
- `FFP_GLASS` — glass.
- `VS_PHONGT`, `VS_DOT3DIFF` — vertex-shader bump/normal diffuse (smooth).
- `VS_SKBASIC`, `VS_SKBUMPPHONGT` — skinned (filtered) basic / bump-phong.
- `VS_FLAG` — flag/cloth.

A `#UV` suffix variant exists for most FF/VS tags (UI toggle) — only use it if you
explicitly need it. Set the chosen tag both in the material name and the `opennova_shader`
custom property.

## LODs (file-level, not a name suffix)

Draw-distance variants are grouped by an integer `_lod_index` custom property on each
**LOD-root Empty**: `_lod_index == 0` is the primary LOD and writes the main `<stem>.ase`;
each root with `_lod_index > 0` writes `<stem>_lod<N>.ase`. Per-LOD meshes reuse the same
`## Mesh<n>` / `PN##` naming within their own root hierarchy. Single-LOD objects set no
`_lod_index` at all — the whole scene is one model.

## `opennova_*` custom properties the exporter reads

- `opennova_shader` (str) — preserve the original shader tag for a material.
- `opennova_zero_axis` (bool) — write an all-zero rotation TM (no-matrix center markers).
- `opennova_bone_index` (int) — fallback bone index on a vertex group when the group name
  isn't `BN##`.
- `nl_ase_name` (str) — original pre-dedup node name, preferred over `obj.name`.

## Limits

- Texture filename (including extension) ≤ **15 chars**; `.dds` is remapped to `.TGA`.
- Materials split into Multi/Sub-Object slots of **64** submaterials each (OED hard limit).
