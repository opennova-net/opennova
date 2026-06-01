# Land Warrior `.3di` model format — reverse-engineering spec

Status: **v10 container + per-LOD blob layout byte-exact and validated (971/971 sample files).**
Per-field record semantics inferred from IDA + the v8 cross-reference; to be finalized during the port.

Source of truth:
- **v10**: `Dflw.exe` (Delta Force: Land Warrior client), imagebase `0x400000`. IDA addresses cited inline.
- **v8** (older): [`Acruid/NovalogicTools` `File3di.cs`](https://github.com/Acruid/NovalogicTools/blob/HEAD/Tools/lib-novalogic/3DI/File3di.cs)
  documents v8 exclusively (throws on non-v8). Used as the semantic cross-reference.

Samples: `C:\Users\taylor\Desktop\dflw_3di\` — 974 files: **971 are v10, 3 are v8**
(`JPAN8.3DI`, `PFB1.3DI`, `TESTPART.3DI`).

Validator: [`validate_lw_3di.py`](validate_lw_3di.py) — reproduces every v10 file's byte layout exactly.

---

## 1. Versions

The 4-byte signature is `'3' 'D' 'I' <version>`:

| Version | Magic LE | Handled by | Our source |
|---|---|---|---|
| **8** (`0x08`) | `0x08494433` | older NovaLogic engine / NovalogicTools | NovalogicTools `File3di.cs` (port directly) |
| **10** (`0x0A`) | `0x0A494433` | `Dflw.exe` (this binary) | IDA RE below |

`Dflw.exe`'s loader **rejects byte[3] < 10** (`sub_47CB60 @ 0x47cbd6`), so v8 files do not load in the
shipped LW client — they belong to an earlier build. The two versions are the same format *family*
with different struct sizes (v10 records are larger). Supporting both means one parser with a version
branch.

---

## 2. IDA function map (v10, Dflw.exe)

| Address | Suggested name | Role |
|---|---|---|
| `0x47CB60` | `LW3di_Load(model_out, filename, ctx)` | Top-level loader/validator. Magic + 160-B header + material block + per-LOD dispatch. |
| `0x47CF80` | `LW3di_LoadLod` (thunk `0x402D97`) | Reads 232-B LOD header + blob; computes sub-array pointers by accumulation. |
| `0x47E040` | `LW3di_ReadMaterial` (thunk `0x40258B`) | Reads one 80-B material record; dedups against global table; creates D3D textures. |
| `0x47CEC0` | `LW3di_FaceUvMasks` (thunk `0x402AEF`) | Precomputes pow2 log/mask UV-wrap fields into a face record. |
| `0x47C7E0` | `modelbin_IntegrityCheck` | Validates faces→material ptr, animation overrun, material-name len ≤15, pow2 texture size. Documents struct invariants. |
| `0x4F4F00` | `Pak_Open` | Opens file across up to 16 mounted **PFF** archives. |
| `0x4F50E0` | `Pak_Read(buf, n)` | Sequential read of `n` bytes from the open file. |

Global tables: materials at `unk_915E68` (80-B stride, count `dword_915E60[0]`, max 2500); LOD pool at
`unk_652E08` (232-B stride, max 600); model pool at `unk_7FBD0C` (160-B stride).

---

## 3. v10 file layout (byte-exact, validated)

```
+0x000  Header                      160 bytes (0xA0)
        u32   material_count        (read separately, see below)
        Material[material_count]    80 bytes each
        Lod[header.lod_count]       { 232-byte LodHeader ; data blob of LodHeader.blob_size bytes }
EOF     (cursor lands exactly here for all 971 v10 samples)
```

Read order in `LW3di_Load`: 160-B header → *(optional)* `pre_count`×20-B records → `u32 material_count`
→ `material_count`×80-B materials → for each LOD: `LW3di_LoadLod`.

### 3.1 Header (160 bytes, `0xA0`)

| Off | Type | Field | Notes |
|---|---|---|---|
| `0x00` | char[3] | magic | `"3DI"` |
| `0x03` | u8 | version | `0x0A` (10) |
| `0x04` | char[~16] | name | null-terminated model name |
| `0x14` | u32 | lod_count | ≤ 4 (`lpBuffer[5]`) |
| `0x18` | u32 | lod_dist_high | Q16.16 distance threshold |
| `0x1C` | u32 | lod_dist_med | Q16.16 |
| `0x20` | u32 | lod_dist_low | Q16.16 |
| `0x28` | char[4] × lod_count | render_tags | per-LOD 4-char render type (e.g. `"crng"`, `"npwe"`, `"0gro"`, `"oleh"`) |
| `0x78` | u32 | pre_count | count of 20-byte records that follow the header. **0 in all 971 samples**; loader allocs `20*pre_count` (`0x47cbf2`). Purpose TBD. |
| `~0x58+` | — | **STALE POINTERS** | The header is an in-memory struct dumped to disk; from ~`0x58` onward most dwords are runtime pointers (`0x0012xxxx` stack, `0x76b3xxxx` DLL). The loader **overwrites** them (`lpBuffer[31]`, `[20..23]`, `word[56/57]`, `[26]`, `[29]`). **Ignore on read; zero on write.** |

> `0x18/0x1C/0x20` are the direct ancestor of GP's `lod_thresholds[3]` and `0x28` of GP's `model_tag`
> (`libs/threedi/include/threedi/threedi_gp.h`). The 4th distance slot (`DistTiny` in v8) is unused at v10.

### 3.2 Material record (80 bytes, `0x50`) — `LW3di_ReadMaterial @ 0x47e073`

| Off | Type | Field | Notes |
|---|---|---|---|
| `0x00` | char[16] | tex_name_0 | primary texture (`.PCX`/`.TGA`), name ≤15 chars |
| `0x10` | char[16] | tex_name_1 | secondary texture / second name |
| `0x24` | u32 | group_id | compared during dedup |
| `0x28` | u16 | selector_id | compared against surface selector bytes at `surface+0x34+selector_slot` for variant materials |
| `0x2A` | u16 | flags | `0x08`=force-load, `0x100`/`0x200`=has tex0/tex1 (alpha modes), `0x600`/`0x108` matched in dedup, `0x80`=duplicate-of-existing |
| `0x2C` | u16 | tex_width | clamped ≤256; power of two (integrity-checked) |
| `0x2E` | u16 | tex_height | clamped ≤256; power of two |
| `0x30`..`0x40` | u32×5 | runtime D3D handles | zeroed on read; filled by `dword_63EF60` (texture create). **Ignore on read.** |

### 3.3 LOD: header (232 bytes, `0xE8`) + data blob

`LW3di_LoadLod` reads the 232-B header (`0x47cfb6`), then a single blob of `blob_size` bytes
(`0x47cff8`), then fixes up pointers. Confirmed header fields (dword index = byte offset / 4):

| dword idx | Off | Field |
|---|---|---|
| `[5]` | `0x14` | **blob_size** (bytes of the data blob that follows) |
| `[32]` | `0x80` | n_vertices |
| `[34]` | `0x88` | n_normals |
| `[36]` | `0x90` | n_facerefs (80-B records) |
| `[38]` | `0x98` | n_array_12 (12-B records) |
| `[40]` | `0xA0` | n_subobjects (120-B records) |
| `[42]` | `0xA8` | n_triindex (12-B = 3×u32) |
| `[44]` | `0xB0` | n_surfaces (128-B named records) |
| `[46]` | `0xB8` | n_array_8b (8-B records) |
| `[48]` | `0xC0` | n_array_80b (80-B records) |

Odd indices in between (`[33] [35] [37] …`) are runtime pointer slots filled during fixup.

### 3.4 LOD data blob — 9 arrays, concatenated in this order (byte-exact, validated)

From the pointer-accumulation in `LW3di_LoadLod` (`0x47d0c6`–`0x47d158`). **For all 971 samples,
`blob_size == Σ(count × stride)` for every LOD** — zero leftover:

| # | count field | stride | content (inferred) |
|---|---|---|---|
| 1 | `[32]` | 8 | **vertices** — `int16 x,y,z,w` (v8 confirms 4×int16; v10 vertex math at `0x47d4b5` touches +0/+2/+4 as `>>8` fixed-point) |
| 2 | `[34]` | 8 | **normals** — `int16 ×4` (matches v8 normal layout) |
| 3 | `[36]` | 80 | face-material slot records (ref a surface by index at +76; `<<7` ×128 fixup at `0x47d30a`) |
| 4 | `[38]` | 12 | array_12 (collision planes? — v8 has `nColPlanes`) |
| 5 | `[40]` | 120 | **sub-objects / mesh groups** — hierarchy parent at +44, fixed-point translation at +60/+64/+68 (`>>8`), owns vertex/face/index sub-ranges via count+ptr pairs at +4/+8, +12/+16, +20/+24, +28/+32, +36/+40 |
| 6 | `[42]` | 12 | triangle index triplets (3×u32) |
| 7 | `[46]` | 8 | array_8b |
| 8 | `[48]` | 80 | array_80b |
| 9 | `[44]` | 128 | **named surfaces** — name[16] (≤15) at +0, flags at +16 (`0x4000`,`0x1`,`0x8000`,`0x800`,`0x200`), material index u16 at +24, anim-frame byte at +30, selector bytes at +0x34..+0x37, material ptr at +68 (the "face->mat ptr" the integrity check validates) |

Fixed-point: geometry uses `>>8` shifts (24.8) and a `× 0.62` factor (`0x47d37c`) on a per-surface
int16 — document the exact unit/orientation when porting (cross-check by rendering).

---

## 4. v8 layout (from NovalogicTools `File3di.cs`) and v8↔v10 correspondence

NovalogicTools reads v8 sequentially: header → vertices → normals → faces → sub-objects → bone
animations → collision → materials. Struct sizes:

| Struct | v8 size | v10 size | Notes |
|---|---|---|---|
| Header | 128 | 160 | v8 has `name[12]`, `HeaderLodInfo`(20: Count + 4 dists + 4 render-type enums), `TextureCount` |
| Texture header | 52 (`ModelTexHeader`: name[28], bmSize, index, flags, w, h, 3 ptrs) | — | v10 folds texture+material into one 80-B record |
| Material | 120 (`0x78`) | 80 | |
| LOD header | 192 | 232 | v8 fields: `nVertices, nNormals, nFaces, nSubObjects, nPartAnims, nMaterials, nColPlanes, nColVolumes`, bbox `x/y/z min/max` |
| Vertex | 8 (`int16 ×4`) | 8 | **same** |
| Normal | 8 (`int16 ×4`) | 8 | **same** |
| Face | 72 | 80 | v8 face = vtx idx, normal idx, `tu1-3/tv1-3` UVs, material idx |
| Sub-object | 112 | 120 | |

The v8 `ModelLodHeader` field names map directly onto the v10 LOD-header counts in §3.3, which is how
the v10 blob arrays were labelled. v8 is the cleaner reference for *semantics*; v10 for the bytes we
actually need.

---

## 4b. Animation — `ANM`/`KSA`/`ACA`/`SAF1` sidecars

LW does **not** embed skeletal animation in the `.3di` and uses **no `.bad` files**. Animations are
separate **`.SAF`** files (magic `"SAF1"` = `0x31464153`), loaded on demand from PFF by
`sub_44A0B0 @ 0x44a0b0` (reached via the `slot <n> <file>` console command at `0x44ab30`). The model's
sub-objects (the 120-B records in the LOD blob, §3.4 #5 — parent at +44, rest translation at +60/64/68)
are the skeleton; the SAF part-keyframes drive their rotation. **53/53 sample `.SAF` files validate as
plain (unencrypted) `SAF1`.** Validator: [`validate_saf.py`](validate_saf.py).

```
+0x00  char  magic[4]      "SAF1"
+0x04  u32   a             0x64 in samples (fps? subtype)
+0x08  u32   frame_count
+0x0C  u32   root_block    0x34 (52) — per-frame root block size
per frame:
  root block (52 B): u32 n_parts (<=15) ; u32 ; floats[...]  (loader uses dword[5,6,7,8,9,10] as
     translation x/y/z *85.333 -> i16 and dword[2,3,4] as rotation *1365.333 -> i16 BAM; pitch clamp >= -30)
  n_parts x 4 B part keyframes: u8 part_id(+0x80 bias) ; i16 angle
```

On-disk per-frame size = `root_block + 4*n_parts`; cursor lands exactly on EOF for all samples
(e.g. `3CROU01A.SAF`: 39 frames × (52 + 15×4) + 16 = 4384 B). Animations are named by gait + equipped
weapon (string table: `crwalk0_knife`..`crwalk7_knife`, `crwalk0_2hd`..; file names like `3CROU01A.SAF`,
`3SHOOT01.SAF`). Separate texture/UV **flipbook** animation also exists (128-B surface records with flag
`0x4000` spanning `surface[+30]` following surface entries — `modelbin_IntegrityCheck` "Animation overrun").

**Port implication:** SAF1 is its own format. The `.3di` parser handles geometry + skeleton + materials;
a separate `threedi_saf` (or `libs/anim`) loader handles `.SAF`, bound to a model by sub-object/part index.

### Current importer support

LW animation is now represented as sidecar data rather than as geometry IR:

- `pyopennova.lw_animation` parses decoded `ANM`, `ACA`, `SAF1`, and `KSA` files.
- `build_animation_context()` still returns BAD/ADM contexts for older games, but returns
  `LwAnimationContext` when an ANM exists and ADM/BAD does not.
- `AssetResolver` decodes loose or PFF LW text assets before the parsers see them.
- Blender imports LW skinned meshes with the synthetic BN## armature already derived from IR parts,
  binds vertex weights through the existing primitive bone table, and records resolved LW animation
  inventory on the armature.

The sidecar graph observed in loose DFLW is:

```
ITEMS.DEF chr_file player01 + anim_def playanim
  -> PLAYANIM.ANM: movement slot velocity [override]
  -> PLAYER01.KSA: 100-byte header, 255 slot records, 15,972 baked runtime frames
  -> PLAYER01.ACA: slot <slot_id> <saf_file> [loop_frame]
  -> 53 *.SAF files overriding specific slots
```

`KSA` records are 28 bytes:

```
u32 frame_count
u32 runtime_pointer    original game memory address, ignored by importers
u32 slot_id
u32 loop_frame
u32 unknown[3]         zero in current samples
```

`SAF1` loader normalization from `sub_44A0B0` writes each clip frame to the same 88-byte runtime shape
as KSA. Runtime bytes `+0x00..+0x3B` are 15 `u8 token ; i16 angle ; u8 pad` records, and bytes
`+0x3C..+0x4D` are nine i16 root values derived from root floats `[5,8,6,9,7,10,3,2,4]` with the
observed `85.333336`/`1365.3334` scalars and the `root[7] > -30 -> -30` clamp.

SAF/KSA transform playback is intentionally not applied yet. The exact game consumer still needs RE;
applying guessed token/axis mappings corrupts the rest pose. The imported armature records
`lw_animation_status=parsed_not_applied_pending_re` plus clip/slot counts for diagnostics.

---

## 4c. v10 face (`[36]`) + surface (`[44]`) layout — VALIDATED

The 80-byte `face80` record is a **triangle** (first 40 bytes follow the v8 `ModelFace`). Fields
empirically validated across **578,424 triangles in 1942 LODs** (`libs/threedi/src/threedi_lw.cpp`,
`tests/threedi/lw_parse_test.cpp`):

| off | type | field | validation |
|---|---|---|---|
| +0x04..+0x18 | int32×6 | `tu1..tu3, tv1..tv3` UVs | (from v8 correspondence) |
| +0x1C/+0x1E/+0x20 | int16×3 | `vertex[3]` | **0 out-of-range vs vertex_count** |
| +0x22/+0x24/+0x26 | int16×3 | `normal[3]` | **0 out-of-range vs normal_count** |
| +0x4C | int32 | `surface_index` | **0 out-of-range vs surface_count** |

Material is reached **indirectly**: `face.surface_index` → 128-byte surface record, but the on-disk
`material_index` (u16 @ +0x18) is only a fallback for LW v10. IDA audit of `LW3di_LoadLod`
(`sub_47CF80`) and sample data show the engine links variant surfaces by comparing
`surface[0x34 + dword_5B752C]` to the material selector id at `material+0x28`. `dword_5B752C`
is selected from the BMS model header by `sub_47E5C0`; standalone object preview uses slot 0.
The converter therefore resolves flag `0x1`/`0x8000` surfaces by selector slot 0 first, then by
case-insensitive surface name against material texture names, then by the +0x18 material index.
Surface name is at +0x00, flags at +0x10, flipbook frame span (flag `0x4000`) at +0x1E. Surfaces
named `DONTDRAW.PCX`, `DONTDRAW.TGA`, or `NoName` are hidden and should not emit render geometry.

LOD flag bit 0 marks the LW sub-objects as a skeleton, not as rigid localized mesh chunks. The 120-byte
sub-object positions are rest pivots, vertices remain in model/rest space, and vertex `w` is the skeleton
sub-object index for the vertex influence. `BADGUY.3DI` validates this: LOD0 has faces whose vertices
carry different `w` values than the owning face range, so rigid per-subobject rendering stretches or
"blows out" the model. The converter preserves model-space positions and emits IR primitive bone tables
plus one-weight vertex skinning for flag-1 LODs.

## 5. Missing pieces and RE targets

Highest-priority RE target is the runtime animation consumer: find the function that consumes the
88-byte SAF/KSA frames and writes final sub-object matrices. That should prove token-to-bone mapping
after the loader's `+0x80` bias, the token byte's axis/part semantics, the angle unit, interpolation,
loop-frame behavior, root-motion fields, and whether ANM velocity affects sampling or only gameplay.

Material parity is next: label `sub_47C130` render-state bits, surface mode byte `+112`, material flags
`0x08/0x80/0x100/0x200/0x600/0x108`, secondary texture bits (`0x1000`), and surface flipbook flag
`0x4000`. Only proven alpha/blend/two-sided/filter/wrap behavior should move into IR/material
descriptors; everything else should stay diagnostic.

1. The 12-B `[38]` and 8-B `[46]`/80-B `[48]` blob arrays (likely collision: v8 `nColPlanes`/
   `nColVolumes`). Geometry/render path does not need them; decode when adding collision.
2. UV/render-state semantics: `LW3di_FaceUvMasks` (`sub_47CEC0`) only precomputes texture pow2
   log/mask fields into the surface/face runtime data; face UVs remain fixed-point `tu/tv / 65536`
   in the converter. Surface render-state flags from `sub_47C130` still need semantic labels.
3. SAF1 ↔ model binding: `part_id` values map to the same sub-object indices used by vertex `w`, but
   the exact float field order in the 52-B root block (translation vs rotation indices) still needs
   validation against animation playback.
4. Coordinate units/orientation beyond geometry preview: LW geometry is raw Z-up and +X-forward.
   The converter maps raw `x/y/z` to IR `-y/z/x`, which the Godot adapter's IR-X mirror presents as
   Godot `y/z/x` (+Y up, +Z forward). LW triangle corners are emitted as `0,2,1` so the final Godot
   surface winding is front-facing. The `>>8`, `x0.62`, and SAF `*85.333`/`*1365.333` scales still
   need final semantic labels.
5. Purpose of the 20-byte `pre_count` block (0 in all samples here; find a sample where it's non-zero).
6. v8: validate NovalogicTools' layout against the 3 local v8 files before trusting it.

---

## 6. Eventual integration (IR-layer; mirrors GP trio)

- `libs/threedi/include/threedi/threedi_lw.h` + `src/threedi_lw.cpp`: `ThreediLwFile`, `threedi_lw_detect`,
  `threedi_lw_parse`/`_read`, with a **version branch (v8 / v10)**.
- `libs/threedi/src/threedi_ir_from_lw.cpp`: `threedi_ir_from_lw` → `ThreediModelIR`.
- Register in `threedi_ir_read` (`libs/threedi/src/threedi_ir.cpp:90`) **after** the `"3DI3"` check
  (since `"3DI3"` also has `magic[0..2]=="3DI"`): detect `magic[0..2]=="3DI" && magic[3] in {8,10}`.
- Add to `libs/threedi/CMakeLists.txt`; tests + `.3di` fixtures under `tests/`.
