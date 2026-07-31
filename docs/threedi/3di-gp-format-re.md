# 3DI / GP binary format — RE record

Consolidated 2026-06-10 from `notes/wcrate5-chunk-inventory.md`, `notes/subobject-naming.md`,
and the twenty `notes/gp-corpus-probe-*.md` probe records.

Two related on-disk model formats:

- **3DI3** — the chunked tooling/export format (`'3DI3'` magic, version `0x103`)
  written and read by the NovaLogic mod tools. Addresses in §1 and §3 are
  `ModSuperOed.exe` (OED, 32-bit PE, imagebase `0x400000`,
  IDB `ModSuperOed.exe.i64`).
- **GP** (GPM/GPS/GPP) — the BHD-era runtime `.3di` format. Addresses in §2 are
  `dfvas.exe` (Delta Force: Black Hawk Down affiliate build). One early note
  attributed `0x50bd0b` to `jodemo.exe`; every later probe cites the same range in
  dfvas, and it is treated as dfvas here.

Reimplementation: `libs/threedi` (`threedi_gp.h` with `threedi_gp_read.cpp` +
`threedi_gp_write.cpp` for GP; the Threedi3di3 reader/writer for 3DI3; and
`threedi_panm_runtime.cpp` for live PANM sampling). Probe harnesses live in
`tests/threedi/_dump_gp_*.cpp` (non-ctest executables) plus
`scripts/probe_gp_field.py`.

---

## 1. 3DI3 wire format

### Chunk walker rules

File = `'3DI3'` magic (4 B) + `version` u32 + a single implicit ROOT chunk.
Each chunk: `{ id[4], size[4], payload[size] }`. Little-endian throughout.

- `id` is 4-byte printable ASCII; in the disassembly the constants appear
  byte-reversed (`'TOOR'` = on-disk bytes `54 4F 4F 52` = ASCII `ROOT`). The
  chunk-ID string table is contiguous in OED at `0x58D1D4..0x58D2B4`.
- `size` (u32): bit 31 (`0x80000000`) = `PARENT_FLAG` (payload is a sequence of
  nested chunks); bits 0..23 (`0x00FFFFFF`) = `LENGTH_MASK` (payload byte count,
  excluding the 8-byte chunk header).
- On-disk ROOT lives at file offset `0x08`; its payload starts at `0x10`.

OED walker/IO surface:

| Concern | OED function |
|---|---|
| Parse on-disk tree | `ThreediWriter_LoadChunkTreeFromFile` (callers `[orig: Load3DI @ 0x46FE10]`, `[orig: ReExport3DI @ 0x45E130]`) |
| Find named child | `ThreediChunk_FindChild(writer, parent, fourcc)`; `parent == nullptr` = ROOT scope |
| Sibling walk (RLOD under RDTA) | `ThreediChunk_FindNextSibling(node, fourcc)` |
| Allocate output chunk | `ThreediChunk_Begin(writer, parent, fourcc)` (used by `[orig: Export3DI @ 0x45DA20]`) |
| Stamp magic+version | `[orig: Write3DIHeader @ 0x451B00]` (`'3ID3'`, `0x103`) |
| Serialize tree | `[orig: Write3DIToDisk @ 0x451D40]` |

### Wcrate5 chunk inventory

Fixture: `fixtures/wcrate5/Wcrate5.3di` (3636 bytes, 3DI3 v0x0103). Sizes are
payload bytes (exclusive of the 8-byte header).

