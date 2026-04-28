# IDA witness matrix — particle system

**Re-validation pass started**: 2026-04-27 (worktree particles)
**Truth source**: retail `Jointops.exe` IDB at
`C:\Users\taylor\Development\opennova-godot\RE\Jointops.exe.i64`
**Companion artifacts**: [`ptl_format.md`](ptl_format.md) — narrative grammar;
[`ptl_corpus_catalog.md`](ptl_corpus_catalog.md) — corpus inventory;
[`open_questions.md`](open_questions.md) — unverified items.

## Verdict legend

- **match** — code citation matches IDA, layout/grammar matches IDA. Add inline `@ 0xADDR (IDAName)` if missing.
- **match-relabel** — address correct, layout correct, but human-readable label in code mislabels what the function does. Code stays, comment updates.
- **misaligned** — layout does not match IDA. Code needs fix.
- **missing-citation** — claim has no `@ 0xADDR` reference; needs one added.
- **missing-citation+verified** — no citation in code, but IDA witness is verified in this pass; just needs the citation added.
- **no-witness-found** — searched IDA, no match. Quarantine the call site.
- **pending** — not yet evaluated.
- **deferred** — out of scope for this worktree (runtime / emitter / atlasing).

## Section-tag dispatch

The four section strings live in `.rdata` and are referenced by the section dispatcher:

