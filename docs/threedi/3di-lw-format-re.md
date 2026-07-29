# Land Warrior .3di (v8/v10) - format RE record

**Status: Land Warrior `.3di` import is unlanded (PR #45 closed); this format record is the
durable reference.** All IDA addresses cite `Dflw.exe` (Delta Force: Land Warrior client,
imagebase `0x400000`) unless noted otherwise.

> Consolidated 2026-06-10 from `notes/lw-3di-format.md` and `notes/3di-lw/lw-saf-pose.md`.

v8 semantic cross-reference: [`Acruid/NovalogicTools` `File3di.cs`](https://github.com/Acruid/NovalogicTools/blob/HEAD/Tools/lib-novalogic/3DI/File3di.cs)
(documents v8 exclusively; throws on non-v8).

---

## 1. Container: v8/v10 layout

### 1.1 Versions and detection

The 4-byte signature is `'3' 'D' 'I' <version>`:

| Version | Magic LE | Handled by | Reference |
|---|---|---|---|
| **8** (`0x08`) | `0x08494433` | older NovaLogic engine | NovalogicTools `File3di.cs` |
| **10** (`0x0A`) | `0x0A494433` | `Dflw.exe` | IDA RE below |

The LW loader **rejects byte[3] < 10** `[orig: LW3di_Load @ 0x47CB60, check @ 0x47cbd6]`, so v8
files do not load in the shipped client; they belong to an earlier build. The two versions are the
same format family with different struct sizes (v10 records are larger): one parser with a version
branch covers both.

**Difference from JO-era `3DI3`:** the JO-era format is a chunked container (size dwords with a
parent flag in the high bit, low 24 bits = length), while LW v8/v10 is a flat sequential dump of
in-memory structs - fixed-size header, then arrays, with **stale runtime pointers serialized in
place** (see §1.3). Detection must run *after* the `"3DI3"` check, since `"3DI3"` also has
`magic[0..2] == "3DI"`: LW = `magic[0..2]=="3DI" && magic[3] in {8,10}`
(registration point: `threedi_ir_read`, `libs/threedi/src/threedi_ir.cpp`).
LW also embeds no skeletal animation and uses no `.bad`/`.adm` files; animation is the `.SAF`/`.KSA`
sidecar family (§2).

### 1.2 IDA function map (v10)

| Address | Name | Role |
|---|---|---|
| `0x47CB60` | `LW3di_Load(model_out, filename, ctx)` | Top-level loader/validator: magic + 160-B header + material block + per-LOD dispatch |
| `0x47CF80` | `LW3di_LoadLod` (thunk `0x402D97`) | Reads 232-B LOD header + blob; computes sub-array pointers by accumulation |
| `0x47E040` | `LW3di_ReadMaterial` (thunk `0x40258B`) | Reads one 80-B material record; dedups against global table; creates D3D textures |
| `0x47CEC0` | `LW3di_FaceUvMasks` (thunk `0x402AEF`) | Precomputes pow2 log/mask UV-wrap fields into a face record |
| `0x47C7E0` | `modelbin_IntegrityCheck` | Validates faces→material ptr, animation overrun, material-name len ≤ 15, pow2 texture size (documents struct invariants) |
| `0x4F4F00` | `Pak_Open` | Opens file across up to 16 mounted PFF archives |
| `0x4F50E0` | `Pak_Read(buf, n)` | Sequential read from the open file |

Global tables: materials at `unk_915E68` (80-B stride, count `dword_915E60[0]`, max 2500); LOD pool
at `unk_652E08` (232-B stride, max 600); model pool at `unk_7FBD0C` (160-B stride).

### 1.3 v10 file layout (byte-exact, validated)

```
+0x000  Header                      160 bytes (0xA0)
        u32   material_count
        Material[material_count]    80 bytes each
        Lod[header.lod_count]       { 232-byte LodHeader ; data blob of LodHeader.blob_size bytes }
EOF     (cursor lands exactly here for all 971 v10 samples)
```

Read order in `[orig: LW3di_Load @ 0x47CB60]`: 160-B header → *(optional)* `pre_count` × 20-B
records → `u32 material_count` → `material_count` × 80-B materials → per LOD: `LW3di_LoadLod`.

#### Header (160 bytes)

| Off | Type | Field | Notes |
|---|---|---|---|
| `0x00` | char[3] | magic | `"3DI"` |
| `0x03` | u8 | version | `0x0A` (10) |
| `0x04` | char[~16] | name | null-terminated model name |
| `0x14` | u32 | lod_count | ≤ 4 |
| `0x18` | u32 | lod_dist_high | Q16.16 distance threshold |
| `0x1C` | u32 | lod_dist_med | Q16.16 |
| `0x20` | u32 | lod_dist_low | Q16.16 |
| `0x28` | char[4] × lod_count | render_tags | per-LOD 4-char render type (e.g. `"crng"`, `"npwe"`, `"0gro"`, `"oleh"`) |
| `0x78` | u32 | pre_count | count of 20-B records following the header; **0 in all 971 samples**; loader allocs `20*pre_count` `[orig: @ 0x47cbf2]`; purpose unknown |
| `~0x58+` | - | **stale pointers** | the header is an in-memory struct dumped to disk; from ~`0x58` onward most dwords are runtime pointers (`0x0012xxxx` stack, `0x76b3xxxx` DLL); the loader overwrites them. **Ignore on read; zero on write.** |

`0x18/0x1C/0x20` are the direct ancestor of GP's `lod_thresholds[3]` and `0x28` of GP's `model_tag`
(`libs/threedi/include/threedi/threedi_gp.h`). The 4th distance slot (`DistTiny` in v8) is unused at v10.

#### Material record (80 bytes) `[orig: LW3di_ReadMaterial @ 0x47e073]`

| Off | Type | Field | Notes |
|---|---|---|---|
| `0x00` | char[16] | tex_name_0 | primary texture (`.PCX`/`.TGA`), name ≤ 15 chars |
| `0x10` | char[16] | tex_name_1 | secondary texture / second name |
| `0x24` | u32 | group_id | compared during dedup |
| `0x28` | u16 | selector_id | matched against surface selector bytes at `surface+0x34+selector_slot` for variant materials |
| `0x2A` | u16 | flags | `0x08`=force-load, `0x100`/`0x200`=has tex0/tex1 (alpha modes), `0x600`/`0x108` matched in dedup, `0x80`=duplicate-of-existing |
| `0x2C` | u16 | tex_width | clamped ≤ 256; power of two (integrity-checked) |
| `0x2E` | u16 | tex_height | clamped ≤ 256; power of two |
| `0x30..0x40` | u32×5 | runtime D3D handles | zeroed on read, filled at texture create. **Ignore on read.** |

#### LOD header (232 bytes) + data blob

`[orig: LW3di_LoadLod @ 0x47CF80]` reads the 232-B header (`0x47cfb6`), then a single blob of
`blob_size` bytes (`0x47cff8`), then fixes up pointers. Confirmed count fields (dword index =
byte offset / 4); the odd indices in between (`[33] [35] [37] ...`) are runtime pointer slots
filled during fixup:

| dword idx | Off | Field |
|---|---|---|
| `[5]` | `0x14` | **blob_size** |
| `[32]` | `0x80` | n_vertices |
| `[34]` | `0x88` | n_normals |
| `[36]` | `0x90` | n_faces (80-B records) |
| `[38]` | `0x98` | n_array_12 (12-B records) |
| `[40]` | `0xA0` | n_subobjects (120-B records) |
| `[42]` | `0xA8` | n_triindex (12 B = 3×u32) |
| `[44]` | `0xB0` | n_surfaces (128-B named records) |
| `[46]` | `0xB8` | n_array_8b (8-B records) |
| `[48]` | `0xC0` | n_array_80b (80-B records) |

### 1.4 LOD data blob - 9 arrays, concatenated in this order (byte-exact)

From the pointer accumulation in `LW3di_LoadLod` (`0x47d0c6`-`0x47d158`). For all 971 samples,
`blob_size == Σ(count × stride)` for every LOD - zero leftover:

| # | count field | stride | content |
|---|---|---|---|
| 1 | `[32]` | 8 | **vertices** - `int16 x,y,z,w` (v8 confirms 4×int16; v10 vertex math at `0x47d4b5` touches +0/+2/+4 as `>>8` fixed-point) |
| 2 | `[34]` | 8 | **normals** - `int16 ×4` (matches v8) |
| 3 | `[36]` | 80 | **faces** (triangles; §1.5) - reference a surface by index at +0x4C; `<<7` (×128) fixup at `0x47d30a` |
| 4 | `[38]` | 12 | array_12 (likely collision planes - v8 has `nColPlanes`) |
| 5 | `[40]` | 120 | **sub-objects / skeleton** - hierarchy parent at +44 (`+0x2C`), fixed-point translation at +60/+64/+68 (`>>8`), owns vertex/face/index sub-ranges via count+ptr pairs at +4/+8, +12/+16, +20/+24, +28/+32, +36/+40 |
| 6 | `[42]` | 12 | triangle index triplets (3×u32) |
| 7 | `[46]` | 8 | array_8b (unknown; likely collision-adjacent) |
| 8 | `[48]` | 80 | array_80b (unknown; likely collision-adjacent - v8 has `nColVolumes`) |
| 9 | `[44]` | 128 | **named surfaces** - name[16] (≤ 15) at +0, flags at +0x10 (`0x4000`, `0x1`, `0x8000`, `0x800`, `0x200`), material index u16 at +0x18, anim-frame byte at +30, selector bytes at +0x34..+0x37, material ptr at +68 (the "face→mat ptr" the integrity check validates) |

Fixed-point: geometry uses `>>8` shifts (24.8) and a `× 0.62` factor `[orig: @ 0x47d37c]` on a
per-surface int16; exact unit labels for `>>8` / `×0.62` are still open (§3.3).

### 1.5 Face (80 B) + surface (128 B) semantics - VALIDATED

The 80-byte face record is a **triangle**; its first 40 bytes follow the v8 `ModelFace`. Fields
empirically validated across **578,424 triangles in 1,942 LODs**
(`libs/threedi/src/threedi_lw.cpp`, `tests/threedi/lw_parse_test.cpp`):

| Off | Type | Field | Validation |
|---|---|---|---|
| +0x04..+0x18 | int32×6 | `tu1..tu3, tv1..tv3` UVs (fixed-point, `/65536`) | from v8 correspondence |
| +0x1C/+0x1E/+0x20 | int16×3 | `vertex[3]` | 0 out-of-range vs vertex_count |
| +0x22/+0x24/+0x26 | int16×3 | `normal[3]` | 0 out-of-range vs normal_count |
| +0x4C | int32 | `surface_index` | 0 out-of-range vs surface_count |

Material is reached **indirectly**: `face.surface_index` → 128-B surface record; the on-disk
`material_index` (u16 @ +0x18) is only a fallback. The engine links variant surfaces by comparing
`surface[0x34 + dword_5B752C]` to the material selector id at `material+0x28`; `dword_5B752C` is
selected from the BMS model header by `[orig: sub_47E5C0]`, and standalone object preview uses
slot 0. Converter resolution order for flag `0x1`/`0x8000` surfaces: selector slot 0 → case-insensitive
surface name vs material texture names → the +0x18 material index. Surface flipbook frame span
(flag `0x4000`) is at +0x1E; surfaces named `DONTDRAW.PCX`, `DONTDRAW.TGA`, or `NoName` are hidden
and emit no render geometry. Texture/UV flipbook animation is the only animation inside the `.3di`
(128-B surface records with flag `0x4000` spanning `surface[+30]` following entries -
`modelbin_IntegrityCheck` "Animation overrun").

**Skeleton LODs:** LOD flag bit 0 marks the sub-objects as a skeleton, not rigid localized mesh
chunks. The 120-B sub-object positions are rest pivots, vertices remain in model/rest space, and
vertex `w` is the skeleton sub-object index for that vertex's (single) influence. `BADGUY.3DI`
proves it: LOD0 faces carry vertices whose `w` differs from the owning face range, so rigid
per-subobject rendering stretches the model. The converter preserves model-space positions and
emits IR primitive bone tables plus one-weight skinning for flag-1 LODs.

**Coordinates:** LW geometry is raw Z-up, +X-forward. The converter maps raw `x/y/z` → IR
`-y/z/x`; the Godot adapter's IR-X mirror presents that as Godot `y/z/x` (+Y up, +Z forward).
LW triangle corners are emitted as `0,2,1` so the final Godot winding is front-facing.

### 1.6 v8 layout and v8↔v10 correspondence

NovalogicTools reads v8 sequentially: header → vertices → normals → faces → sub-objects → bone
animations → collision → materials. Struct sizes:

| Struct | v8 size | v10 size | Notes |
|---|---|---|---|
| Header | 128 | 160 | v8 has `name[12]`, `HeaderLodInfo` (20 B: Count + 4 dists + 4 render-type enums), `TextureCount` |
| Texture header | 52 (`ModelTexHeader`: name[28], bmSize, index, flags, w, h, 3 ptrs) | - | v10 folds texture+material into one 80-B record |
| Material | 120 (`0x78`) | 80 | |
| LOD header | 192 | 232 | v8 fields: `nVertices, nNormals, nFaces, nSubObjects, nPartAnims, nMaterials, nColPlanes, nColVolumes`, bbox `x/y/z min/max` |
| Vertex | 8 (int16×4) | 8 | same |
| Normal | 8 (int16×4) | 8 | same |
| Face | 72 | 80 | v8 face = vtx idx, normal idx, `tu1-3/tv1-3` UVs, material idx |
| Sub-object | 112 | 120 | |

The v8 `ModelLodHeader` field names map directly onto the v10 LOD-header counts (§1.3), which is
how the v10 blob arrays were labelled. v8 is the cleaner reference for semantics; v10 for the
bytes that matter. v8 layout is not yet validated against the 3 local v8 samples (§3.3).

---

## 2. SAF/KSA animation sidecar

LW embeds no skeletal animation in the `.3di` and uses no `.bad` files. The model's sub-objects
(the 120-B records in §1.4 #5: parent at +44, rest translation at +60/64/68) ARE the skeleton;
separate sidecar files drive them.

### 2.1 Sidecar graph (observed in loose DFLW data)

```
ITEMS.DEF  chr_file player01 + anim_def playanim
  -> PLAYANIM.ANM : <name> <slot> [vel] [override]   (movement slot velocities)
  -> PLAYER01.KSA : 100-byte header, 255 slot records, 15,972 baked runtime frames
  -> PLAYER01.ACA : slot <slot_id> <saf_file> [loop_frame]
  -> 53 *.SAF     : per-clip overrides of specific slots
```

Loader addresses:

| Address | Name | Role |
|---|---|---|
| `0x44A0B0` | `LWAnim_LoadSAF1` | SAF clip → 88-B runtime frames; `+0x80` bias, root scalars/clamp |
| `0x449F50` | `LWAnim_LoadKsa` | 100-B header, 255 × 28-B slot records @ offset 100, then 88-B frames |
| `0x44AC50` / `0x44AB30` | `LWAnim_LoadAca` / `LWAnim_ParseAcaLine` | `slot <id> <saf> [loop]` |
| `0x44AA60` / `0x44A6D0` | `LWAnim_LoadAnm` / `LWAnim_ParseAnmLine` | `<name> <slot> [vel] [override]` |
| `0x44A320` | `LWAnim_LoadCharacter` | character-level binding |
| `0x4A0C00` | `LWAnim_PoseSkeleton` | runtime frame → 15 bone world matrices (§2.4) |

Clips are named by gait + equipped weapon (string table `crwalk0_knife`..`crwalk7_knife`,
`crwalk0_2hd`..; files like `3CROU01A.SAF`, `3SHOOT01.SAF`).

### 2.2 SAF1 on-disk layout

Magic `"SAF1"` = `0x31464153`, plain (unencrypted) in all 53 samples.

```
+0x00  char  magic[4]      "SAF1"
+0x04  u32   a             0x64 in samples (fps? subtype)
+0x08  u32   frame_count
+0x0C  u32   root_block    0x34 (52) - per-frame root block size
per frame:
  root block (52 B): u32 n_parts (<= 15) ; u32 ; floats[...]
  n_parts x 4-B part keyframes: u8 part_id (+0x80 bias applied by loader) ; remaining angle bytes
```

`[orig: LWAnim_LoadSAF1 @ 0x44A0B0]` consumes root-block dwords `[5,6,7,8,9,10]` as translation
x/y/z (`× 85.333336` → i16) and `[2,3,4]` as rotation (`× 1365.3334` → i16 BAM), with a pitch
clamp `root[7] > -30 → -30`. On-disk per-frame size = `root_block + 4*n_parts`; the cursor lands
exactly on EOF for every sample (e.g. `3CROU01A.SAF`: 39 frames × (52 + 15×4) + 16 = 4,384 B).

### 2.3 KSA slot records (28 bytes)

```
u32 frame_count
u32 runtime_pointer    original in-game memory address; ignore on import
u32 slot_id
u32 loop_frame
u32 unknown[3]         zero in current samples
```

### 2.4 88-byte runtime frame (one per animation frame)

Both SAF and KSA normalize into the same 88-B runtime frame shape:

- `+0x00..+0x3B` (60 B): **15 bone records, 4 bytes each: `[b0, b1, b2, pad]`** -
  b0/b1/b2 are **three 8-bit Euler angle bytes**.
  **Warning (dead end that corrupted poses):** an earlier reading of these records as
  `u8 token ; i16 angle ; u8 pad` was wrong; the `+0x80` bias the SAF loader applies to b0 is a
  signed→unsigned BAM bias, not an index. The value is consumed as a rotation angle.
- `+0x3C..+0x4F` (root block): **nine i16** derived from the SAF root floats in order
  `[5,8,6,9,7,10,3,2,4]`, with the `85.333336` (translation) / `1365.3334` (rotation) scalars
  and the `root[7] > -30 → -30` clamp baked in at load. Drives bone 0 (root) + object motion.

### 2.5 Pose builder recipe `[orig: LWAnim_PoseSkeleton @ 0x4A0C00]`

Fills `unk_8840C0` (15 bones × 64 B). High confidence: derived directly from the
matrix-construction code. Per bone `i` in 0..14 (sub-object array at `model+164`, stride 120):

1. Parent `p = subobj[i] @ +0x2C` (dword index 11).
2. Rest offset `o = subobj[i] @ +0x30/+0x34/+0x38` (dword 12/13/14) = the loader's
   `rel = pos - parent.pos`, fixed-point.
3. **World position** = `unk_8840C0[p]` (parent world matrix) · `o` (matrix×point via
   `sub_404138 → 0x458C60`). Position chains through the parent; bone 0 is the root (special, §2.7).
4. **World rotation** from the 3 angle bytes via `sub_402ECD → Entity_ClearSuspensionState @ 0x4592B0`:
   - `b2<<24` → **Rx**, applied FIRST (innermost): rows `[1,0,0] [0,c,-s] [0,s,c]`
   - `b1<<24` → **Ry**, second: rows `[c,0,-s] [0,1,0] [s,0,c]`
   - `b0<<24` → **Rz**, third (outermost): rows `[c,-s,0] [s,c,0] [0,0,1]`
   - Composite `R = Rz(b0) · Ry(b1) · Rx(b2)`. **Rotation is ABSOLUTE (model frame), NOT
     accumulated through the parent** - only position chains. This is the subtle point a naive
     port gets wrong.
5. Output 3×4 `[R | t]` row-major into `unk_8840C0 + 64*i` (`t` in column 3 = world position).

### 2.6 Angle unit and trig

- `byte << 24` over a 2^32 circle ⇒ `θ = byte · (2π/256)` rad (= byte · 1.40625°), standard CCW.
- Trig via fixed-point table `[orig: sub_442840 @ 0x442840]`: `idx = angle >> 21` (2,048 entries),
  21-bit lerp; `1.0 = 0x400000 = 2^22`; products `>> 22`.
- **Tables are runtime-generated** (`0x1103A20` sine; `0x5688F8 → 0x1104220` cosine = sine + 512
  entries = 90° phase) and are zero in the static image - an importer should use float `sin`/`cos`
  with the unit above. The sine/cosine assignment is pinned by the constraint **byte=0 must be
  identity**: only sine@`0x1103A20` / cosine@`0x5688F8` gives `Rx(0)=I`.

### 2.7 Root (bone 0) and gameplay-only terms

- Root is built after the 15-bone loop (`0x4A0E88+`) from globals `dword_88418C/88419C/8841AC`
  (fed by the frame root block) plus the live object heading. For import: apply the root block's
  translation (and optional rotation) as root motion; do NOT apply the gameplay heading.
- **Skip on import** (live game state, not authored clip data):
  - object-heading add on bones 1, 2 (`0x4A0D4E` / `0x4A0D6E`),
  - arm/aim clamp on bones 4, 6, 8 (`0x4A0DBC`).

### 2.8 Importer integration recipe

Compute each bone's WORLD matrix per §2.5, convert world → pose-local
(`parent.world⁻¹ · bone.world`), and feed the existing two-pass world→rest-local path
(`scene_builder/animation.py` `_build_action_from_bad`, which already does this for BAD). Reuse
`build_armature_from_parts` - the synthetic BN## bones already match the sub-object order.
Float math is fine; only axis order, angle unit, and absolute-vs-local rotation must match
(all pinned above). SAF `part_id` values map to the same sub-object indices used by vertex `w`.

---

## 3. Validator / verification status

### 3.1 Validated

| Claim | Evidence |
|---|---|
| v10 container + per-LOD blob layout byte-exact | **971/971** v10 samples reproduce exactly (corpus `~/Desktop/dflw_3di`, 974 files: 971 v10, 3 v8 - `JPAN8.3DI`, `PFB1.3DI`, `TESTPART.3DI`); scratch validator `validate_lw_3di.py` (notes/, untracked) |
| `blob_size == Σ(count × stride)`, zero leftover | every LOD of all 971 samples |
| Face vertex/normal/surface indices in range | **578,424 triangles / 1,942 LODs**, 0 out-of-range (`tests/threedi/lw_parse_test.cpp`) |
| SAF1 plain (unencrypted), per-frame size exact to EOF | **53/53** sample `.SAF`; scratch validator `validate_saf.py` (notes/, untracked) |
| Pose-builder recipe (§2.5-2.7) | read directly from matrix-construction code, not inferred from data |

### 3.2 Implementation state (at PR #45 close)

- `libs/threedi`: `threedi_lw.h/.cpp` parser (v10) + IR conversion with skeleton/one-weight
  skinning for flag-1 LODs; parse tests as above. v8 branch and textures deferred.
- `pyopennova.lw_animation` parses decoded `ANM`, `ACA`, `SAF1`, `KSA`;
  `build_animation_context()` returns `LwAnimationContext` when an ANM exists and ADM/BAD does
  not; `AssetResolver` decodes loose/PFF LW text assets before parsing. Blender imports LW skinned
  meshes with the BN## armature, binds one-weight skinning, and records the resolved animation
  inventory on the armature.
- SAF/KSA **playback was intentionally not applied** in the importer at close time; the armature
  records `lw_animation_status=parsed_not_applied_pending_re`. The pose recipe in §2 (pinned after
  that decision) is what a playback implementation must follow; the remaining gap is end-to-end
  validation against in-game playback.

### 3.3 Open RE targets

1. Runtime animation consumer end-to-end proof: validate §2.5's recipe (interpolation, loop_frame
   behavior, whether ANM `vel` affects sampling or only gameplay) against rendered playback.
2. Material parity: label render-state bits in `[orig: sub_47C130]`, surface mode byte +112,
   material flags `0x08/0x80/0x100/0x200/0x600/0x108`, secondary-texture bit `0x1000`, surface
   flipbook flag `0x4000`. Only proven alpha/blend/two-sided/filter/wrap behavior should move into
   IR material descriptors.
3. The 12-B `[38]` and 8-B `[46]` / 80-B `[48]` blob arrays (likely collision: v8
   `nColPlanes`/`nColVolumes`). Not needed for render; decode when adding collision.
4. UV/render-state semantics: `[orig: LW3di_FaceUvMasks @ 0x47CEC0]` only precomputes pow2
   log/mask fields into runtime data; face UVs remain fixed-point `tu/tv / 65536`.
5. Final semantic labels for the `>>8` (24.8) and `× 0.62` geometry scales and the SAF
   `85.333336`/`1365.3334` scalars.
6. Purpose of the 20-B `pre_count` block (0 in all 971 samples; needs a non-zero sample).
7. v8: validate the NovalogicTools layout against the 3 local v8 files before trusting it.

## 4. Divergence catalog (D-3DILW)

Stable IDs for the deferrals stated in §3.2/§3.3 (dispositions in the canonical vocabulary of
[divergence-ledger.md](../divergence-ledger.md)). The whole record is unlanded (PR #45 closed);
these rows ride whenever an LW import is revived.

| ID | Divergence | Disposition |
|---|---|---|
| D-3DILW-1 | v8 branch deferred: the `threedi_lw` parser handles v10 only; the NovalogicTools v8 layout is unvalidated against the 3 local v8 files (§3.2, §3.3 item 7) | **NEEDS-RE** — validate before adding the v8 branch. |
| D-3DILW-2 | Textures deferred: v10 geometry + one-weight skinning land in `libs/threedi`, but material textures are not ported (§3.2) | **WITNESSED-READY-DEFERRED** — material parity (§3.3 item 2) is the prerequisite grill. |
| D-3DILW-3 | SAF/KSA playback intentionally not applied (`lw_animation_status=parsed_not_applied_pending_re`); the §2 pose recipe is pinned but end-to-end validation against in-game playback is pending (§3.2, §3.3 item 1) | **NEEDS-RE** — prove the recipe against rendered playback. |

The remaining §3.3 items (collision blobs, UV/render-state semantics, geometry-scale labels, the
`pre_count` block) are unresearched RE targets with no witnessed behavior gap yet and stay in §3.3.