| Chunk path | Payload B | Parent? | OED reader | OED writer | Layout / notes |
|---|---|---|---|---|---|
| ROOT | 3620 | yes | `ThreediWriter_LoadChunkTreeFromFile` | `[orig: Export3DI @ 0x45DA20]` | Implicit top-level container. |
| ROOT/INFO | 0 | no | `Load3DI` lookup at `0x46FFC5` | begun at `0x45DB1F`, never populated | Always empty in fixtures; no standalone `WriteINFO` found. |
| ROOT/GHDR | 28 | no | `Load3DI` inline from `0x4700B7` | `[orig: WriteGHDR @ 0x452B40]` | `name[16]`, `mesh_type` u32 (1=basic static, 2=skinned), `lod_count` u32, `max_radius_fp16` u32. |
| ROOT/USRP | 8 | no | `Load3DI` inline at `0x47028D` | `[orig: WriteUSRP @ 0x452C10]` | `{count u32, record_size u32=48, records[]}`. 48 B record = x,y,z + rx,ry,rz (fp16.16), subobj_idx u32, type u8, name[16]. count=0 here. |
| ROOT/CTRL | 8 | no | `[orig: sub_473710 @ 0x473710]` (CTRL reader; called at `0x470468`) | `[orig: WriteCTRL @ 0x452E90]` | `{count u32, record_size u32=24, records[]}`. Disk = 24 B name records; runtime expands to 52 B (render_mode resolved via `FindRenderModeByName`). count=0 here. |
| ROOT/MTRL | 592 | no | `Load3DI` -> `ConvertMaterialProperties` per record at `0x470276` | `[orig: WriteMTRL @ 0x453470]` | `{count u32, record_size u32=584, ThreediMaterial[]}`. 584 B disk record expands to 1204 B runtime (`OedMallocRaw(1204 * count)` at `0x4701F8`). 1 material here. |
| ROOT/CDTA | 1372 | yes | `[orig: LoadCollisionModelFromChunks @ 0x472330]` | `[orig: WriteCDTA @ 0x456050]` | Collision container; reader allocates a 0x88 runtime header + packed verts/normals/faces/objects/planes/transforms/volumes buffer. |
| ROOT/CDTA/CMDL | 64 | no | find at `0x472358` | via `WriteCDTA` | 16 dwords of model totals: bbox (6 f32), radii (3 f32), num_vertices, num_normals, num_faces, num_objects, num_transforms, num_planes, num_volumes. |
| ROOT/CDTA/CVRT | 104 | no | find `0x472373`, loop `0x47267B` | `[orig: WriteCVRT @ 0x454450]` | `{count, record_size, vertex_records[]}`; position as 16.16 fixed packed as 4 x i16. |
| ROOT/CDTA/CNRM | 112 | no | find `0x47238F`, loop `0x47270B` | `[orig: WriteCNRM @ 0x454600]` | 8 B runtime each: 3 x i16 16.16 normal + i16 dominant axis. |
| ROOT/CDTA/CFAC | 800 | no | find `0x4723AA`, loop `0x4727A9` | `[orig: WriteCFAC @ 0x454830]` | 44 B (11 dwords) per face: vert_idx[3] i16, normal_idx i16, plane_dist fp16.16, bbox min/max fp16.16, material_flags, poly_type u8. |
| ROOT/CDTA/BPLN | 80 | no | find `0x4723C5`, loop `0x472D0A` | `[orig: WriteBPLN @ 0x455A80]` | Per record: flags i16, normal 3 x i16 16.16, radius i32. 12 B runtime. |
| ROOT/CDTA/BVOL | 44 | no | find `0x4723E1`, loop `0x472DF3` | `[orig: WriteBVOL @ 0x455CE0]` | 36 B disk -> 40 B runtime; collidable_type remapped via switch (cases 0,1,4..C,E,10..13) from `0x472E60`. |
| ROOT/CDTA/COBJ | 96 | no | find `0x4723FC`, loop `0x4729A1` | `[orig: WriteCOBJ @ 0x454E70]` | 88 B disk -> 108 B runtime per object (pointer wiring at `0x473108..0x473272`). |
| ROOT/CDTA/CXLT | 8 | no | find `0x472417`, loop `0x472C79` | `[orig: WriteCXLT @ 0x455920]` | 12 B runtime each (3 x f32 translation). count=0 here. |
| ROOT/RDTA | 1388 | yes | `Load3DI` find at `0x470059` | begun at `0x45DBB3` | Render-data container; one RLOD child per LOD. |
| ROOT/RDTA/RLOD | 1380 | yes | `[orig: Threedi_LoadLodEntry @ 0x474920]` (loop `0x4704B0..0x47050E`) | `[orig: WriteRDTA @ 0x459000]`, skinned `[orig: WriteRDTA_Skinned @ 0x45C580]` | Contains RMDL/VERT/INDX/STRP/ROBJ/PANM. |
| .../RMDL | 12 | no | find at `0x474945` | via `WriteRDTA` | `model_type[4]`, `lod_threshold_fp16` u32, `render_object_count` u32. |
| .../VERT | 1012 | no | `[orig: LoadRenderVertexBuffer @ 0x474380]` (invoked `0x474A56`) | `[orig: WriteVertices_Basic @ 0x457AD0]` / `[orig: WriteVertices_Extended @ 0x457CE0]` / `[orig: WriteVertices_SkinnedBasic @ 0x457F20]` / `[orig: WriteVertices_SkinnedExtended @ 0x4581A0]` | `{count u32, stride u32, flags u32, vertex_records[]}`. Flag `0x14` = tangents, `0x40` = skinned; stride selects layout. |
| .../INDX | 116 | no | find `0x474977`, loop `0x474AB7` | via `WriteRDTA` | `{count u32, pad u32, u16 indices[]}`, copied verbatim. |
| .../STRP | 56 | no | find `0x47498F`; basic loop `0x474CAF`, skinned `0x474B60` | via `WriteRDTA` / `WriteRDTA_Skinned` | Disk record 12 dwords static / 17 dwords skinned; runtime 36 B / 48 B. material_idx, index_offset, num_indices, num_triangles, is_strip, start_vertex, num_vertices, bbox min/max (static) or bone_table (skinned). |
| .../ROBJ | 60 | no | find `0x4749A8`, loop `0x474EE0` | via `WriteRDTA` / `WriteRDTA_Skinned` | 52 B disk -> 64 B runtime `ThreediRuntimeSubobject`: opaque/alpha strip counts, parent_index, rel[3], abs[3], bounding center+radius. |
| .../PANM | 76 | no | find `0x4749C1`, decode `0x475175..0x4757ED` | `[orig: WritePANMChunk @ 0x458430]` | `{count, record_size=68, records[]}`. Per-LOD part animation; rotation modes 2/3/4, translation 1/2/3, scale 1/2 decoded into runtime flags `0x5/0x101/0x201/0x11/0x21/0x401/0x801/0x1001`. |
| ROOT/OCCL | 64 | yes | `[orig: LoadOcclusionModelFromChunks @ 0x473E60]` | `[orig: WriteOCCL @ 0x456460]` | Occlusion container (runtime: objects 60 B, verts 12 B, planes 16 B, faces 12 B). |
| ROOT/OCCL/OVRT | 8 | no | find `0x473E82`, loop `0x473FF2` | via `WriteOCCL` | 12 B runtime (3 x f32). count=0 here. |
| ROOT/OCCL/OPLN | 8 | no | find `0x473E9D`, loop `0x47405C` | via `WriteOCCL` | 16 B runtime (3 x f32 normal + f32 radius). count=0. |
| ROOT/OCCL/OFAC | 8 | no | find `0x473EB9`, loop `0x4740E0` | via `WriteOCCL` | 12 B runtime (raw_indices, edge_data, other_edge_data). count=0. |
| ROOT/OCCL/OOBJ | 8 | no | find `0x473ED4`, loop `0x4741BF` | via `WriteOCCL` | 60 B runtime per object (type/parent/connecting bytes, position[3], radius, indices, counts, ptr slots). count=0. |
| ROOT/LGHT | 8 | no | `[orig: LoadLightsFromChunk @ 0x473940]` (invoked `0x470664`) | `[orig: WriteLGHT @ 0x456DF0]` | `{count, record_size, light_records[]}`. 116 B disk (36 B legacy) -> 120 B runtime. count=0 here. |
| ROOT/MTRX | 72 | no | `[orig: Threedi_LoadMtrxChunk @ 0x473850]` (invoked `0x47047C`) | `[orig: WriteMTRX @ 0x452FA0]` | `{count, record_size=64, Matrix4x4f[]}` row-major; 1 matrix here. |

Top-level read order in `Load3DI` (`0x46FFB2..0x4700A3`):
INFO, GHDR, USRP, CTRL, MTRL, CDTA, RDTA, OCCL, LGHT, MTRX.
`Export3DI` creation order (`0x45DF43..0x45E03E`):
GHDR, USRP, CTRL, MTRX, MTRL, CDTA, per-LOD RDTA, OCCL, LGHT.
Write order is creation order, which is **not** the on-disk read order.

### Derived chunks (not direct `.ase`/`.3dp` copies)

- **GHDR.max_radius_fp16** `[orig: WriteGHDR @ 0x452B40]`: max of the derived
  per-LOD bounding radius (`lod.maxRadius`, computed from LOD geometry) across all
  LODs, times 65536, truncated to u32. Not a `.3dp` field; an encoder can
  recompute it from rebuilt geometry.