| Tag string | Address | Reachable from |
| --- | --- | --- |
| `[effectdef]` | `0x7dd9e8` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecb60` |
| `[particledef]` | `0x7dd9d8` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecb8c` |
| `[tabledef]` | `0x7dd9cc` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecbb8` |
| `[tabledef_edithandles]` | `0x7dccf0` | `CEffectWorld_ParseSectionCallback` data ref @ `0x5ecbe4`; also `CEffectTableDef_ParseCallback` @ `0x5e4020`, `CParticleTableDef_ParseScriptLine` @ `0x5e92e4`, `CParticleDef_ParseProperties` @ `0x5ea346` (used as an end-of-section sentinel) |

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `libs/particle/src/parser.cpp` (TBD) | dispatcher | `CEffectWorld_ParseSectionCallback` | `@ 0x5ecb40` (size 0x101) | Single function references all 4 section strings (data refs above); branches on tag → per-section parser | match (witness only) | Our C++ parser collapses dispatch + per-section into one switch — this function is the IDA mirror. |
| (none yet) | dispatcher | `CEffectTableDef_ParseCallback` | `@ 0x5e4010` (size 0x1b1) | Alternate path for `[tabledef_edithandles]` | pending | Likely an editor-only callback. Not on the runtime asset-load path. |

## Per-section parsers

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| (none yet) | parser-fn | `CParticleDef_ParseProperties` | `@ 0x5ea320` (size 0x2525) | Tokenizes `[particledef]` body into a `CConfigReader` map | pending | Inner tokenizer; not yet decompiled in detail. Cross-check via `[tabledef_edithandles]` ref @ `0x5ea346` (section terminator). |
| (none yet) | hydrator-fn | `CParticleDef_ParseFromConfigMap` | `@ 0x5ed210` (size 0x1da5) | Reads ~80 named keys from config map → `CParticleEffectDef` (~5204 B); see field offset table below | match (witness only) | Drives our C++ `ParticleDef` field set. |
| (none yet) | parser-fn | `CParticleTableDef_ParseScriptLine` | `@ 0x5e92b0` (size 0x266) | `[tabledef]` line driver | pending | |
| (none yet) | parser-fn | `CParticleTableDef_ParseProperties` | `@ 0x5eefc0` (size 0x228) | Companion property reader for `[tabledef]` | pending | |
| (none yet) | parser-fn | `CParticleDefEntry_ParseGraphicProperty` | `@ 0x5e3550` (size 0xabe) | Per-graphic-layer parser; 4 layers, 788 B each | pending | |
| (none yet) | parser-fn | `CParticleDefEntry_ParseBlendMode` | `@ 0x5e29f0` (size 0xc8) | Maps `additive` / `blend` / `distort` strings → enum | pending | Decode the full enum next round. |
| (none yet) | parser-fn | `CParticleTableDef_ParseTransformFlags` | `@ 0x5e2950` (size 0x39) | Tabledef flag bits | pending | |

## Save / write (round-trip verification gold)

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| (none yet) | writer-fn | `CParticleDef_SaveToFile` | `@ 0x5e4d70` (size 0x9ef) | Emits a `[particledef]` block | pending | Future: implement a C++ writer and round-trip against this output. |
| (none yet) | writer-fn | `CParticleEffectDef_WriteToFile` | `@ 0x5e0fe0` (size 0xc7) | Emits an `[effectdef]` block | pending | |
| (none yet) | writer-fn | `CParticleTableDef_WriteToFile` | `@ 0x5e27e0` (size 0xee) | Emits a `[tabledef]` block | pending | |

## `CParticleEffectDef` layout (heap, ~5204 B)

Offsets confirmed live from `CParticleDef_ParseFromConfigMap @ 0x5ed210` decompile, this RE pass.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` | `id` | std::string (heap) | First call: `CConfigReader_GetString("id", …)` |
| `+132` | `child_id` | std::string ptr | Sub-particle reference |
| `+136` | `flags` | u32 bitfield | Parsed via `FlagTable_ParseFromString(@ 0x846A18, …)` |
| `+140` | `move` | u32 enum | Parsed via `FlagTable_ParseFromString(@ 0x848800, …)` |
| `+148` … `+935` | `graphic[0]` | 788 B | First graphic layer |
| `+936` … `+1723` | `graphic[1]` | 788 B | Second graphic layer |
| `+1724` … `+2511` | `graphic[2]` | 788 B | Third graphic layer |
| `+2512` … `+3299` | `graphic[3]` | 788 B | Fourth graphic layer |
| `+3300` | `graphic_count` | u32 | Incremented in the parse loop after each found graphic |
| `+3304` | `alpha` | float | |
| `+3308` … `+3379` | `alpha_func` playback flags | 72 B | |
| `+3380` … `+3451` | `red_func` playback flags | 72 B | |
| `+3452` … `+3523` | `green_func` playback flags | 72 B | (note: parser order red/blue/green/blue is interleaved with strings; offsets verified at write sites) |
| `+3524` … `+3595` | `blue_func` playback flags | 72 B | |
| `+3596` | `color1` | packed RGB (4 B) | |
| `+3600` | `color2` | packed RGB (4 B) | |
| `+3604` | `color3` | packed RGB (4 B) | |
| `+3608` | `color4` | packed RGB (4 B) | |
| `+3612` | `bump_scale` | float | |
| `+3616` | `emit_dur` | float | |
| `+3620` | `emit_dur_adj` | float | |
| `+3624` | `emit_rate` | float | |
| `+3628` | `emit_rate_adj` | float | |
| `+3704` | `emit_delay` | float | |
| `+3708` | `emit_burst` | i32 | Engine clamps to ≥1 post-parse |
| `+3712` | `emit_maxoverride` | i32 | |
| `+3716` | `age` | float | Key string at `off_7DD900` (`"age"`, len 3) |
| `+3720` | `age_adj` | float | |
| `+3724` | `y_offset` | float | |
| `+3728` | `z_offset` | float | |
| `+3732` | `scale` | float | |
| `+3740` | `scale_adj` | float | |
| `+3744` … `+3815` | `scale_func` playback flags | 72 B | |
| `+3816` | `orientation` | vec3 | |
| `+3828` | `orientationadj` | vec3 | |
| `+3840` | `yaw_rot` | float | |
| `+3844` | `yaw_rot_adj` | float | |
| `+3848` | `pitch_rot` | float | |
| `+3852` | `pitch_rot_adj` | float | |
| `+3856` | `roll_rot` | float | |
| `+3860` | `roll_rot_adj` | float | |
| `+3864` | `orbitalspeed` | float | |
| `+3868` | `orbitalspeed_adj` | float | |
| `+3872` | `orbital_axis` | vec3 | Default `{0,1,0}` when missing |
| `+3884` | `spread` | float | |
| `+3888` | `spread_skip` | float | |
| `+3892` | `speed` | float | |
| `+3896` | `speed_adj` | float | |
| `+3900` | `elastic` | float | |
| `+3904` | `gravity` | float | |
| `+3908` | `gravity_mask` | vec3 | |
| `+3920` | `drag` | float | |
| `+3924` | `emit_shape` | i32 | |
| `+3928` | `emit_shape_size` | vec3 | |
| `+3940` | `emit_shape_size_skip` | vec3 | |
| `+3952` … `+4591` | `collide_sound[0..19]` | 32 B each | 20 slots, stride 32 |

