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
| `libs/particle/src/parser.cpp` (apply_particle_key) | parser-fn | `CParticleDef_ParseProperties` | `@ 0x5ea320` (size 0x2525) | Full decomp 2026-04-27: stricmp dispatch on ~80 keys — matches our switch. Sets bit 0x02 on `reverse` trailing token (e.g. scale_func @ 0x5eafdd) and bit 0x01 on `inverse`. `[tabledef_edithandles]` @ 0x5ea346 returns 1 to skip section. | match | **Engine bugs preserved as documented**: g{2,3,4}_color1 falls through to higher-color slots in this dispatcher. Our parser does the *correct* mapping (g{N}_color{M} → graphics[N-1].color[M]); see open_questions.md. |
| `libs/particle/src/parser.cpp` (apply_particle_key) | hydrator-fn | `CParticleDef_ParseFromConfigMap` | `@ 0x5ed210` (size 0x1da5) | Reads ~80 named keys from config map → `CParticleEffectDef` (~5204 B); see field offset table below | match (witness only) | Drives our C++ `ParticleDef` field set. |
| `libs/particle/src/parser.cpp` (apply_table_key) | parser-fn | `CParticleTableDef_ParseScriptLine` | `@ 0x5e92b0` (size 0x266) | `[tabledef]` line driver | pending | Equivalent line-handler not directly decompiled; the corpus 32×8 invariant is verified across 77/77 files via the smoke test. |
| (none yet) | parser-fn | `CParticleTableDef_ParseProperties` | `@ 0x5eefc0` (size 0x228) | Companion property reader for `[tabledef]` | pending | |
| `libs/particle/src/parser.cpp` (apply_particle_key, graphic decl + g_* dispatch) | parser-fn | `CParticleDefEntry_ParseGraphicProperty` | `@ 0x5e3550` (size 0xabe) | Full decomp 2026-04-27: graphic1 resets idx=0, graphicN++ (capped at 3); per-key dispatch via strstr; "reverse"/"inverse" trailing tokens set bits 0x02/0x01 on each func. | match | Engine quirk: g2_color1 / g3_color1 / g3_color2 / g4_color1 / g4_color2 are remapped by the outer dispatcher (CParticleDef_ParseProperties) to higher-color slots before reaching here — see row above. |
| `libs/particle/src/particle.cpp::parse_blend_mode` + `blend_mode_name` | parser-fn | `CParticleDefEntry_ParseBlendMode` | `@ 0x5e29f0` (size 0xc8) | Full decomp 2026-04-27: chained strstr in this exact order: additive=1, blend=0, premult=2, bump=3, mod=4 (`off_7DCBA8`), mod2x=5, bumpadd=6, distort=7. Engine quirk: "bump" matches before "bumpadd" so `bumpadd` resolves to 3 (Bump), not 6. | match | Quirk replicated by our parser (otherwise corpus diverges). |
| (none yet) | parser-fn | `CParticleTableDef_ParseTransformFlags` | `@ 0x5e2950` (size 0x39) | Tabledef flag bits | pending | |
| `libs/particle/src/particle.cpp::parse_flag_table` | helper-fn | `FlagTable_ParseFromString` | `@ 0x5df970` (size 0x45) | Iterates {bit:u32, name:char[260]} entries (264 B stride, 4-byte preamble); ORs all matched bits via `strstr`. | match | Used to parse both flags table @ 0x846A18 (26 named bits / 29 slots) and move table @ 0x848800 (5 bits). |
| `libs/particle/src/particle.cpp::format_flag_table` | helper-fn | `sub_5DF9C0` (BuildFlagString) | `@ 0x5df9c0` (size 0x98) | Inverse of FlagTable_ParseFromString; emits names in table order, single-space separated, with one trailing space. | match | Replicated exactly so writer output matches engine layout. |

## Save / write (round-trip verification gold)

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `libs/particle/src/writer.cpp::write_particle` | writer-fn | `CParticleDef_SaveToFile` | `@ 0x5e4d70` (size 0x9ef) | full decomp 2026-04-27: field-by-field fprintf, `%5.3f` floats, BGR-byte order in memory printed back as R,G,B. Engine writes `emit_dur` **twice** (lines 0x5e4e6a + 0x5e4f30, same field +906) and `lod` (offset +38) regardless of whether ParseProperties consumed it. | match | Round-trip parity verified by `particle_writer_roundtrip_test`. Not byte-exact against corpus (engine writer uses `\n` only; OS may translate). |
| `libs/particle/src/writer.cpp::write_effect` | writer-fn | `CParticleEffectDef_WriteToFile` | `@ 0x5e0fe0` (size 0xc7) | full decomp 2026-04-27: `\tid = %s;`, `\tpdefs = name1, name2;` (separator from word_7CDA14 = ", "). Note: space-equals (not tab-equals like particledef). | match | |
| `libs/particle/src/writer.cpp::write_table` | writer-fn | `CParticleTableDef_WriteToFile` | `@ 0x5e27e0` (size 0xee) | full decomp 2026-04-27: 32 fixed rows of 8 `%u` values; `\ttlN = ...;` lines; "id = " uses space-equals. | match | Our writer zero-fills if parsed table has < 32 rows (defensive; corpus always has 32). |
| (none yet) | writer-fn | (no engine writer found for `[tabledef_edithandles]`) | n/a | format inferred from boatwake.ptl:40-45 | match (corpus-derived) | Implemented in `write_handles`; round-trip verified. |

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