- **MTRX** `[orig: ComputeMTRX @ 0x452990]` + `[orig: WriteMTRX @ 0x452FA0]`:
  derived from per-subobject center-point axes (3x3 frames). Per subobject:
  dedup against `g_Matrices` via `GetPartAnimAxisIndex` (dot >= 0.999), rearrange
  axis components into the engine convention (-y, z, x with sign flips), then
  `Matrix4x3_Invert`; store (max 64 entries). Because of the float-heavy
  transform + inversion, byte-exact MTRX output requires OED's x87 precision
  (below); a 64-bit SSE2 build diverges in low FP bits.

### OED x87 control word (parity-build requirement)

OED's legacy MSVC CRT startup (`start @ 0x525610` -> `__cinit @ 0x527BDE` ->
`_fpmath @ 0x523530` -> `[orig: _setdefaultprecision @ 0x52A27C]`) calls
`_controlfp(0x00010000, 0x00030000)` then `fnclex`:

- Precision: **_PC_24** (24-bit single-precision x87 mantissa) — a deliberate
  downgrade from the MSVC default `_PC_53`.
- Rounding: `_RC_NEAR` (CRT default, unmodified). Exceptions: all masked.
- Scope: everything after `__cinit` (`Export3DI`, `Load3DI`, collision baking,
  MTRX, USRP fp16.16 conversions, STRP bbox-midpoint encoding at
  `0x474D85..0x474E1F`).

A byte-exact OED-parity sub-build must call `_controlfp(_PC_24, _MCW_PC)` early
in `main()`. The only other `_controlfp` sites in OED are local save/restore
wrappers (D3DX shader preprocessor and a few math helpers) that do not change the
process-wide state.

### Retail PANM control-register sampling (Jointops.exe)

This subsection is the live runtime complement to the OED format findings
above. Its addresses are from retail `Jointops.exe` (imagebase `0x400000`, IDB
`Jointops.exe.kong.i64`), freshly witnessed and completed by the
catalog/loader/writer audit on 2026-07-29.

| Component | Verdict | Evidence |
|---|---|---|
| Global CTRL catalog (`libs/threedi/src/threedi_ctrl_catalog.cpp`) | **MATCHING** | all 96 populated 32-byte descriptors at `0x83DCE8..0x83E8E7` are preserved in ordinal order; the next descriptor at `0x83E8E8` is zero and terminates the scan, so 96 is the complete retail list. Case-insensitive lookup is pinned by `threedi_ctrl_catalog` `[orig: CtrlName_ToOrdinal @ 0x57B290]` |
| Loader-compatible name resolution | **MATCHING** | ordinary lookup reports a miss unambiguously, while `threedi_ctrl_register_loader_ordinal` deliberately reproduces retail's miss → ordinal-0 alias `[orig: sub_5B4640 @ 0x5B4640; ordinal store @ 0x5B46E6]` |
| Structural loader remap | witnessed / represented | model-local CTRL references in material/texture animation, PANM, and lights become global ordinals during `ThreediGp_LoadFromFile` `[orig: @ 0x5B5C80..0x5B5DA2; @ 0x5B5E0B..0x5B5EF6; @ 0x5B5F4F..0x5B5F62]` |
| PANM scalar-track sampler (`libs/threedi/src/threedi_panm_runtime.cpp`) | **MATCHING MATH / PARTIAL RNG LIFETIME** | `threedi_panm_sample_track_raw` structurally translates `[orig: PANM_SampleTrack @ 0x5b2270]`; controlled and deterministic waveform paths are pinned, and noise dispatch consumes per submitted instance, but its LCG is not yet retail's whole-process CRT stream |
| Controlled PANM mode catalog | **MATCHING** | only type 113 reads a control register; 114–117 remain ordinary waveform types `[orig: PANM_SampleTrack @ 0x5b2270; wave_lookup @ 0x5de6b0]`; all known shipped controlled PANM tracks are type 113 |
| Runtime slot layout and consumer math | **MATCHING** | each global slot is an 8-byte pair: signed value dword at `0x83FCE8 + 8·ordinal`, adjacent state dword at `0x83FCEC + 8·ordinal`; PANM reads the signed value with retail low-dword `IMUL`/arithmetic-shift behavior |
| Process-global bus lifetime/arbitration | **OPEN** | retail keeps one persistent 96-slot array shared by every model draw; the retained OpenNova path currently constructs a zero-based array from each model's current Dictionary, so unwritten values do not flow across models in retail draw order. Retail's later batch snapshot/restore preserves written material values but does not erase that submission-time persistence requirement (D-3DI-2) |
| Noise RNG lifetime/call order | **OPEN** | retail `CWaveformTable_Build @0x5DE360` consumes 256 calls from the same process CRT `rand()` later used by PANM/material/light noise, interleaved with unrelated engine callers. OpenNova shares one MSVC-formula stream only among the ported waveform consumers and uses a precomputed table, so dispatch/math and intra-consumer order match but the runtime sample sequence does not (D-3DI-2) |
| Retail CTRL producers | **PARTIAL PORT** | the complete dedicated-writer census is recorded below; the catalog, remap semantics, and consumers are implemented, but several retail semantic producers remain separate port gaps |

#### Canonical global catalog

`CtrlName_ToOrdinal` scans this table case-insensitively. Its return value is
zero both for the real `LOD_FRAC` entry and for an unknown name. Retail's model
loader stores that result unchanged, so an unknown or empty inline model CTRL
name aliases global ordinal 0. OpenNova additionally treats a null API input as
ordinal 0 for safety; retail's loader passes a non-null inline name buffer.
OpenNova keeps an unambiguous ordinary lookup
(`-1` for a miss) and isolates the retail alias in
`threedi_ctrl_register_loader_ordinal`.