Per-graphic-layer (788 B) layout, parsed in the loop at `0x5ee47f`+:

| Offset (within graphic) | Field | Type | Notes |
| --- | --- | --- | --- |
| `+0` … `+259` | `texture` | char[260] | e.g. `dirtpuf.tga` |
| `+260` … `+323` | `blend_mode_raw` | char[64] | lowercased; passed to `CParticleDefEntry_ParseBlendMode` |
| `+324` | `blend_mode_id` | i32 | result of ParseBlendMode |
| `+328` | `scale` | float | inherits from particle, then per-graphic override |
| `+332` | `scale_adj` | float | |
| `+336` … `+407` | `scale_func` playback flags | 72 B | qmemcpy'd from particle then per-graphic override parses on top |
| `+408` | `alpha` | float | |
| `+412` … `+483` | `alpha_func` playback flags | 72 B | |
| `+484` … `+555` | `red_func` playback flags | 72 B | |
| `+556` … `+627` | `green_func` playback flags | 72 B | |
| `+628` … `+699` | `blue_func` playback flags | 72 B | |
| `+700` | `color1` | packed RGB | |
| `+704` | `color2` | packed RGB | |
| `+708` | `color3` | packed RGB | |
| `+712` | `color4` | packed RGB | |
| `+716` | `flip_frames` | i32 | Defaults to 1 if not provided |
| `+720` | `flip_rate` | i32 | |