## Runtime — semantic port in `libs/particle/src/emitter.cpp` (Phase 2, 2026-04-27)

This row group is "match (semantic)" — our portable simulator captures the
engine's per-particle behavior (emission, lifetime, integration, RNG resolution)
without claiming byte-exact parity with the DirectX-bound renderer.

| Code-loc | Item | Tag/struct/fn | Claimed citation | Resolved IDA witness | Verdict |
| --- | --- | --- | --- | --- | --- |
| `libs/particle/include/particle/emitter.h::Emitter` | struct | `CParticleEmitter` | runtime instance, ~252 B | parentPos, orient matrix, AABB, particle buffer ptr, stride, spring/drag/damping, force vec — full layout in SpawnParticle decomp | match (semantic) | Our portable struct does NOT mirror byte layout. |
| `libs/particle/src/emitter.cpp::emitter_advance` | runtime-fn | `CParticleEmitter_AdvanceFrame` | `@ 0x5e6570` (size 0x40b) | full decomp 2026-04-27: emit timing (interval = 1/emit_rate), burst loop, expire-by-age | match (semantic) | We collapse AdvanceFrame + UpdateParticles into one function. |
| `libs/particle/src/emitter.cpp::integrate_particle` | runtime-fn | `CParticleEmitter_UpdateParticles` | `@ 0x5e6980` (size 0x3df) | full decomp 2026-04-27: pos+=vel\*dt, vel.y += gravity\*dt, vel-=drag\*dt\*vel, scale+=scale_vel\*dt, age-=dt unless NEVERAGE | match (semantic) | Order matches engine (explicit Euler — pos first, then forces). |
| (none yet) | runtime-fn | `CParticleEmitter_UpdateAllParticles` | `@ 0x5f3be0` (size 0x1224) | combined update (large; orchestrates Update + Submit + Render) | deferred | Renderer-bound; out of scope for the portable simulator. |
| (none yet) | runtime-fn | `CParticleEmitter_BuildBillboardQuads` | `@ 0x5e6d60` (size 0x7d7) | render — vertex 28 B, 4 verts/quad | deferred | DirectX-bound; lives in the Godot wrapper layer. |
| (none yet) | runtime-fn | `CParticleEmitter_RenderStaticBillboards` | `@ 0x5f4e10` (size 0x80c) | non-rotating quad render | deferred | Renderer-bound. |
| (none yet) | runtime-fn | `CParticleEmitter_BuildOrientationMatrix` | `@ 0x5f3970` (size 0x267) | parent transform compose (4x4) | deferred | Skipped for the MVP simulator; `Emitter::position` + `Emitter::forward` is sufficient until parent-binding lands. |
| (none yet) | runtime-fn | `CParticleEmitter_TrySubmitForRender` | `@ 0x5e7540` (size 0x33) | submit gate | deferred | Renderer-bound. |
| (none yet) | runtime-fn | `CParticleEmitter_TranslatePosition` | `@ 0x5efe90` (size 0xad) | per-emitter position update | deferred | Set `Emitter::position` directly each frame. |
| `libs/particle/src/emitter.cpp::emit_one_internal` + `apply_emission_shape` | runtime-fn | `CParticleEmitter_SpawnParticle` | `@ 0x5e7640` (size 0xaa9) | full decomp 2026-04-27: emit_shape switch (1=box, 2=sphere, 3=cone), random color1..4 pick, age = age + age_adj\*rand10, scale_velocity = 1/age, per-particle flags from per-graphic _func presence | match (semantic) | Engine RNG resolution `rand() & 0x3FF` mirrored in `emitter_rand10`; we use a portable LCG so test seeds are platform-stable. |
| (none yet) | runtime-fn | `CParticleEmitter_SpawnNewParticle` | `@ 0x5f35b0` (size 0x28c) | wrapper / scheduler around SpawnParticle | deferred | Folded into `emitter_advance`'s emission loop. |
| `libs/particle/src/emitter.cpp::emitter_init` | runtime-fn | `CParticleEmitter_Init` | `@ 0x5419e0` (size 0x86) | emitter ctor / init | match (semantic) | Engine init also handles AnimMap + sound triggers — out of scope for our pure simulator. |
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