| Ordinal | Retail name | Ordinal | Retail name |
|---:|---|---:|---|
| 0 | `LOD_FRAC` | 48 | `HELO_PILOTYAW` |
| 1 | `LOD_FADE_IN` | 49 | `HELO_PILOTPITCH` |
| 2 | `LOD_FADE_OUT` | 50 | `HELO_CPILOTYAW` |
| 3 | `FLICKER` | 51 | `HELO_CPILOTPITCH` |
| 4 | `SWING` | 52 | `HELO_GUNYAW` |
| 5 | `TALK` | 53 | `HELO_GUNPITCH` |
| 6 | `DEATH` | 54 | `HEAT_GLOW` |
| 7 | `NVG_FLIP` | 55 | `EWEAP_GUNYAW` |
| 8 | `TEAMSWING` | 56 | `EWEAP_GUNPITCH` |
| 9 | `HUD_HEALTH` | 57 | `WEAP_SPIN` |
| 10 | `HUD_MANA` | 58 | `TRACER_SCALE` |
| 11 | `HUD_COMPASS` | 59 | `TRACER_WIDTH` |
| 12 | `WPN_TRIGGER` | 60 | `VEHICLE_WHEELS` |
| 13 | `WPN_HAMMER` | 61 | `VEHICLE_STEERING` |
| 14 | `PARA` | 62 | `VEHICLE_SPEED` |
| 15 | `PARA_O` | 63 | `VEHICLE_GUNYAW` |
| 16 | `DOOR_00` | 64 | `VEHICLE_GUNPITCH` |
| 17 | `DOOR_01` | 65 | `OBJECT_DESTROY` |
| 18 | `DOOR_02` | 66 | `OBJECT_DESTROY01` |
| 19 | `DOOR_03` | 67 | `OBJECT_DESTROY02` |
| 20 | `DOOR_04` | 68 | `OBJECT_DESTROY03` |
| 21 | `DOOR_05` | 69 | `OBJECT_DESTROY04` |
| 22 | `DOOR_06` | 70 | `OBJECT_DESTROY05` |
| 23 | `DOOR_07` | 71 | `VEHICLE_SPECIAL1` |
| 24 | `DOOR_08` | 72 | `VEHICLE_SPECIAL2` |
| 25 | `DOOR_09` | 73 | `VEHICLE_TIRE00` |
| 26 | `DOOR_10` | 74 | `VEHICLE_TIRE01` |
| 27 | `DOOR_11` | 75 | `VEHICLE_TIRE02` |
| 28 | `DOOR_12` | 76 | `VEHICLE_TIRE03` |
| 29 | `DOOR_13` | 77 | `VEHICLE_TIRE04` |
| 30 | `DOOR_14` | 78 | `VEHICLE_TIRE05` |
| 31 | `DOOR_15` | 79 | `VEHICLE_TIRE06` |
| 32 | `UPL_INTENSITY` | 80 | `VEHICLE_TIRE07` |
| 33 | `LIGHTSWITCH0` | 81 | `VEHICLE_TIRE08` |
| 34 | `LIGHTSWITCH1` | 82 | `VEHICLE_TIRE09` |
| 35 | `LIGHTSWITCH2` | 83 | `VEHICLE_TIRE10` |
| 36 | `LIGHTSWITCH3` | 84 | `VEHICLE_TIRE11` |
| 37 | `PARTICLE_ALPHA` | 85 | `VEHICLE_TIRE12` |
| 38 | `PARTICLE_RGB` | 86 | `VEHICLE_TIRE13` |
| 39 | `HELO_REAR_GEAR` | 87 | `VEHICLE_WHEELS00` |
| 40 | `HELO_GEARDOORS` | 88 | `VEHICLE_WHEELS01` |
| 41 | `HELO_GEAR` | 89 | `VEHICLE_WHEELS02` |
| 42 | `HELO_GEARB` | 90 | `VEHICLE_WHEELS03` |
| 43 | `HELO_BAYDOORS` | 91 | `LFP_CAMPPERCENT` |
| 44 | `HELO_PCANOPY` | 92 | `TEX_TEAM` |
| 45 | `HELO_CPCANOPY` | 93 | `TEX_CAMO1` |
| 46 | `HELO_ROTOR` | 94 | `TEX_CAMO2` |
| 47 | `HELO_TAILROTOR` | 95 | `TEX_CAMO3` |

#### Loader remap and runtime bus

The CTRL chunk is a model-local list of authored names, but its list order is
not the runtime bus layout. `[orig: sub_5B4640 @ 0x5B4640]` resolves each name
through the global catalog and stores the global ordinal in the runtime CTRL
record at `0x5B46E6`. `ThreediGp_LoadFromFile` then follows each file-local
reference through that record and patches the consuming field to the global
ordinal: material/texture-animation references at
`0x5B5C80..0x5B5DA2`, PANM references at `0x5B5E0B..0x5B5EF6`, and light
references at `0x5B5F4F..0x5B5F62`. Consequently B50Cal's local order
`[HEAT_GLOW, EWEAP_GUNYAW, EWEAP_GUNPITCH]` never makes yaw global ordinal 1;
the patched values are 54, 55, and 56. Unknown authored names follow the same
path and become ordinal 0.

The global bus has 96 pairs. The even dword at
`0x83FCE8 + 8·ordinal` is a **signed `int32` value**, not a clamped `uint16`;
`0x10000` is the exact 16.16 endpoint and negative values remain meaningful.
The odd dword at `0x83FCEC + 8·ordinal` is adjacent state. PANM ignores it;
controlled texture animation reads it to choose fractional-frame versus
modulo-frame interpretation. No writer to that odd dword was found, so its
static zero selects the fractional-frame branch. Integer consumers retain
two-operand `IMUL`'s wrapping low 32 bits before an arithmetic right shift
rather than widening the product.

Sorted material rendering does not simply observe whichever writer happened to
touch the global bus last. During submission,
`[orig: collect_render_objects_for_batch @ 0x5D8F20]` walks the four register
ordinal bytes in the render-material record and copies each nonzero ordinal's
current value into the 68-byte batch entry
`[orig: snapshot @ 0x5D91AB..0x5D91DE]`. The sorted flush restores those
captured values to the global slots before calling
`apply_shader_parameters`
`[orig: CRenderBatchQueue_FlushBatches @ 0x5D9F50; restore
@ 0x5DA1B8..0x5DA1FD; shader call @ 0x5DA436]`. Ordinal zero is the byte-level
sentinel and is not copied by that loop. This explains why a retained
per-model value snapshot can reproduce a controlled material's written inputs,
but it does not reproduce the persistent bus value inherited by an unwritten
slot or the submission/flush RNG schedule.