## Runtime — out of scope, documented for next worktree

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict |
| --- | --- | --- | --- | --- | --- |
| (none yet) | struct | `CParticleEmitter` | runtime instance, ~252 B | parentPos, orient matrix, AABB, particle buffer ptr, stride, spring/drag/damping, force vec | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_AdvanceFrame` | `@ 0x5e6570` (size 0x40b) | emission/expiration/child spawning | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_UpdateParticles` | `@ 0x5e6980` (size 0x3df) | per-frame physics integration | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_UpdateAllParticles` | `@ 0x5f3be0` (size 0x1224) | combined update (large) | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_BuildBillboardQuads` | `@ 0x5e6d60` (size 0x7d7) | render — vertex 28 B, 4 verts/quad | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_RenderStaticBillboards` | `@ 0x5f4e10` (size 0x80c) | non-rotating quad render | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_BuildOrientationMatrix` | `@ 0x5f3970` (size 0x267) | parent transform compose | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_TrySubmitForRender` | `@ 0x5e7540` (size 0x33) | submit gate | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_TranslatePosition` | `@ 0x5efe90` (size 0xad) | per-emitter position | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_SpawnParticle` | `@ 0x5e7640` (size 0xaa9) | per-particle init | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_SpawnNewParticle` | `@ 0x5f35b0` (size 0x28c) | wrapper / scheduler | deferred |
| (none yet) | runtime-fn | `CParticleEmitter_Init` | `@ 0x5419e0` (size 0x86) | emitter ctor / init | deferred |
| (none yet) | runtime-fn | `CParticleSystemDef_InitDefaults` | `@ 0x5e14f0` (size 0x3a3) | startup defaults | deferred |
| (none yet) | runtime-fn | `CParticleManager_Construct` | `@ 0x5e87f0` (size 0x1e2) | manager singleton | deferred |
| (none yet) | runtime-fn | `CParticleManager_BuildTextureAtlases` | `@ 0x5e8db0` (size 0x44d) | atlas baking from `graphicN` textures | deferred |
| (none yet) | runtime-fn | `CParticleManager_ResolveAllReferences` | `@ 0x5ec850` (size 0xc4) | links effect → pdef → table | deferred |
| (none yet) | runtime-fn | `CParticleManager_RecursiveSortAndRender` | `@ 0x5ec980` (size 0x188) | depth sort + draw | deferred |
| (none yet) | runtime-fn | `CParticleManager_RenderBatch` | `@ 0x5e9890` (size 0x466) | batch draw | deferred |
| (none yet) | runtime-fn | `CParticleManager_TransformToViewSpace` | `@ 0x5ecc50` (size 0x31c) | view-space transform | deferred |
| (none yet) | runtime-fn | `CParticleManager_BeginFrame` | `@ 0x5ecfc0` (size 0xd6) | per-frame begin | deferred |
| (none yet) | runtime-fn | `CParticleManager_FindTableDefByName` | `@ 0x5e9540` (size 0xec) | lookup by id | deferred |
| (none yet) | runtime-fn | `CEffectDef_ResolveTblDefReference` | `@ 0x5e9630` (size 0x4a) | curve ref → tabledef | deferred |
| (none yet) | runtime-fn | `CEffectDef_ResolveAllReferences` | `@ 0x5e9d70` (size 0x28e) | full resolve pass | deferred |
| (none yet) | runtime-fn | `CEffectDef_AddSubEffect` | `@ 0x5a3020` (size 0x18) | append child to effect | deferred |
| (none yet) | runtime-fn | `CEffectDef_FindByTypeName` | `@ 0x5b01a0` (size 0x34) | lookup by id | deferred |
| (none yet) | runtime-fn | `CEffectDef_Construct` | `@ 0x5b01e0` (size 0x5e) | effectdef ctor | deferred |
| (none yet) | runtime-fn | `CEffectDef_InvokeFactory` | `@ 0x5e19e0` (size 0x4e) | factory dispatch | deferred |
| (none yet) | runtime-fn | `CEffectDef_SetTextureName` | `@ 0x5ef8e0` (size 0x1f) | texture assign | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_Initialize` | `@ 0x5e6020` (size 0x541) | emitter init | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_AdvanceEmission` | `@ 0x5e1d30` (size 0x1dc) | per-frame emission step | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_SpawnBetweenPositions` | `@ 0x5ea0a0` (size 0xa1) | spawn along a segment | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_Destroy` | `@ 0x5e3460` (size 0x83) | dtor | deferred |
| (none yet) | runtime-fn | `CEffectEmitter_SetOrientationFromDirection` | `@ 0x5e5b00` (size 0x2ae) | orient from facing dir | deferred |
| (none yet) | spawn API | `CEffectWorld_SpawnEmitterAtPosition` | `@ 0x5f6df0` (size 0x182) | top-level spawn entry | deferred |
| (none yet) | spawn API | `CEffect_UpdateEmitterTransform` | `@ 0x5f7410` (size 0x1be) | per-frame transform update | deferred |
| (none yet) | weather | `WeatherParticle_UpdateAllEmitters` | `@ 0x5cb100` (size 0x4b0) | separate weather particle subsystem | deferred |
| (none yet) | weather | `WeatherParticle_LoadTextures` | `@ 0x5de840` (size 0x85) | weather texture load | deferred |
| (none yet) | debug | `Debug_DrawParticleStats` | `@ 0x44c840` (size 0x10e) | debug overlay | deferred |