`PANM_SampleTrack`, called while building posed matrices by
`[orig: Model_TransformBoneMatrices @ 0x58e390]`, first handles constant type
24.
Type 113 then treats the track parameter as a control-register ordinal and
interpolates the signed start/end window from the slot's even value dword.
Every other active type, including 114–117, evaluates
`wave_lookup(type, phase + time * rate)` instead. The low-nibble waveform
dispatch is shared with material animation through
`[orig: wave_lookup @ 0x5de6b0]`;
the fact that the UV-matrix consumer assigns special meanings to all five
types 113–117 does **not** extend those meanings to PANM.

The corpus agrees with that consumer-specific branch:

| Corpus | Controlled PANM tracks | Models | Type 113 | Types 114–117 |
|---|---:|---:|---:|---:|
| checked-in canonical `.3di` set | 93 (97 if the duplicate temporary fixture is included) | — | all | 0 |
| retail base install | 1,740 | 178 | all | 0 |
| retail `RevX02` expansion | 253 | 42 | all | 0 |

#### Retail writer coverage and port boundary

The complete global-bus xref audit, including literal stores and indexed range
writers, found dedicated retail writers for ordinals **3–10, 14–36, 41,
46–47, and 52–95**. It found no dedicated writer for **0–2, 11–13, 37–40,
42–45, or 48–51**. Absence from that second set does not make ordinals 1–95
unreachable: the generic ACTION `ctrlreg <NAME>` path can animate any
successfully resolved nonzero ordinal
`[orig: ActionDef_ParseScriptLine @ 0x4027FA; ActionSlot_ExecuteAction
@ 0x4020CC; CtrlRegAnimSlot_Allocate @ 0x401CA0;
CtrlRegAnimSlot_UpdateAll @ 0x401BF0]`. Ordinal 0 cannot be addressed through
that generic path because zero is also the resolver's miss sentinel.

The producer census partitions all 96 ordinals without an unclassified tail:

| Producer status | Count | Ordinals |
|---|---:|---|
| exact value/state projection hosted in its bounded semantic scope | 10 | **8, 54–56, 61–62, 71–72, 91–92** |
| dedicated retail writer exists; exact original publisher remains open | 68 | **3–7, 9–10, 14–36, 41, 46–47, 52–53, 57–60, 63–70, 73–90, 93–95** |
| no dedicated writer found; only the generic path can reach nonzero members | 18 | **0–2, 11–13, 37–40, 42–45, 48–51** |

This is a producer-status partition, not a format-support partition: every one
of the 96 names is present in the canonical catalog and can be resolved by the
loader and consumed by a model. The names for every ordinal are in the
canonical table above.

That ACTION path is not safe to approximate from the weapon FSM's phase.
`CtrlRegAnimSlot_Allocate` stores four dwords per slot: the ActionDef pointer,
repeat count, its third argument, and `ctrlreginc`; all witnessed callers pass
the live `MountSlot *` as that third argument
`[orig: ActionSlot_BeginActivePhase @ 0x53F878..0x53F87B]`. The updater then
treats the same dword as the initial numeric accumulator before clamping and
reversing it. No shipped `weapon.def` in the audited corpora authors `ctrlreg`,
so this address-dependent dormant behavior has no retail-data witness. OpenNova
therefore leaves the animator explicitly unported instead of substituting an
action counter or normalized clip phase.

The currently hosted writer-value families are:

- PLAYPARTANIM channel 1 publishes `VEHICLE_SPECIAL1` (71) only when item
  attribute `0x1000` is clear; channel 2 always publishes
  `VEHICLE_SPECIAL2` (72). The integrator uses wrapping signed-dword ADD/SUB
  and clamps only on strict upper/negative overshoot; ordinary sweeps retain
  the exact `0x10000` endpoint, while zero-time states can leave that range.
  This is a fixed semantic mapping, not a walk over model
  CTRL order `[orig: Entity_ApplyCommand case 0x22 @ 0x43B192; integrator
  @ 0x456710; HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38]`.
- `HEAT_GLOW` (54) is live: the attachment path writes it at `0x440969` and
  `0x440991` while resolving the parent carrier's PANM/bones for a live UseGun
  child, and the first-person viewmodel writes it at `0x4DEEC2..0x4DEEF5`
  `[orig: HUD_CacheWeaponSlotInfo @ 0x440930, sole caller
  Entity_AttachToBoneAndUpdateTransform @ 0x546518;
  Player_RenderFirstPersonViewModel @ 0x4DED60]`.
- `EWEAP_GUNYAW`/`EWEAP_GUNPITCH` (55/56) retain the witnessed emplaced-weapon
  angular stores in world and first-person presentation scopes.
- `VEHICLE_STEERING` (61) zero-extends the high word of the live cveh steering
  state. `VEHICLE_SPEED` (62) reproduces the `CDQ`/`XOR`/`SUB` absolute value
  and unsigned `0x10000` cap, including the `INT_MIN → 0x10000` edge. They are
  published only for authority rows with the modeled cveh state; compact
  joiner rows carry neither source and do not synthesize it
  `[orig: Entity_CacheVehicleHUDStats @ 0x4929B0; stores
  @ 0x4929D7 / @ 0x4929F1]`.
- The numbered-zone generic callback publishes `TEAMSWING` (8), signed-byte
  `TEX_TEAM` (92), and conditionally `LFP_CAMPPERCENT` (91). The latter is an
  omitted write when the shared 13-dword timer entry is absent; when present,
  it is `limit ? trunc(current/limit × 65536) : 0x10000`. OpenNova applies
  mixed S2C `0x53`/`0x6F` updates in wire order and advances the shared entry
  once after the complete client receive pump. Sector-model and first-person
  submissions also retain their separate signed `TEX_TEAM` writers
  `[orig: BoneCallback_gnrc_World @ 0x4E2860; render_sector_entity
  @ 0x5C4190; Player_RenderFirstPersonViewModel @ 0x4DED60]`.

OpenNova now has the exact catalog, lookup/loader alias, global-reference
ordinals, signed value layout, and audited per-call consumer math. The retained
runtime still lacks retail's process-global persistent bus lifetime and
cross-model last-writer ordering. This is therefore **catalog and consumer-math
fidelity**, not full bus/producer fidelity. The 10 value/state projections
listed above are landed in bounded semantic scopes, while exact frustum
submission timing remains part of the global-bus gap. The generic ACTION
animator, the other 68 dedicated-writer ordinals, unavailable compact-joiner
source fields, and the separate overheat particle emitter are bounded by
D-3DI-2/D-WPN-28 in the divergence ledger.

OED export now preserves the loader's structural rule for every style
`> 0x70`: even a raw PANM style 114 reference is collected into CTRL and its
local index is serialized before retail remaps it. `oed_export_3di_test` pins
that behavior. The current editor generator catalog still does not offer PANM
114–117 as authoring choices, so this is load/runtime/export fidelity, not a
claim of retail OED UI parity for those unused PANM styles.

---

## 2. GP runtime format — corpus probe findings

Corpus: 639 `.3di` files (GPM/GPS/GPP) from the `AS_ASSETS` BHD affiliate build
(`~/Desktop/AS_ASSETS`, `__temp.3di` excluded); 639/639 parse cleanly. The
material-struct probe (§2.3 first table) used the smaller 22-file
`fixtures/3dp/` set instead. All struct/field names refer to
`libs/threedi/include/threedi/threedi_gp.h`; fields proven always-zero and
loader-unread are named `pad_<struct>_<hexoffset>` and round-tripped verbatim.

Loaders: `[orig: dfvas load_gpm_model_0 @ 0x50b710]` (model-level sections),
`[orig: dfvas load_gpm_model @ 0x50fb30]` (collision),
`[orig: dfvas load_gpm_occdata @ 0x50fd70]` (occlusion),
`[orig: dfvas load_rmodel_resource @ 0x510650]` (per-LOD render model).
File-level GP flags witnessed in `load_gpm_model_0`: bit 0 = has light_info,
bit 1 = has vstream.

Corpus population summary:

| Population | Count |
|---|---|
| Fixtures | 639 (all parse) |
| RModels (LODs) | 1,598 |
| Material lookup entries | 4,047 |
| Batch entries | 4,115 (3,859 local / 256 global) |
| Fixtures with lights / light entries | 33 / 93 |
| Fixtures with VStream | 113 |
| Fixtures with collision | 639 |
| Collision faces / objects / volumes | 465,598 / 3,815 / 9,128 |
| Fixtures with occlusion / occlusion objects | 98 / 592 |
| Global-batch RModels / global-batch polys | 256 (68 files) / 788 |
| GPS fixtures | 1 (`Oicw_1R.3di`, 2,379 verts) |
| Fixtures with extra_polys (VP1/VP2) | 0 |

### 2.1 Batch entries

32 B per batch entry. Loader uses only `+0x00` (runtime opaque ptr, overwritten),
`+0x04` opaque_count, `+0x08` runtime transparent ptr, `+0x0C` transparent_count
`[orig: load_rmodel_resource @ 0x510650, static path 0x510821..0x5108b5]`.

| Offset | Field | Corpus |
|---|---|---|
| +0x10..+0x1C | `pad_batch_10/14/18/1C` | zero in 4,115/4,115 entries (both local and global paths) |

Verdict: true padding; loader neither reads nor writes `+0x10..+0x1C`. Parser
discards (reads for byte advancement only); writer emits 0.

### 2.2 Light section

Layout: outer header 8 B (`version` u32, `payload_size` u32); inner header 60 B
(ambient light 36 B + `light_count` u32 + runtime ptr u32 + 4 pad dwords);
entries 48 B x `light_count`. Loader reads the 8 B outer header, allocates
`payload_size`, bulk-reads, then walks only the per-light array
`[orig: load_gpm_model_0 @ 0x50bd0b..0x50bd84]`. The per-entry loop reads only
`style` (+0) and resolves `phase` (+1) via control-register lookup
`[orig: 0x50c08b..0x50c0c6]`. The light section is not necessarily last in the
file; mesh/texture blobs can follow it.

| Claim | Corpus | Verdict |
|---|---|---|
| Inner-header tail `pad_light_2C/30/34/38` zero | all fixtures (parser asserts + `gp_corpus_invariants_test`) | padding; loader stores but never tests them |
| Per-entry `pad_light_entry_24/28/2C` zero | 93/93 entries, 33 fixtures | padding; offsets unaccessed by loader |
| `payload_size == 60 + 48 * light_count` (no trailing gap) | 33/33 fixtures, gap = 0 in all (counts 1..11) | exact; the `gp_skip(remainder)` branch is a safety net only |

### 2.3 Materials

`ThreediGpMaterial` (22/22 `fixtures/3dp/` fixtures probed):

| Offset | Field | Corpus | Notes |
|---|---|---|---|
| +0x40 | `pad_disk_uvoffset_u` | zero 22/22 | df4oed runtime struct holds u_offset (UI state) here; disk is padding. `load_rmodel_resource` never touches it. |
| +0x44 | `pad_disk_uvoffset_v` | zero 22/22 | same, v_offset. |
| +0x74 | `pad_runtime_ptr` | zero 22/22 | overwritten at load with 0 or a 60 B palette/material-info entry ptr `[orig: load_rmodel_resource @ 0x510bd3 / 0x510bce]`; disk value never read. |

`ThreediGpMaterialLookup` (60 B entries; 4,047 entries, 639/639 fixtures):

| Offset | Field | Corpus | Notes |
|---|---|---|---|
| +0x10..+0x20 | `pad_lookup_10/14/18/1C/20` | zero x4,047 | unread by any traced loader path. |
| +0x24 | `seq_index` | (live field) | the only field the lookup-search loop reads for matching `[orig: load_rmodel_resource @ 0x510bd3]`. |
| +0x2C, +0x30 | `runtime_2C`, `runtime_30` | — | explicitly zeroed by the loader post-read (runtime slots). |
| +0x34, +0x38 | `pad_lookup_34/38` | zero x4,047 | inert on-disk padding; NOT zeroed by the loader (distinguishes them from the runtime slots). |

Verdict: 60 B entry = live fields + `seq_index` + 2 loader-zeroed runtime slots +
7 always-zero pad dwords.

### 2.4 RModel (LOD) header — live fields

0x88 B header per LOD, read whole by `[orig: load_rmodel_resource @ 0x510650]`;
the fields below are consumed by the LOD-selection/render path, not the loader.
1,598 RModels probed.

| Offset | Field | Values seen | Interpretation |
|---|---|---|---|
| +0x20 | `lod_dist_threshold_q16` | 0 (624 files); 3.0 / 4.0 (character models); 7.0 (flag cloth) — Q16.16, 4 distinct | view-distance cutoff for LOD switch / skinned-anim disable |
| +0x28 | `lod_anim_lod_count` | 0 or 3 (3 on 12 files: characters + mounted weapons) | number of leading LODs carrying articulated/animated geometry |
| +0x38 | `lod_secondary_dist_q16` | 0; 4.0 on the 3 flag models only | secondary threshold, likely cloth-sim activation range |
| +0x68 | `extra_xform_count` | (live field) | precedes the pad tail below |

### 2.5 RModel header pads (promoted discards)

| Offset | Field | Corpus |
|---|---|---|
| +0x24, +0x2C, +0x30, +0x34, +0x3C, +0x40, +0x44 | `pad_lod_24/2C/30/34/3C/40/44` | zero x1,598 |
| +0x48, +0x4C | `pad_lod_48/4C` | zero 639/639 files |
| +0x6C..+0x84 | `pad_lod_6C/70/74/78/7C/80/84` | zero x1,598 |

Verdict: all 16 dwords always zero; loader reads but never stores/branches on
them. Parser stores named fields; writer round-trips via `raw_header`.

### 2.6 VStream header

Present when file-level flags bit 1 is set (113/639 fixtures, predominantly
character/weapon models and skinned interiors). 16 B header = buffer_ptr
(disk-zero), `data_size` (0 means fallback `24 * rverts_count`), then two dwords
the loader copies verbatim into the runtime buffer without reading back
`[orig: load_gpm_model_0 @ 0x50b710, vstream branch]`. The stream carries
24 B/vertex tangent/binormal data.

| Offset | Field | Corpus |
|---|---|---|
| +0x08 | `pad_vstream_08` | zero 113/113 |
| +0x0C | `pad_vstream_0C` | zero 113/113 |

Verdict: preserved verbatim by dfvas (round-trips safely) but carry no data in
any known fixture.

### 2.7 Subobjects

72 B (18 dwords) per subobject. The loader's post-load loop touches only dword 0
(runtime submesh ptr, overwritten with the batch-buffer pointer, then advanced by
`32 * batch_count`) and dword 1 (`batch_count`)
`[orig: load_rmodel_resource @ 0x510833]`. In the global-batch path the runtime
allocation grows to 92 B per subobject `[orig: 0x5108e3]`.

| Offset | Field | Corpus |
|---|---|---|
| +0x00 | `pad_runtime_submesh_ptr` | zero 639/639 (overwritten unconditionally at load) |
| +0x08 | `pad_subobject_08` | zero 639/639 |

### 2.8 Global-batch metadata block

20 B (5 dwords) read **once per RModel** in the global-batch path, between
`total_batch` and the per-batch loop. (An earlier note mislabelled this as a
per-subobject block; the 92-vs-72 B subobject size in §2.7 is a separate runtime
allocation detail.)

| Offset | Field | Corpus |
|---|---|---|
| +0x00..+0x10 | `pad_global_batch_00/04/08/0C/10` | zero in 256/256 global-batch RModels (68 files) |

Verdict: runtime-pointer slots or unactivated reserved fields; no IDA witness of
a reader. Writer emits the IR fields (previously hardcoded zeros, coincidentally
identical).

### 2.9 Bone-info sub-record (global-batch polys)

Global-batch polys are allocated at stride `88 + 2 * index_count * sizeof(u16)`
`[orig: load_rmodel_resource @ 0x510aa6]`. Within the 88 B fixed prefix:
`+0x00..+0x17` poly header (material_index, indices_tag, index_count,
triangle_count, topology, first_vertex, max_vertex_index); `+0x18..+0x2F` the
24 B bone-info sub-record = `bone_table[16]` + `pad_bone_info_align[7]` +
`bone_table_length` u8; `+0x30..+0x57` remaining prefix fields.

| Bytes | Field | Corpus |
|---|---|---|
| +0x28..+0x2E (7 B gap) | `pad_bone_info_align[7]` | zero in 788/788 global-batch polys |

Verdict: structural alignment so `bone_table_length` lands at offset 23. dfvas
never accesses the gap bytes post-allocation.

### 2.10 GPS vertex `normal_w`

GPS (THREEDI_GP_MESH_STATIC) vertices use a 48 B on-disk stride:

```
+0x00  pos[3]        3 x f32
+0x0C  normal[3]     3 x f32
+0x18  normal_w      f32      <- this field
+0x1C  packed_color  u32
+0x20  uv0[2]        2 x f32
+0x28  uv1[2]        2 x f32
```

Corpus: 1 GPS file (`Oicw_1R.3di`), 2,379 verts. 13.7% exactly zero; 86.3%
non-zero floats bounded in [-1, +1]; **no** exact +/-1.0.

Verdict: not a tangent-handedness sign and not padding — consistent with the 4th
component of a 4-float normal (or a packed per-vertex scalar such as an AO term).
Tangent frames live in the VStream, so it is unrelated to tangent reconstruction.
The loader stores it to runtime vertex `+0x1C`
`[orig: load_gpm_model_0 @ 0x50ba14..0x50ba7e]`; no downstream consumer traced.
Promoted from anonymous discard to `ThreediGpRVert.normal_w` (parsed + written).

### 2.11 Collision

`[orig: load_gpm_model @ 0x50fb30]` reads a 0x88 B collision header, then
bulk-reads `data_size` (header `+0x04`) bytes and carves sub-arrays by pointer
arithmetic: `+0x08` is overwritten with the data-buffer pointer; `+0x30`
vertex_count / `+0x34` vertex_ptr; `+0x38` normal_count / `+0x3C` normal_ptr;
`+0x44` face_ptr; `+0x4C` object_ptr; the 9 trailing slots `+0x64..+0x84` are
populated with per-sub-array offsets at runtime. Per-record fields are never
decoded at load time (physics/raycast functions consume them later).

| Struct | Field(s) | Corpus | Verdict |
|---|---|---|---|
| Header (0x88 B) | `pad_collision_08` (+0x08) | zero 639/639 | runtime data-ptr slot, zero on disk |
| Header | 9 trailing slots +0x64..+0x84 | zero 639/639 | runtime sub-array ptr slots; round-tripped via `raw_header` only, parser `GP_ASSERT_ZERO` |
| Face (44 B) | `pad_face_2A` (+0x2A, i16) | zero in 465,598/465,598 faces | padding after `surface_type` u8 + pad u8 |
| Object (128 B) | `pad_object_28/2C/30` (+0x28..+0x30) | zero x3,815 | reserved |
| Object | `pad_object_70/74/78/7C` (+0x70..+0x7C) | zero x3,815 | tail position consistent with runtime-ptr slots (same pattern as header) |
| Volume (96 B) | `pad_volume_2C` (+0x2C) | zero x9,128 | reserved |
| Volume | `plane_ptr` (+0x4C) | zero on disk | runtime pointer |
| Volume | `pad_volume_50/54/58/5C` (+0x50..+0x5C) | zero x9,128 | reserved; never read back post-load |

44 B CollisionFace layout:

```
+0x00  vertex_indices[3]  u16 x 3
+0x06  normal_index       i16
+0x08  plane_d            i32
+0x0C  bbox min/max x,y,z i32 x 6
+0x24  surface_flags      i32
+0x28  surface_type       u8
+0x29  pad                u8
+0x2A  pad_face_2A        i16
```

96 B CollisionVolume layout (offsets verified by counting the parser read
sequence; an earlier plan spec was off by one field):

```
+0x00  type, flags                  i32 x 2
+0x08  min/max x,y,z                i32 x 6
+0x20  extent[3]                    i32 x 3
+0x2C  pad_volume_2C                i32
+0x30  bbox min/max x,y,z           i32 x 6
+0x48  plane_count                  i32
+0x4C  plane_ptr (runtime, disk 0)  u32
+0x50  pad_volume_50/54/58/5C       i32 x 4
```

### 2.12 Occlusion objects

60 B per object; loader computes vertex/plane/face array pointers but never
accesses per-object offsets `+0x2C..+0x3B`
`[orig: load_gpm_occdata @ 0x50fd70]`. Three runtime pointers at
`+0x18/+0x20/+0x28` precede the tail.

| Offset | Field | Corpus |
|---|---|---|
| +0x2C..+0x38 | `pad_occ_obj_*` (4 dwords) | zero in 592/592 objects (98 fixtures) |

### 2.13 VariablePoly1 reserved tail (vacuous)

`ThreediGpExtraPoly` VariablePoly1 is a 40 B sub-struct with a 16 B tail at
`+0x18..+0x24` (`vp1_pad_18/1C/20/24`). **Zero fixtures in the 639-file corpus
contain any extra_poly record** — the VP1/VP2 parse path is structurally derived
from `[orig: load_rmodel_resource @ 0x510650]` (24 B header consumed
field-by-field, 16 B tail unaccessed) but is unexercised by any known fixture.
The all-zero invariant holds vacuously. Treat this path as unvalidated until a
fixture with extra_polys surfaces.

### 2.14 Open items

- 3DI3: `WriteINFO` was never located in OED (likely nonexistent; the chunk is
  begun empty). Revisit if a fixture with a non-empty INFO surfaces.
- 3DI3: all OCCL leaves in Wcrate5 are count-zero; per-record encodings are
  unvalidated against a fixture with real occlusion geometry.
- 3DI3: `[orig: LoadRenderVertexBuffer @ 0x474380]` was catalogued but not
  decompiled; the VERT stride/flag-to-writer-variant mapping is unverified.
- GP: global-batch flag-bit discrepancy. The subobject probe (and the IDA
  witness at `0x5108e3`) keys the 92 B subobject allocation on `flags & 2`
  (85/639 files), while the Phase-4 metadata probe counts global-batch RModels
  via `flags & 1` (68 files, 256 RModels). These are likely different flag words
  (runtime vs parser RModel flags) but the bit numbering is unreconciled.
- GP: `normal_w` has no traced consumer; the runtime shader may read it from the
  vertex buffer directly.

---

## 3. Subobject naming convention (OED `.ase` lookup)

OED matches `.3dp` `partanimation subobject N` entries to `.ase` GEOMOBJECTs by
**the leading decimal integer of NODE_NAME** — not by `*NODE_PARENT`, which is
cosmetic and ignored for classification.

`[orig: ConvertToInternal @ 0x4268B3]` classifies every GEOMOBJECT in a first
pass (loop `0x42340C..0x42361D`) on the uppercased first one or two characters of
NODE_NAME:

| Leading char(s) | Classification |
|---|---|
| `_` | center marker |
| `~` | attach marker |
| `#` | stretch box |
| `!` | ignored entirely |
| digit `0`-`9` | subobject N = `atol(name)` (1-indexed); object index appended to `g_SubObjects[N-1]` (atol at `0x423580`, assignment at `0x4235A4`) |
| `C/B/L/D/O/V` + `A`-`Z` | collision object |
| `U` | user point |

`lod->subobjectCount` tracks the maximum N seen (update at `0x423625`); the
`.3dp` `numpartanim` equals it. ASE subobject N (1-indexed) maps to PANM slot
N-1 (0-indexed). `[orig: ExportSubobjectTempBuild @ 0x457360]` runs once per
populated subobject slot during export.

Convention (verified against Armry01.ase / Armry01.3dp): geometry for part index
`i` is named `"%02d MeshM"` with parent `"%02d"` (e.g. `"01 Mesh0"` /
`*NODE_PARENT "01"` -> subobject 1 -> PANM slot 0).

**Gotcha (durable):** a synthesized name like `"subobject_0"` parses as
`atol(...) == 0`, which never increments `subobjectCount`; zero subobject slots
get allocated and `ExportSubobjectTempBuild` is never called — the export
silently drops all part data. Decoders must synthesize `"%02d Mesh0"` style
names (`robj_index + 1`).

---

## 4. Divergence catalog (D-3DI)

Stable IDs for the divergences described in prose above (dispositions in the
canonical vocabulary of [divergence-ledger.md](../divergence-ledger.md)). The
ledger's permanent register carries D-3DI-1.

| ID | Divergence | Disposition |
|---|---|---|
| D-3DI-1 | Byte-exact `MTRX` output requires OED's x87 `_PC_24` (24-bit single) precision (§1 "Derived chunks" MTRX derivation + §1 "OED x87 control word"); a 64-bit SSE2 build diverges in low FP bits. A parity sub-build restores byte-exactness via `_controlfp(_PC_24, _MCW_PC)` early in `main()`. | **PERMANENT** — an x87 FP-precision constraint, not a math error; the parity sub-build is the documented path when byte-exactness is needed. [orig: ComputeMTRX @ 0x452990 / WriteMTRX @ 0x452FA0 / _setdefaultprecision @ 0x52A27C] |

The §2.14 "Open items" are unresearched questions (no witnessed behavior gap yet)
and stay there; the §2 verdict tables are `MATCHING`/true-padding facts, not
divergences.
