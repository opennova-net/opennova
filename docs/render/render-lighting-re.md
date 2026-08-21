# World lighting / modulator chain — reverse-engineering record

The runtime lighting chain: the iris auto-exposure modulator, the per-pass
world lighting block, the per-entity shader uniforms and D3D light set, the
dynamic point-light path, the terrain/foliage lighting constants, and the
lighting texture/cubemap resources — witnessed in retail `Jointops.exe`
(imagebase `0x400000`, IDB `Jointops.exe.kong.i64`; all addresses are that
binary's). Implementing code: `engine/runtime/renderer/light_runtime.{h,cpp}` (the
witnessed chain as pure functions), `engine/formats/env` `env_weather.h`/`env_render.cpp`
(`ModulatorChain`, `WeatherColorBlock::set_step_deltas`, `iris_gain`),
`engine/runtime/renderer/object_shader_template.cpp` (the renderer-neutral typed
object-pipeline descriptor; legacy filename),
`godot/src/env/nova_weather_core.cpp` (the ticked chain),
`godot/src/env/{nova_weather,nova_mission_environment}.cpp` +
`godot/src/object/nova_object_model.cpp` (the uniform feed),
`godot/shaders/terrain_lighting.gdshaderinc` (the terrain c0/c1 surface).
Landed by maturity REN-5 ([maturity-program.md](../maturity-program.md);
standing rules [ADR 0023](../adr/0023-render-visual-parity.md)). The T1
instrument (`tests/renderer/state_vectors_test.cpp` section 5) pins every
scalar function in this record; the GUT env vectors
(`godot/tests/env_parity_vectors_test.gd`) pin the ticked modulator chain.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Modulator → shader gain unpack (÷64, 64 = identity) | MATCHING | `renderer::unpack_modulator_scale` `[orig: Render_UnpackModulatorToLightScale @ 0x58db30 → Render_LightScaleR/G/B @ 0x8409f4..fc; EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0 → @ 0x840b24..2c]`; `renderer_state_vectors` section 5 (identity 0x404040 → exactly 1.0, 0xFF → 255/64) |
| The modulator chain (modulator2 → modulator → every color block, same tick) | MATCHING (ported; env #17 CLOSED) | `env::ModulatorChain` + the witnessed block order `[orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..0x57f03c]`; 62-tick target chase `[orig: @ 0x57e512..0x57e538; ColorBlock_SetStepDeltas @ 0x57d940]`; env vectors re-dumped surgically (exactly the 8 weather checkpoint rows; hand-check wb/k064 `0x31·61/64 = 0x2E`) |
| Iris auto-exposure sampling | MATCHING (marched port, bounded residuals) | the curve was already ported (env record §Iris); the 3-point camera-ray march is PORTED (2026-07-18): `Simulation::compute_iris_samples` (terrain-clipped 8 u camera ray, thirds march, per-sample blink/indoor classification + 3 sun-occlusion rays) → `WeatherCore::set_exposure_from_iris_samples` (per-sample curve vs ceiling/floor indoors, ×level/8 sun outdoors, INT /3 average) `[orig: compute_ambient_light_along_direction @ 0x5c7a00; terrain_sector_compute_lighting @ 0x5c7550]`; the outdoor sample stays the no-world editor fallback; residuals on D-RLIT-2 |
| World lighting block (per-pass build + ctx store) | MATCHING (math ported) | `renderer::build_world_lighting` `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; RenderBatchCtx_StoreLightingConstants @ 0x5d89e0]` incl. the NVG hemi rewrite, vehicle-scope grey, NVG world dim, and the two hemisphere averages; `renderer_state_vectors` section 5 |
| Per-entity uniforms (slots 227-230) + interior daylight lerp | MATCHING (math ported; reimpl transfer wired) | `renderer::compute_entity_lighting` `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`; the aux float = the parent interior's daylight openness (model+536), NOT a dual-LOD fade. The reimpl now parses `items.def light_transfer` as a clamped percentage, carries it through `ItemDatabase`, and applies the normalized value to interior ROBJ sections and contained player/viewmodel lighting |
| FF vertex lighting (ambient + dir + hemisphere delta lights, saturate, ×2) | MATCHING | `renderer::ff_vertex_light` + the checked-in technique implementation `[orig: Lighting_SetHemisphereD3DLights @ 0x5d8cb0; D3D light 0 @ 0x5d9ce2..0x5d9d76; _FFP.fx TSSColor MODULATE2X]`; D-RMAT-5 FIXED; T2 swatch: 116/120 cells moved, the 4 VS_TRACER (unlit, MODULATE 1×) cells byte-identical |
| Per-entity sun visibility (effectScale source) | MATCHING (2026-08-18: the outdoor feed is live) | `renderer::sun_visibility_factor` `[orig: Entity_ComputeSunVisibility @ 0x5c6800; stack write @ 0x5c7fa5]`; `world::CollisionWorld::sun_visibility_blocked_rays` casts the witnessed one-segment/three-radius query against the drawn entity's OWN proximity-candidate slice (the `+0x1BC`/`+0x1C0` walk of `raycast_find_collision_entity @ 0x539a70` — only structures overlapping the entity's inflated bubble can shade it; an empty slice keeps quality 4, retail's `+0x1C0 == 0` skip), `Simulation::get_entity_sun_visibility_changes` diffs quality per bms_id for the shell, and the local player's quality reaches the presenter (third-person body dims; FP parts keep the witnessed effectScale=1 exemption) (D-RLIT-3) |
| EffectWorld dynamic point lights (instance pool, spawn/query/select, color × modulator × RgbGen, {1,0,15/r²,1}, ≤4, owner/interior groups, fade/decay lifecycle, transient spawners, coronas) | **PARTIAL (2026-08-20 vertex-rate delivery + coronas ported, spot delivery witnessed DEAD; interior groups + terrain/foliage open)** | `renderer::LightScene` (engine/runtime/renderer/light_scene.h carries the witness map) hosts the 4096×176B pool, safe generation leases, the faithful group-gated selection API, target-disable filtering, and a separately named all-overlap camera query `[orig: Light_InstanceTable @ 0x2732e28; collect_nearby_zones_by_aabb @ 0x5aa250; update_light_slots @ 0x5abc50 — no xrefs; the live per-draw select+enable is `Light_SelectAndEnableForDraw @ 0x5ab9d0` (ex collect_visible_foliage_slots, renamed 2026-08-20), same gate semantics]`. `EffectLightDirector` routes mission-start/late-node model lights and four transient families into object shaders; per-draw owner isolation ported 2026-08-18 (`LightScene::select_for_draws`, per-instance light uniforms, camera-global query = report census only); batched static sources spawn with a synthetic owner so subobject-attached records obey the group gate instead of leaking as world lights (2026-08-20, `[orig: Entity_SpawnGlowEffects @ 0x56c8ae — SetOwnerGroup(entity, bone) for every spawning entity kind]`). Point-light shading now runs at the VERTEX rate like the D3D pipeline the pool lights ride (`v_point_light_diffuse` in shared.gdshaderinc `[orig: Light_ApplyAsD3DLight @ 0x5abd50 — LightEnable on fixed-function VERTEX lights]`) — the per-pixel evaluation painted a circular pool retail never draws (the 00tra-armory-lght-retail fixture). Corona billboards ported IN FULL (`LightScene::collect_corona_quads` + `light_corona.gdshader` `[orig: EffectWorld_RenderLightCoronas @ 0x5aaf40; texture "texlightcrn" @ Lighting_InitTextures 0x5a94f0]`), including the owner visible-section gate (the occlusion verdict masks feed the collect `[orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960 over g_BuildingSectionVisMask @ 0x297f250 — the ported OcclusionWorld::section_mask domain]`), the flag-0x100 impact re-center (the flag's only witnessed reader — the corona drops radius/2 back onto the impact `[orig: @ 0x5ab037..0x5ab05c]`), and the fog-to-black fold (mode-2 fog forces FOGCOLOR black `[orig: CD3DDevice_SetFogAndBlendMode @ 0x677740 case 2]`). Spot/Target-cone runtime delivery is witnessed DEAD CODE in JO (`LightPool_SpawnSpotProjectorEffect @ 0x5a9fd0` is caller-less; model LGHT falloff/rotation/view_proj never reach the pool). Interior groups, terrain/foliage delivery, and static-destruction rebinding remain residuals on D-RLIT-4. |
| Model-authored `LGHT` chunks | **RE-GRADED 2026-08-16**: consumed at spawn via the EffectWorld pool, never via per-material uniforms | The old “no post-load read” claim audited the wrong field: the +0xCC xref audit missed that `Entity_SpawnGlowEffects @ 0x56c7c0` walks the model's light array (count at model dword 49 = +0xC4, records at dword 50 = +0xC8, stride 120) at mission start and spawns one EffectWorld pool instance per record — white spawn color, radius = atten_end × 65536, the record's RGB-gen block attached, subobject/blink-box owner attach, and the three authored disable flags → render flags 512/1024/2048 `[orig: @ 0x56c836..0x56c92c]`. The per-material `u_local_light_*` path stays editor-preview-only (`u_local_light_count = 0` in gameplay — that half of the old row remains true); gameplay illumination flows through `renderer::LightScene` |
| Terrain surface c0/c1 | MATCHING (ported) | c0 = SKY block, c1 = LIGHT block (both [0] ÷255): `renderer::terrain_surface_light`, `terrain_lighting.gdshaderinc` corrected from the gobj-era combined/fill guess `[orig: terrain_setup_lighting_and_shader @ 0x604420; init_terrain_lighting_color_ramps @ 0x604ee0 ← Render_TerrainScene @ 0x610c80]` |
| Dynamic projected entity shadows | WITNESSED / reimpl-native approximation | Retail allocates the independent projected render-slot path for people (and the local player) or ItemDef `DynamicShadow`; attached third-person weapons join their entity, while the first-person viewmodel does not cast. ItemDef `NoShadow` does not gate this path. Mission placement carries that admission policy (the aspirational streamed-model resolver twin was deleted 2026-08-11 — unreferenced since birth) `[orig: Entity_InitFromModel @ 0x40E1BC..0x40E1F7; GUT: mission_object_placer_test, nova_object_model_runtime_gate_test]` |
| Static sector/model sun shadows onto terrain/foliage | WITNESSED / hosted page-alpha subset | pool-2 buildings cast unless `NoShadow`; pool-1 items additionally require `StaticShadow`; every ROBJ in the selected LOD enters a black PROJSHAD temporary RT which is composited into terrain-tile alpha, not back onto sector models. Runtime now collects typed static sources, resolves supported LOD/ROBJ and `TEX_TEAM` alpha frames, rasterizes A-only projections into the shared terrain/foliage page, and retires the global directional surrogate `[orig: Terrain_CollectAndRenderTileModels @ 0x60D250; Render_SubmitEntity @ 0x60D971; PolyTrn_RenderTile composite @ 0x60E0C6..0x60E19D]`; unsupported animation/skinning and exact general projection/depth/cache cadence remain D-TERRAIN-7 |
| Foliage/sector-model lighting constants | MATCHING (witnessed; values pinned) | the blend PS inherits the terrain's device c0/c1 (no foliage-side write) `[orig: Foliage_SetupFarSlotDraw @ 0x6007c0]`; the lightmap-tile pass `[orig: Foliage_RenderFarPatches @ 0x609de0]` (renamed 2026-08-15, ex render_terrain_lightmaps); foliage.gdshader header updated |
| Lighting textures + DOT3 dynamic-light shader | witnessed / reimpl-native equivalent | procedural falloff set + the last embedded PS outside FrameFX `[orig: Lighting_InitTextures @ 0x5a94f0]` — the ps.1.1 DOT3 per-pixel light is the fixed-function era's OmniLight; the reimpl's real per-pixel lights serve the intent (D-RLIT-6 note) |
| Cubemap sources (CubeEnvironment / CubeRotSpecular / CubeNormalize) | witnessed (the D-RORD-5 specular-cube question CLOSED) | live scene cube re-rendered 6 faces per 128 frames `[orig: update_environment_cubemap @ 0x6106a0]`; the static sun-glint cube (white pow-800 + warm pow-40 along −Z, rotated by MatRotSpecular) `[orig: Render_FillStaticCubemaps @ 0x58f290 → generate_cubemap_lighting @ 0x685bb0]`; normalization cube `[orig: generate_normalmap_cubemap @ 0x685570]`; the analytic 5-light sky fill is caller-less dead code |
| Render-slot (character shadow) lighting | witnessed / out of REN port scope | dominant-light pick + terrain shadow-anchor march `[orig: RenderSlot_UpdateEntityLight @ 0x5d6a30]`, slot render lighting (D3D light 4, NTSC-weighted negated colors into PS c21-23) `[orig: RenderSlot_SetupNextLighting @ 0x5d7250]` — the Shadow_/Scar_ family exclusion (ADR 0023) |

## Witness map

**The modulator chain (env #17's consumer).** Every weather color block's
render color multiplies by the modulator block; the modulator multiplies by
modulator2; modulator2 by the constant identity bytes 0x404040. The witnessed
tick order is **modulator2 → modulator → light → sky → ground → fog → skyfog
→ ceiling → cloud → floor → skybase/bright/highlight → cloudbase/highlight/
edge** — the 16 `interpolate_weather_color` calls at
`[orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c]` (decoded from
the `mov ecx, imm32` block addresses), so the exposure propagates SAME-TICK.
The modulator's target is the iris sample replicated to gray
(`0x10101 × gain`) and chased over 62 ticks: `ColorBlock_SetStepDeltas
@ 0x57d940` sets each channel's max step to
`|target_byte<<20 + frames/2 − current| / frames`
`[orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538]` — retail
re-targets every render pass, i.e. every tick. `compute_ambient_light_along_direction
@ 0x5c7a00` produces the gain (fully pinned 2026-07-18, ported at the marched-iris
slice):

- **Ray**: end = camera + camera-forward × 8.0 — the `(0x80000, 0, 0)` vector
  rotated through the camera Euler matrix (camera globals @ 0xA78364..78)
  `[orig: @ 0x5c7a56..0x5c7a6f]`, then `raycast_entity_collision @ 0x413760`
  clips the end IN PLACE: terrain first (`Terrain_RaycastHeightmapHiRes_0
  @ 0x60e710`, called `(start, end, end)`), then the entity collision-model
  nearest hit `[orig: the Flags & 0x800000 indoors gate skips the terrain leg
  @ 0x413785]`.
- **March**: samples at end, end + (cam−end)/3, end + 2(cam−end)/3 (truncating
  integer thirds) `[orig: @ 0x5c7ad8..0x5c7b30]`; the iris curve runs
  PER SAMPLE and the three INT gains average `(s0+s1+s2)/3`
  `[orig: @ 0x5c7b45]`.
- **Per sample** (`terrain_sector_compute_lighting @ 0x5c7550`): blink-section
  test at the point against the sector's type-5 entities
  (`Entity_TestCollisionSections @ 0x4aef90`, flags 0x8000). INDOOR (hit slot 0
  nonzero): pool-2 entity = `hit >> 20`; **no interior data (`pool_entry[12]`
  == 0) short-circuits to the curve with ALL inputs zero — the ÷2m limb
  diverges and the clamp serves gain 255** `[orig: @ 0x5c7652]`; else
  directional = 0, sky/ground ← the CEILING/FLOOR blocks' [1] slots
  (@ 0x26C6474 / @ 0x26C64DC), interior light group = `(hit >> 12) & 0x1F`
  (`Lighting_SetInteriorLightGroup @ 0x5a90e0`, pair @ 0x272ED84/0x272ED80).
  OUTDOOR: sun level = 8 − one per BLOCKED sun ray — three entity-only
  raycasts (`raycast_find_collision_entity @ 0x539a70` with allowAllTypes = 1,
  hit → `neg/sbb/add 1` → −1) from the sample to sample + 200 × light_dir,
  clip radii −0x2000/−0x5000/−0x8000 `[orig: @ 0x5c7765..0x5c77d7]`, gated on
  the CALLER's sector having entities (`sector[112] @ 0x5c7707` — an
  optimization: with no entities the rays cannot hit); directional = light
  block [1] × level/8 (`light_scale = level × 1/2040 @ 0x5c77e9`), sky/ground
  = sky/ground [1]; light group cleared (0, 0). Then the iris curve
  ([env-tod-re.md](../env/env-tod-re.md) §Iris auto-exposure; luminance
  weights 0.25/0.5/0.25).

Reimpl: `Simulation::compute_iris_samples` (sampling; Godot camera →
mission fixed) + `CollisionWorld::segment_hits_static` /
`world::terrain_clip_segment` (the ray primitives) +
`WeatherCore::set_exposure_from_iris_samples` (per-sample curve + /3
average), stamped per render frame by `GameWorld._stamp_iris_samples` and
consumed by `Weather`'s per-tick re-target; the pure outdoor sample
remains the no-world editor fallback.

**The gain unpacks.** `Render_UnpackModulatorToLightScale @ 0x58db30` writes
`Render_LightScaleR/G/B @ 0x8409f4..fc` = modulator bytes ÷ 64; its SOLE
consumer is `apply_shader_parameters @ 0x58e05d` binding **ColorSrcGlobalGain
(handle slot 232)** — in the shipped `.fx` corpus the SELFLUM emissive
(`SelfLumColor × ColorSrcGlobalGain`). `EffectWorld_UnpackModulatorToAmbientScale
@ 0x5aaef0` writes `EffectWorld_AmbientScaleR/G/B @ 0x840b24..2c`, consumed
by `Light_GetPointLightParams @ 0x5a9180`, `Light_FillD3DPointLight
@ 0x5aa450`, `render_foliage_instance @ 0x5aa830`,
`foliage_setup_render_matrices @ 0x5aab30`, and
`EffectWorld_RenderLightCoronas @ 0x5aaf40` (ex CEffect_RenderFoliageBillboards,
renamed 2026-08-20 — it renders light coronas, not foliage) — dynamic lights
and foliage/effects brightness ride the exposure.

**The world lighting block (writer).** `CTerrainRenderer_BuildLightingShaderConstants
@ 0x5c8090` (callers: `Terrain_RenderSceneWithReflection @ 0x5c93a0` ×5 —
per-wave, mirrored and unmirrored — `Player_RenderFirstPersonViewModel
@ 0x4deea4`, and the offscreen `Water_RenderReflectedWorldScene`) fills a 32-float block from the
env blocks' **[0] render colors** (post-modulator — the exposure reaches
world lighting through the block colors themselves) ÷255: [0] dir-enable
flag, [1..3] ← `Env_LightBlock`, [5..7] ← −normalize(`Environment_GetLightDirectionFloat
@ 0x57d870`), [8..10] ← `Env_SkyBlock`, [12..14] ← `Env_GroundBlock`,
[16..18] ← `Env_FloorBlock`, [20..22] ← `Env_CeilingBlock`. Overrides: the
NVG hemisphere rewrite (`g_NVGActive @ 0xb7654c` && !`dword_A890C8`):
`c' = c·0.25f' + modulator_byte·f'/640`, `f' = (g_NVGBrightnessLevel+1)·0.2`
(`@ 0x5c8205..0x5c82e9`); the vehicle-scope grey (`Player_CanFireWeapon` &&
`Player_IsVehicleSeatHasFlag4`): dir 0.1, all hemis 0.5, flag cleared
(`@ 0x5c8389..0x5c843c`); the NVG world dim (the bool arg): everything 0.25,
dir zeroed (`@ 0x5c8448..0x5c84f0`). `RenderBatchCtx_StoreLightingConstants
@ 0x5d89e0` (ex-misnomer `set_bounding_volume_from_box`) copies the block to
batch-ctx +112 and derives **ctx+208 = (ceiling+floor)/2** and **ctx+224 =
(sky+ground)/2** (the AmbientColor source); flag 0 zeroes the dir color.

**The per-entity reader.** `setup_entity_lighting_and_shader_constants
@ 0x5d98a0` (per FlushBatches entry) pushes the uniform surface — the handle
slots pinned by their `GetParameterByName` stores
`[orig: HLSLEffect_LoadFromFile @ 0x5af3fe..0x5af485]`: **225 CameraPos,
226 DirLightVector, 227 DirLightColor, 228 HemiGroundColor, 229 HemiSkyColor,
230 AmbientColor**. Outdoor path: 227 ← ctx dir color × entry[9]
(effectScale), 228/229/230 ← ground/sky/outdoor-average direct. Under batch
entry flag bit 1 (submit 0x80 — interior-parented entities and the armed
viewmodel): 227 additionally × entry[10], 228 ← lerp(floor, ground, t),
229 ← lerp(ceiling, sky, t), 230 ← lerp(indoorAvg, outdoorAvg, t) where
**t = entry[10] = the parent interior's daylight-openness float** (interior
model data +536, captured into the render-state stack aux by the wave
`[orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7f8a..0x5c7f93]` and the
viewmodel `[orig: @ 0x4def3c]`) — a closed building gets pure floor/ceiling
ambience with no sun. **entry[9] = the sun-visibility factor**.
`Entity_ComputeSunVisibility @ 0x5c6800` returns 1.0 when the entity's
proximity-source count at +0x1C0 is zero. Otherwise its origin is entity
position plus the uniformly scaled collision-AABB midpoint stored at
+0x1FC..+0x204, and its endpoint is 200 units along the active fixed light
direction. It calls `raycast_find_collision_entity @ 0x539a70` at radii
−0x2000, −0x5000, and −0x8000; zero means blocked, and the result is
(4−blocked)×0.25, written into the pushed stack top at 0x5c7fa5. The midpoint
and signed-rounding scale are built by `Entity_InitFromModel` at
0x40df2e..0x40e03c; raw type 6 with ItemDef attrib 0x20 uses a zero center
instead (`@ 0x40defc..0x40df16`). `Entity_BuildProximityListsFromPools
@ 0x4b8eb0` creates source slices only for pool 0 and eligible active pool 1
entities (the attrib-0x20 exclusion is waived for item type 1), so pool-2
statics naturally remain at full sun. An interior-parent reference at +0x1D0
bypasses the rays and keeps 1.0
`[orig: setup_terrain_effect_for_entity @ 0x5c74ae..0x5c74f3]`. The reader
then fills **D3D light 0** (directional, diffuse = the scaled dir color,
direction = ctx dir or the defaults `g_DefaultLightDir{X,Y,Z} @ 0x8437e0`
= {0,1,0} when the flag is clear — with the color already zeroed) and calls
`Lighting_SetHemisphereD3DLights @ 0x5d8cb0` (ex-misnomer
`setup_d3d_clip_planes`): **D3D light 2** = directional {0,−1,0} with
diffuse = sky − ambientAvg, **D3D light 3** = {0,+1,0} with ground −
ambientAvg — the fixed-function hemisphere as two DELTA lights pivoting on
the average (identically `mix(ground, sky, N.y·0.5+0.5)`). Under the mirror
gate (ctx+841) slot 226 gets the re-negated direction (w = 0.8); in normal
passes 226 is never pushed — the FF path lights through D3D light 0. The
combined FF vertex diffuse saturates and the output stage doubles it:
`TSSColor(0, Modulate2x, Texture, Diffuse)` — **pixel = tex × min(lit,1) × 2**
(VS_TRACER alone modulates 1×).

**Hosted `light_transfer` wiring (2026-07-29).** The `items.def` parser now
recognizes `light_transfer`, clamps the authored integer percentage to
`0..100`, and stores its normalized `0.0..1.0` value through the def FFI and
`ItemDatabase` (Ihq01 is the shipped witness: `20` → `0.2`). Mission
placement supplies that value before building the model's section materials.
The portal split leaves ROBJ 0 on outdoor lighting and interpolates ROBJ 1+
toward the floor/ceiling blocks by the parent transfer. A player inside the
building's blink volume, including first-person arms/weapon lighting, inherits
the same parent value even when the coarse `local_player_indoors` state is
false. This closed the interior half of D-RLIT-3; the outdoor half landed
2026-08-18 — the occlusion frame now runs retail's one-segment/three-radius
sun query per drawn entity against the entity's OWN proximity-candidate
slice (`world::CollisionWorld::sun_visibility_blocked_rays` — the
`raycast_find_collision_entity @ 0x539a70` walker iterates entity_a's
`+0x1BC` arena block bounded by the `+0x1C0` count, so only structures whose
inflated sphere overlaps the entity's bubble ever block; a far building's
long shadow leaves it in full sun; clip radii −0x2000/−0x5000/−0x8000 shared
with the iris march) and applies the `(4 − blocked) × 0.25` factor through
`ObjectModel.set_entity_lighting_context` as a per-bms change list.
Contained entities keep the interior route (factor 1.0); pool-2 statics,
slice-less rows, and the FP viewmodel keep the witnessed full-sun default.

**EffectWorld dynamic point lights.** 176-byte records in `Light_InstanceTable
@ 0x2732e28`, handle-indexed; per-frame in-frustum handles in
`Light_VisibleHandles @ 0x272eda0` (count @ 0x272ed98). Params
(`Light_GetPointLightParams @ 0x5a9180`): position fixed→float Y-negated,
pos.w = 65536/range_fixed, color = record RGB × `EffectWorld_AmbientScale` ×
intensity (+ optional RgbGen animation via `RgbGen_EvaluateColor @ 0x5b23d0`,
gen block ticked by `Light_TickGenBlock @ 0x5a8ae0`), attenuation
`{1, 0, 15/range², 1}` with range = fixed × 1.25/65536 — the same set the
OED preview emits (PrepareLightParams @ 0x46A500). `Light_FillD3DPointLight
@ 0x5aa450` fills a D3DLIGHT9 (type POINT) with the same values and a
**1.5× diffuse boost**. Group culling: each light carries an owner pair
(entity ptr + section at record +76/+80); `Light_PassesActiveGroups
@ 0x5a9120` accepts lights owned by the sample position's interior
(`Lighting_SetInteriorLightGroup @ 0x5a90e0`, section-matched) or by the
entity being rendered (`Lighting_SetOwnerLightGroup @ 0x5a9100`);
`update_light_slots @ 0x5abc50` enables at most **4 concurrent D3D lights**
(indices offset by `Light_D3DIndexBase @ 0x840b20`), maintaining the active
list @ 0x2732dfc.

These records ARE fed from model `LGHT` at spawn (2026-08-16 correction; the
old +0xCC xref audit checked the wrong field): `Entity_SpawnGlowEffects
@ 0x56c7c0` walks the model's post-load light array (count at +0xC4, records
at +0xC8, stride 120) and spawns one pool instance per record — the mission
start walks pools 1..2 (`Game_StartMission @ 0x525d19 → sub_5227B0`), and
powerup registration spawns late (`@ 0x442ce6`). The flicker chain closes
RQ-2: reading an instance with a gen block runs `Light_TickGenBlock
@ 0x5a8ae0`, which hashes the light's fixed position into the weather wave
ring and writes the GLOBAL FLICKER control register (0x83FD00 = value slot
0x83FCE8 + 8·ordinal 3), so style-113 records interpolate start→end by a
position-phased ring sample. The per-material `u_local_light_*` uniforms stay
editor-preview-only (`ObjectPreview.set_model_light_preview_enabled(true)`);
gameplay illumination flows through the pool.

**Lifecycle and the transient spawners (2026-08-16, routed; delivery partial).** Record dword
15 is a FADE MODE, dwords 16/17 the countdown (current/initial), float 14
the blend the params multiply as `intensity`. The per-frame tick
(`EffectWorld_TickInstancesAndLightScale @ 0x5aa170`, called from
`Game_ProcessMainFrame @ 0x5267a1`) decrements a positive counter; at
expiry, mode 5 hides the slot (flag bit 2 — the query-skip bit;
`CEffectInstance_SetBlendAmount @ 0x5a8ee0` ≥ 0.001 re-shows it) and every
other mode memsets the slot dead; while counting, modes 2/5 render
`blend = d16/d17`. The same tick derives the ambient scale from
`Env_TerrainColorRecip` bytes ÷ 128 (the reimpl feeds the env light-state
gain instead — tracked). The instance API around it:
`LightInstance_SetFadeModeAndDuration @ 0x5a8f80` (d15/d16/d17),
`LightInstance_SetOwnerGroup @ 0x5a8fb0` (d19/d20),
`CEffectInstance_SetPositionAndBounds @ 0x5a9070`,
`CEffectInstance_ClearByHandle @ 0x5a8ea0`,
`CEffectInstance_ModifyRenderFlags @ 0x5a8f20`. Retail's setters write
through STALE handles into whatever occupies the slot; the port carries an
opaque generation lease and rejects every stale mutation even after slot
reuse (intentional safety divergence). Four transient spawners are routed:

- **Muzzle glow** (`Entity_UpdateMuzzleGlowEffect @ 0x56c960`, from
  `WeaponSlot_FireAndSpawnEffects @ 0x53f597` — AI/authority fire at the
  fire position — and `ActionSlot_SpawnEffect @ 0x402080` — the action-row
  FIRE arm at the action-transform muzzle; both gated on ammo `MF_Light`
  +36): spawn-once per entity (handle cached at entity+436), radius 1.5,
  color 0xFFE0A0, re-armed per shot to mode 4 / 5 ticks + blend 1.0, owner
  = the shooter. In retail, owner ≠ 0 means the glow lights ONLY the shooter's
  own draws under the per-draw group gate (a self-light) — and once the 5-tick
  fade kills the slot, the cached handle re-arms a dead slot: that entity
  never glows again this life (the flash particle masks it).
- **Impact flash** (`AmmoDef_ProcessImpactEffect @ 0x40a2b3`): ammo
  `light_impact <radius> <r> <g> <b> <seconds>` (`AmmoDef_ParseProperty
  @ 0x40af79` → +132 radius fp16, +128 packed RGB, +136 seconds×62 ticks,
  0 → 10 `@ 0x40b005`), spawned radius/2 above the impact, mode 2, gated on
  the impact-effect leg actually presenting; render flag 0x100 has no
  witnessed reader.
- **Death flash** (`Entity_SpawnDeathPieces @ 0x49351a`): husk deaths above
  water, non-decorations — 2× the PIECE model's bound radius
  (huskFinal ?: husk `@ 0x4934af`), color 0xFFC080, mode 2 / 31 ticks,
  corona disabled (flag 512).
- **Round glow** (`RoundData_SpawnRound @ 0x4ec8da`): ammo `light_move`,
  mode 1 / duration −1 (permanent), terrain disabled (flag 1024), handle at
  round+0x1B4, spawned radius/2 up, per-tick follow at the raw round
  position (`@ 0x4eaa9f`), cleared in `Projectile_ReleaseEffects`.

The owner-group write in the model-light spawner is conditional
(`@ 0x56c8ae..0x56c8db`): a record with a nonzero attach bone is owned by
its entity + section (cabin self-lights); a record inside a blink box is
owned by the CONTAINING building + section (interior room lights); every
other record — the fire barrels — spawns unowned and lights the world.
`Lighting_OwnerGroupEntity`/`InteriorGroup*` are per-DRAW-CONTEXT state, so
owned lights illuminate only their own entity's draws. The portable
`LightScene` preserves that gate. The current Godot adapter cannot provide a
draw context because it publishes process-global shader parameters, so its
explicit camera-global object fallback admits owned lights unscoped. Muzzle
and cabin lights are therefore visible, but can bleed onto nearby objects;
that is an approximation, not owner-group parity.

Mission-start static model lights and late ObjectModel nodes are wired into the pool. Late
nodes retire their handles on `tree_exiting`; mission reset synchronously
clears both the pool and published shader count. Static MultiMesh source rows
still lack a stable BMS identity, so destroying/restoring a batched model does
not retire/respawn its lights and a grafted husk is not rescanned for `LGHT`.
That placed-model lifecycle tail remains open.

`LightInstance_IsAliveAndLightsTerrain @ 0x5a8fe0` (valid ∧ ¬flag 0x400) is
the terrain-projection admission check. RESOLVED 2026-08-20: flag 0x10000 is
the SPOT-PROJECTOR mark, not a corona mark — its only writer is the
caller-less `LightPool_SpawnSpotProjectorEffect @ 0x5a9fd0` (dead code:
zero xrefs, zero data refs), so the marked-instance walk
(`CEffectWorld_RenderAllMarkedInstances @ 0x5ab840` →
`CEffectInstance_RenderShadowVolume @ 0x5a9c60`) and the
`Light_IsSpotlight @ 0x5a9040` legs in `CRenderBatchQueue_FlushBatches`
never run in JO — model `LGHT` spot data (falloff, rotation, view_proj)
never reaches the runtime and every model light renders omni. The REAL
corona renderer is `EffectWorld_RenderLightCoronas @ 0x5aaf40` (ex
CEffect_RenderFoliageBillboards; the witness map lives on
engine/runtime/renderer/light_scene.h — three additive segments marching
toward the camera, the `texlightcrn` procedural radial, the 100-wu cull,
flag 512 disable, the owner visible-section gate).
`EffectWorld_ResetPoolAndInitLighting @ 0x5ab890` is
the mission-start reset (zero pool, identity ambient, `Lighting_InitTextures
@ 0x5a94f0`).

**Terrain/foliage lighting constants.** `Render_TerrainScene @ 0x610c80`
(per frame) calls `init_terrain_lighting_color_ramps @ 0x604ee0` with
**(Env_LightBlock[0], Env_SkyBlock[0])** — NVG blends the sky arg toward the
modulator (`@ 0x610d28..0x610e36`), the vehicle scope forces
`(0x101010, 0xF0F0F0)`. The ramps function stores **c1 = light ÷255
(`PolyTrn_PSConstC1_Light @ 0x31a182c`)** and **c0 = sky ÷255
(`PolyTrn_PSConstC0_Sky @ 0x31a183c`)**, seven VS ramp blocks of
{1,1,1,1} and {light·0.707 + sky} (the `Env_TerrainLightCombined` blend),
and the packed sun/blend ratio `PolyTrn_SunToBlendRatioColor @ 0x849930`
(consumed by the tile renderers). `terrain_setup_lighting_and_shader
@ 0x604420` pushes c0/c1 as PS constants and picks the PS variant
(camera below `Env_WaterHeightFixed` swaps the stage-3 dp3 INPUT to the
water-noise texture — the top tier KEEPS `PS14SplatNormalMap` with t3 = the
water noise, and the misleadingly named PSShadow* pair serves only the
partially authored ps.1.1 tiers (the 2026-08-13 selector decode; full
section in [terrain-re.md](../terrain/terrain-re.md)); tier ≥1 + normal
map → NM variants, splat textures → PS14Splat*); all eight terrain PS share
`r0 = ((t0.a·c1 + c0)/2) ×2 …` — **terrain light = tileAlpha × light +
sky** (the ÷2 and MODULATE2X cancel). `t0` is the cached tile render target,
not raw colormap RGBA. `PolyTrn_RenderTile @ 0x60da70` clears authored
colormap A in its `0x00808080` base draw; `Terrain_GenerateNormalMap
@ 0x603210` supplies A8R8G8B8 RGB `(grid X slope, grid Y slope, up)`/A128
(`0x603470..0x6034eb`), which the reimpl's `FORMAT_RGBA8` upload preserves
without a channel swap, and
`PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e` writes
`tileAlpha = saturate(4·dot(normalByte−0.5, lightByte−0.5))`.
`Environment_GetLightDirectionFloat @ 0x57d870` exposes the direct tuple
`g=(-fixed[1], fixed[2], fixed[0])/65536`; EnvFile preserves `g` unchanged, not
as Godot/world XYZ. `PolyTrn_RenderTile` takes stack fields
`outDir/var_28/var_24` and writes D3DCOLOR `BYTE2←g2`, `BYTE1←g0`,
`BYTE0←g1` (`0x60e201..0x60e331`), so GPU diffuse RGB is `(g2,g0,g1)`.
Terrain and the reimpl's analytic foliage t1 reconstruction now pack `(z,x,y)`.
The old `(x,z,y)` reimpl mapping swapped the horizontal DOT3 axes; flat
dawn/noon/dusk checks missed it, while the 08:00 non-flat oracle pins light
bytes `(231,83,187)` and slope alphas `0.8987774/0.0794002`. Retail foliage
does not repack this light: its blend PS consumes the already-composed cached
`t1.a`. The foliage/sector-model blend PS
(`rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8`) **inherits the same device
c0/c1** — `Foliage_SetupFarSlotDraw @ 0x6007c0` binds the PS without
touching the constants. The sector-model lightmap pass
(`Foliage_RenderFarPatches @ 0x609de0` — renamed 2026-08-15, ex render_terrain_lightmaps) bubble-sorts visible sectors
back-to-front, sets **D3D light 4 as a pure-ambient injector** (ambient =
detail-average × ramp — ×2 combined on the non-multitexture path — diffuse
0), and draws 4 quadrants per sector through the lightmap TILE cache
(`Terrain_FindSectorPatchRT @ 0x6042a0`; the planar world→tile-UV matrix stored @ 0x3266b88
feeds the wind VS's c7/c8 projection). The mission lightmap TGA
(`Terrain_LoadTileSetAtlas @ 0x604a90`: 64-px tiles, dims + reciprocals
@ 0x319f7b0..) is sampled by `render_foliage_billboards @ 0x607b30` and
`PolyTrn_RenderTile @ 0x60da70` via the view @ 0x319f7d4. The CPU-side
1024×1024 premultiplied colormap (`PolyTrn_ColormapPixels @ 0x319f79c`) is
sampled by `sample_terrain_colormap_tinted @ 0x606030`, but the fresh
2026-07-13 foliage trace follows the generated detail vertex to its final
write: that sampled packed diffuse is overwritten by the source-height bend
carrier before emission. It is therefore not a rendered instance-color
source. See the corrected D-FOLIAGE-1 disposition and
[foliage-re.md](../foliage/foliage-re.md).

**Underwater `PSShadow*` and static model shadows are different systems
(2026-07-29 correction).** `terrain_setup_view_and_lighting @ 0x60FE40`
stores `cameraY < Env_WaterHeightFixed` in view field `+0x74`
(`@ 0x60FEE0`), and `terrain_render_visible_sectors @ 0x6090C0` copies it to
`dword_319FB3C @ 0x60915F`. Under that flag,
`PolyTrn_BindStageTextures @ 0x604330` calls `sub_5C0190 @ 0x6043ED` and
binds its result, `Water_NoiseColorTexture @ 0x28EE8B8`, at t3; shader
selection occurs at `terrain_setup_lighting_and_shader @ 0x6044B1`.
Amended by the 2026-08-13 selector decode (the full section lives in
[terrain-re.md](../terrain/terrain-re.md)): the TOP tier keeps
`PS14SplatNormalMap` and merely swaps its stage-3 dp3 input to the water
noise; `PolyTrn_PSShadowBasic` / `PolyTrn_PSShadowNormalMap` (the
`4·t3²·t0.a` coefficient) serve only the partially authored ps.1.1 tiers.
Either way they are underwater animated wave-shadow/noise
variants, not geometry shadow-map receivers. `dword_319FB8C` is likewise not
shadow state: all four writers store one (`@ 0x6040DD`, `0x60910C`,
`0x60E89B`, `0x60EB25`), and `render_terrain_sector_batch` reasserts it at
`0x6092BE` before the read at `0x6095D7`; its live leg admits the ordinary
terrain/local-point-light and foliage work.

Static directional sector shadows instead originate in
`Terrain_CollectAndRenderTileModels @ 0x60D250`. With
`dword_8493DC` enabled, `PolyTrn_RenderTile` invokes it at
`0x60DC83..0x60DC96`. The collector derives a planar projection from
`Environment_GetLightDirectionFixed/Float @ 0x57D8E0/0x57D870`, admits pool-2
buildings unless either BMS/runtime or ItemDef `NoShadow` is set
(`@ 0x60D42F/0x60D43E`), and admits pool-1 items only when they also carry
ItemDef attrib2 `StaticShadow` (`@ 0x60D447..0x60D450`). It fills every ROBJ
matrix slot for the selected LOD (`@ 0x60D926..0x60D95E`) and submits flag
`0x2`/PROJSHAD at `0x60D971`; the live portal
`g_HiddenSectionMask @ 0xB7965C` is not part of this direct render-data path.

The silhouettes render into temporary RT `dword_319A2D8`
(`@ 0x60D5BE..0x60DA4F`) and are sampled into the destination terrain tile
cache at `0x60E10A..0x60E19D`. Terrain receives that cache as t0 and foliage
as t1, so both inherit the projected shadow while sector-model floors and
walls do not receive or self-shadow from it. Ihq01 is the concrete parity
case: pool-2 index 40, no `NoShadow`, three ROBJ in every LOD; all three cast,
despite ROBJ 1/2 being interior-lighting sections in the live model draw.
Dynamic people/ItemDef `DynamicShadow` render slots remain independent
(`Entity_InitFromModel @ 0x40E1BC..0x40E1F7`). Full tile-producer record:
[terrain-re.md](../terrain/terrain-re.md).

The reimpl now follows that receiver domain directly for its supported subset.
A typed static-source snapshot follows destruction/husk/editor transforms;
the page provider selects the retail LOD/ROBJ population, resolves supported
opaque/alpha-tested materials (including discrete `TEX_TEAM` frames), and
rasterizes only page A. Terrain and detail foliage then sample the same current-
frame cache binding, while sector-model floors remain outside the receiver.
The former terrain-only directional static-shadow light and black next-pass
catcher are retired. Exact general c7/c8 projection, unsupported animated or
skinned materials, one-sided/non-opaque overlap behavior, remaining ordered
contributions, and final cache cadence/edge/mip behavior remain D-TERRAIN-7.

**Lighting textures + the DOT3 light shader.** `Lighting_InitTextures
@ 0x5a94f0` builds the procedural set: `texlight2d`/`texlightspot2d` (64²
falloff via `Texture_GenerateProceduralFalloffTexture @ 0x5a92c0` modes 2/1),
`texlightspot1d` (64×8 cone-angle ramps), `texdepthgradw` (alpha ramp
col<<26) and `texdepthgradt` (soft depth window) — the DepthGrad pair the
AMODE_DEPTHTEST beam passes consume — `texlightcrn` (128² radial, black
borders), a 32³ volume falloff, mode-0x600/0x602 two-texture shaders, and —
device-gated ps.1.1 + ≥4 stages — the LAST embedded shader outside FrameFX:
`dp3_sat(t0_bx2, t1_bx2) × t2 × t3 × c0` (normal map · light-direction
texture × two attenuation textures × light color), built additive-opaque and
additive-blend. This is the per-pixel dynamic-light path — the
fixed-function era's per-pixel OmniLight.

**Cubemaps.** `init_render_textures @ 0x58f6e0` creates CubeEnvironment
(64/128/256 by quality `dword_8409E4`), CubeRotSpecular (always 128) and
CubeNormalize (64..256), then `Render_FillStaticCubemaps @ 0x58f290`
(cube-caps gated via `Render_DeviceCapsFlags @ 0x27213d4` bit 0x40, set by
the caps query `sub_676850`, disable mask `Render_ApplyCubemapCapsDisableMask
@ 0x5899e0`) fills CubeNormalize (`generate_normalmap_cubemap @ 0x685570`,
packed 128+127.5·n normals) and **CubeRotSpecular** via
`generate_cubemap_lighting @ 0x685bb0` — per-texel `Σ pow(max(0, N·L), exp)
× intensity × color` over an inline 2-light list: **white {255,255,255}
power 800 intensity 1.4 + warm {255,248,240} power 40** along −Z (light
records: dir float3, B/G/R bytes @ +12..14, intensity @ +16, power @ +20).
`MatRotSpecular` (built per batch in `apply_shader_parameters @ 0x58e14b`
from `build_direction_look_at_matrix(Environment_GetLightDirectionFloat())`)
rotates lookups so the glint tracks the live sun — the Glass.fx GLOW
technique's source, closing D-RORD-5's specular-cube question.
**CubeEnvironment** content is the LIVE SCENE: `update_environment_cubemap
@ 0x6106a0` re-renders all 6 faces once per 128 frames (immediately at
scene start / on force) at the player position clamped ≥ terrain+10 via
`EnvCube_RenderFaceOrAnalyticFill @ 0x58b220` → `GTexture_RenderCubeMapFace
@ 0x6864d0` with the scene callback. The same function's no-callback branch
holds an analytic 5-light sky fill (blue-from-above 0.3/pow2, warm ground
bounce 0.25/pow3, three warm pow-50/60 sun lobes) — **caller-less dead
code** in retail JO.

**The render-slot (character shadow) side** — witnessed, out of REN port
scope (ADR 0023 excludes the Shadow_/Scar_ family).
`RenderSlot_UpdateEntityLight @ 0x5d6a30` (ex `Entity_UpdateRenderState`)
updates a 128-byte slot record (`RenderSlot_Table @ 0x2be3d30`): default
light = the sun direction trio (`RenderSlot_DefaultLightDir* @ 0x2bebd68`),
then the brightest passing point light near the entity wins (luminance
0.3R + 0.6G + 0.1B over distance² attenuation), and the shadow anchor
marches from the entity along the light direction to the terrain
(`Terrain_GetHeightAtPosition @ 0x606720`), with a slot LOD 6..20.
`RenderSlot_SetupNextLighting @ 0x5d7250` (ex `setup_entity_render_lighting`)
pops the next pending slot (`RenderSlot_PendingList @ 0x2be3a98`), sets
D3DRS_AMBIENT white, and lights the slot render with either the entity's
attached light (D3D light 4 + luminance-weighted NEGATED colors
`(c+lum)/2 × −3` into PS c21..c23 — the shadow darkening math) or a white
directional with 0.75 ambient material.

## The ported chain (REN-5)

- `engine/formats/env`: `WeatherColorBlock::set_step_deltas` `[orig: @ 0x57d940]`;
  `ModulatorChain` (identity-snapped pair, witnessed tick order, 62-tick
  exposure chase). `engine/runtime/renderer/light_runtime`: `unpack_modulator_scale`,
  `WorldLightingInputs/Block` + `build_world_lighting`,
  `compute_entity_lighting`, `ff_vertex_light` + `kFFModulate2x`,
  `sun_visibility_factor`, `point_light_color`, `point_light_attenuation`,
  `terrain_surface_light` — all T1-pinned (section 5).
- Reimpl: `WeatherCore` ticks modulator2 → modulator → the four ported
  blocks and exposes `set_exposure_from_iris` (the outdoor sample) +
  `get_color_src_gain`; `Weather` re-targets the exposure each tick from
  the env's iris params and writes the gain back to `MissionEnvironment`
  (`opennova_color_src_gain` global + the object-material uniform).
- The checked-in object technique resources now implement the witnessed model:
  `pixel = tex × min(mix(HemiGround, HemiSky, N.y·0.5+0.5) +
  DirLightColor·max(0,N·L), 1) × 2`, SELFLUM = `tex ×
  min(ColorSrcGlobalGain,1) × 2`, uniform surface
  `u_hemi_sky_color/u_hemi_ground_color/u_dir_light_dir/u_dir_light_color/
  u_color_src_global_gain` (D-RMAT-5 closed in
  [render-material-re.md](render-material-re.md)).
- `terrain_lighting.gdshaderinc`: c0/c1 corrected to (sky, light) — the
  prior combined/fill pairing was a gobj-era stand-in. Its tile-alpha path also
  preserves EnvFile's direct retail getter tuple and applies the witnessed
  D3DCOLOR-to-GPU-RGB permutation `(z,x,y)` before byte quantization; analytic
  foliage uses the same correction (D-TERRAIN-10).

## D-RLIT divergence catalog

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RLIT-1 | Hosted weather runs the complete 16-block chain: modulator2/modulator plus all 14 color blocks | 16 blocks modulate in witnessed order (including skyfog and the static ceiling/cloud/floor trio) `[orig: @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-21)** — `SkyWeatherColorBlocks` preserves the witnessed order; skyfog gets its lightning additive, horizon blend, and tail double; and `MissionEnvironment` writes every color current back. `SkyDome` and the frame clear share the final doubled skyfog, the flat dome consumes smoothed `cloud_rgb`, and indoor iris samples consume the pre-modulated ceiling/floor currents. |
| D-RLIT-2 | Iris exposure uses the marched 3-point camera-ray average with per-sample indoor/outdoor classification and sun occlusion | 3-point average marched back from the camera-ray hit, with per-sample interior detection + 3 sun-occlusion raycasts `[orig: compute_ambient_light_along_direction @ 0x5c7a00; terrain_sector_compute_lighting @ 0x5c7550]` | **PORTED (2026-07-18, the marched-iris slice)** — the march, per-sample indoor/no-data classification (ceiling/floor blocks, gain-255 no-data short-circuit), sun level 8−hits (radii −0x2000/−0x5000/−0x8000), and INT /3 average are live in-world (`compute_iris_samples` → `set_exposure_from_iris_samples`; the sniper/aircraft retail A/B was the trigger — the retail hangar frame runs the indoor-dilated gain ≈ 71/64 vs the old outdoor 59/64). Bounded residuals: the camera-ray ENTITY nearest-hit clip (terrain clip only), pool-1 dynamics in the sun rays (statics walk only), the caller-sector entity-count ray gate (rays always run; identical when no statics exist), and the per-sample interior LIGHT-GROUP side effect (`Lighting_SetInteriorLightGroup @ 0x5a90e0` — rides D-RLIT-4's group hosting). Measurement note (2026-08-20): frozen render fixtures previously published the modulator's mission-reset IDENTITY gain (the live iris/exposure legs never ran at the frozen pose — `registered-2026-08-20` 03tr-sun-sky shows `source_gain [1,1,1]`); the capture refresh now stamps the marched samples and settles the chase at the fixture pose (`Weather.settle_exposure`, capture-seam only), so fixture states measure the settled gain (the 03tr hangar pose serves the indoor-dilated 76/64) |
| D-RLIT-3 | `items.def light_transfer` drives interior ROBJ sections plus the contained player/viewmodel, and drawn outdoor pool-0/pool-1 entities dim DirLightColor by the witnessed 3-radius sun query | interior-parented entities lerp to floor/ceiling ambience by the parent daylight openness (model+536). Eligible outdoor pool-0/pool-1 entities cast one 200-u sun segment at clip radii −0x2000/−0x5000/−0x8000 from position + collision-AABB midpoint, walking the entity's OWN `+0x1BC`/`+0x1C0` candidate slice (self excluded at slice build; only bubble-overlapping structures can block); each blocked cast steps DirLightColor 1.0→0.75→0.5→0.25; contained entities, empty-slice sources, and pool-2 statics stay 1.0 `[orig: setup_terrain_effect_for_entity @ 0x5c74a0; Entity_ComputeSunVisibility @ 0x5c6800; raycast_find_collision_entity @ 0x539a70 (the slice walk); Entity_BuildProximityListsFromPools @ 0x4b8eb0; the FP-pass discard @ 0x4deeb0]` | **FIXED (2026-08-18; slice-scoped 2026-08-19)** — interior transfer 2026-07-29; the outdoor feed 2026-08-18 (`CollisionWorld::sun_visibility_blocked_rays` + the `get_entity_sun_visibility_changes` diff walk + the presenter's local seam; live-probed on 00TRa: courtyard teleports report quality 2 beside structures). Bounded residuals: the joiner wire-present path keeps replicas at 1.0 (its own presenter, no sim entities), the AABB midpoint rides D-COL-3's unscaled bounds, the sun casts walk statics only (the same D-RLIT-2 posture — retail also tests slice DYNAMICS), and pool-1 rows our builder does not yet slice (non-vehicle dynamics: dropped items, crates) keep full sun until the builder's pool-1 leg grows |
| D-RLIT-4 | The portable EffectWorld core, decay lifecycle, safe opaque handles, mission-start/late-node model-light spawn, target-disable gates, four transient routes (2026-08-16), per-draw owner isolation (2026-08-18), and — 2026-08-20 — static-source owner scoping, VERTEX-rate point-light shading, and corona billboards are hosted: `LightScene::select_for_draws` snapshots live slots once, then per rendered model runs the witnessed slot-order first-64 collect + nearest sort + group-gated four-light select with the model's entity as owner scope; batched static sources spawn with a synthetic owner so subobject-attached records obey the same gate; object shaders evaluate the selected four in the VERTEX stage (`v_point_light_diffuse`) so the falloff Gouraud-interpolates like the D3D pipeline; `LightScene::collect_corona_quads` + `light_corona.gdshader` draw the witnessed three-segment additive corona march with the procedural `texlightcrn` radial, the owner visible-section gate (fed by the occlusion verdict masks), the flag-0x100 impact re-center, and the fog-to-black fold. The all-overlap camera query survives as the report/debug census only. | Retail spawns model and transient instances (owner = the spawning entity whenever the record's attach bone != 0 `[orig: Entity_SpawnGlowEffects @ 0x56c8ae]`), then each draw context queries nearest-64 and enables the first four group-passers as D3D VERTEX lights `[orig: collect_nearby_zones_by_aabb @ 0x5aa250 — hard 64 break @ 0x5aa384; Light_SelectAndEnableForDraw @ 0x5ab9d0 (ex collect_visible_foliage_slots) — LightEnable via vtable+0xD4; update_light_slots @ 0x5abc50 carries the same gate but has NO xrefs]`, and draws coronas per scene with the owner-section gate, the 0x100 re-center, and black-fogged additive blending `[orig: EffectWorld_RenderLightCoronas @ 0x5aaf40; Terrain_IsBuildingSectionBitSet @ 0x5c6960; CD3DDevice_SetFogAndBlendMode @ 0x677740]`. Spot/Target-cone delivery is DEAD CODE: `LightPool_SpawnSpotProjectorEffect @ 0x5a9fd0` is caller-less, so LGHT falloff/rotation/view_proj never reach the runtime and every model light is an omni. | **OPEN / PARTIAL DELIVERY** — remaining: interior groups (`Lighting_SetInteriorLightGroup @ 0x5a90e0`) still publish zero; batch draw contexts (a scoped static's light cannot reach its own batched building yet); terrain projected-light matrices/textures (`get_light_projection_info @ 0x5aa5c0` → `CRenderBatchQueue_FlushBatches @ 0x5d9f50`); foliage sampling; powerups; blink-box ownership; bone following; static-batch destruction/restore and husk `LGHT` rebinding; the ambient-scale source; and whether VS-technique batches consume the enabled D3D lights at shader_usage_level 2 (an .fx/FlushBatches lead). Generation leases intentionally reject retail's stale-handle write-through memory alias. |
| D-RLIT-5 | Glass/env reflection = the hemisphere sampled along the reflected view; phong specular = a pow-16 lobe in the witnessed light color | glass GLOW samples CubeRotSpecular (the static sun-glint cube) via MatRotSpecular; NORMAL techniques sample the LIVE CubeEnvironment scene cube; VS_PHONG* samples the PhongMap texture `[orig: @ 0x58f290; @ 0x6106a0; Glass.fx]` | OPEN (approximation) — the cube CONTENTS are witnessed (this record); hosting a live scene cube / the glint cube is the D-RORD-5 bloom-wiring residual's substrate |
| D-RLIT-6 | No baked mission lightmap TGA draped (the below-water terrain water-noise modulation FIXED 2026-08-13 via D-TERRAIN-8 — the top-tier dp3-input swap; terrain-re.md carries the selector decode) | camera-below-water terrain swaps the LIVE stage-3 input to `Water_NoiseColorTexture`; the mission lightmap TGA separately drapes tiles/billboards `[orig: below-water flag @ 0x60FEE0 → dword_319FB3C @ 0x60915F; live t3 slot swap @ 0x6043f2; selector @ 0x6044b1..0x604556; lightmap load @ 0x604A90]` | OPEN — narrowed to mission-lightmap hosting (terrain-record scope); static model sun shadows are the separate D-TERRAIN-7 tile-composition path |
Fixed 2026-08-20 (no ledger ID — witnessed and closed in one round, the
03tr-sun-sky fixture slice): several Godot device seams consumed the
render-float (D3D-world) celestial/light tuple AS IF it were Godot world —
the tuples differ by the x/z swap `godot = (z, y, x)_render` (mission
`(x, y, z)` -> render `(-y, z, x)` `[orig: Math_FixedPointToFloat3_YNegated
@ 0x611210]` vs the reimpl's mission -> godot `(x, z, -y)`; the water-glint
submit matrix `[orig: update_sun_glare @ 0x5ad1ba..0x5ad213]` independently
confirms the map). Every low-sun frame front-lit where retail backlights.
Fixed via `godot/src/env/env_axes.h` at each seam: the `MissionEnvironment`
direction getters (SunShadow emission, the iris march and entity
sun-visibility ray feeds ride them), the `EnvLightValues.dir` publication
(the object directional term), sun/moon/glare/glint body placement, the
glare rays + jitter plane, the sun-veil dot, dome `u_sun_dir`/`u_light_dir`,
and the star-instance placement. The raw tuple deliberately remains on the
`opennova_sun_direction` global and the terrain `u_sun_direction` uniform —
`terrain_lighting.gdshaderinc`/`foliage_detail.gdshaderinc` re-swizzle it
into the engine texture basis themselves and are byte-parity-verified in
that basis (the PolyTrn family), and on the star-field cull (engine-side
star directions share its axes).


| D-RLIT-7 | Static mission objects (the placer's MultiMesh batches) froze the env lighting harvested at load — the throwaway template's materials had no live owner, so TOD/weather/iris advances relit animated models but not the static world (the load-time snapshot even carried the pre-first-iris-tick modulator: gain 1.0 vs the settled 60/64) | retail relights EVERY entity from the current lighting block each frame `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0 ← CRenderBatchQueue_FlushBatches]` | **FIXED (2026-07-06, the model-parity slice)**: the placer registers every harvested batch ShaderMaterial and re-stamps them from the live env (`mission_object_placer.update_environment`, driven per frame by the container's `mission_batch_env_stamper`, generation-gated like the per-model stamp; values/push single-sourced as `ObjectModel.environment_values_from`/`apply_environment_values`); verified batch uniforms == live-model uniforms after settle (dir 159/255, gain 60/64) |
| D-RLIT-8 | The object per-material hemisphere mixed color spaces: `hemi_sky` came from `MissionEnvironment.get_sky_ambient()` = the RAW TOD keyframe (never smoothed, never iris-modulated) while `dir_color`/`hemi_ground` came from the smoothed+modulated weather writeback — off-noon the modulator brightens every block toward the exposure target but the un-modulated sky half stays dark (the sky-facing half of every building too dark at night; the terrain/foliage GLOBALS path was already correct via `get_smooth_sky()`) | retail feeds ALL entity lighting from the post-modulator block render colors — the world-block writer fills [8..10] ← `Env_SkyBlock[0]` ÷255 exactly like light/ground `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; the blocks smooth + modulate in the weather tick @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-06, the REN-6 session)**: the sky block joins the per-tick env writeback seam — `Weather` pushes `get_smooth_sky()` through the new `MissionEnvironment.set_sky_ambient_rt` (mirroring fill/sun/fog, generation-gated), `get_sky_ambient()` serves the smoothed current and re-seeds from the keyframe on discrete TOD recomputes (the `_fill_light` contract); GUT pins the seam (`env_parity_vectors_test.test_sky_ambient_serves_smoothed_writeback`); golden env grid/weather rows byte-identical (the grid collects bare env nodes; the weather checkpoints already read `get_smooth_sky`) |

## IDB changes made during the session

| Address | Old | New | Basis |
|---|---|---|---|
| 0x5a90e0 | sub_5A90E0 | Lighting_SetInteriorLightGroup | writes the interior entity+section pair @ 0x272ED84/80 the light culling tests |
| 0x5a9100 | sub_5A9100 | Lighting_SetOwnerLightGroup | writes the owner pair @ 0x272ED7C/78 |
| 0x5a9120 | sub_5A9120 | Light_PassesActiveGroups | accepts ungrouped lights, the interior group (section-matched) or the owner group |
| 0x5a8ae0 | sub_5A8AE0 | Light_TickGenBlock | ensures the light's RgbGen block is ticked before evaluation |
| 0x5aa450 | sub_5AA450 | Light_FillD3DPointLight | fills a D3DLIGHT9 POINT: color × ambient-scale × intensity × 1.5, atten {1,0,15/r²,1} |
| 0x5d89e0 | set_bounding_volume_from_box | RenderBatchCtx_StoreLightingConstants | qmemcpy of the 32-float block to ctx+112 + the two hemisphere averages; NOT a bbox |
| 0x5d8cb0 | setup_d3d_clip_planes | Lighting_SetHemisphereD3DLights | creates D3D lights 2/3 with the sky−avg / ground−avg delta colors; NOT clip planes |
| 0x5c6800 | sub_5C6800 | Entity_ComputeSunVisibility | 3 raycasts toward the light; (4−blocked)·0.25 |
| 0x5d7250 | setup_entity_render_lighting | RenderSlot_SetupNextLighting | pops the pending slot list; slot-render D3D lighting |
| 0x5d6a30 | Entity_UpdateRenderState | RenderSlot_UpdateEntityLight | the shadow-slot dominant-light pick + terrain anchor march |
| 0x604ce0 | sub_604CE0 | Terrain_LoadScorchTextures | trscrch1-3.tga + qburn01.tga (the brief's "4 lightmap materials" guess was wrong — scorch decals) |
| 0x606030 | sample_terrain_lightmap | sample_terrain_colormap_tinted | samples the CPU colormap (not the lightmap TGA) × tint >>7; foliage instance colors |
| 0x58f290 | sub_58F290 | Render_FillStaticCubemaps | fills CubeNormalize + CubeRotSpecular at init (caps-gated) |
| 0x5899e0 | sub_5899E0 | Render_ApplyCubemapCapsDisableMask | clears Render_DeviceCapsFlags bits from the disable mask |
| 0x58b220 | generate_sky_cubemap | EnvCube_RenderFaceOrAnalyticFill | callback → live scene face render; no callback → the dead analytic 5-light fill |
| 0x2732e28 | unk_2732E28 | Light_InstanceTable | 176-B light records, handle-indexed |
| 0x272ed84/80 | dword_* | Lighting_InteriorGroupEntity/Section | the sample position's interior pair |
| 0x5a9fd0 | (undefined code) | LightPool_SpawnSpotProjectorEffect | defined 2026-08-20: the spot/projector spawner (flag 0x10000, projection matrix at instance bytes 88..152) — CALLER-LESS dead code in Jointops.exe |
| 0x5aaf40 | CEffect_RenderFoliageBillboards | EffectWorld_RenderLightCoronas | it walks Light_InstanceTable drawing additive corona billboards, not foliage |
| 0x5ab9d0 | collect_visible_foliage_slots | Light_SelectAndEnableForDraw | the LIVE per-draw light select: fills Light_VisibleHandles/Count and D3D-enables the first <= 4 (update_light_slots @ 0x5abc50 is the xref-less standalone variant) |
| 0x5a9280 | sub_5A9280 | Light_SetDisabledForPass | cubemap-face renders set 1 (no dynamic lights/coronas), Render_TerrainScene resets 0 |
| 0x2732db8 | dword_2732DB8 | Light_CoronaShader | the 1-texture additive corona shader over Light_TexCorner ("texlightcrn") |
| 0x2732de8 | dword_2732DE8 | EffectWorld_LightsDisabledForPass | the pass gate above |
| 0x2732de4 | dword_2732DE4 | Light_InstanceHighWater | the pool high-water count |
| 0x272ed7c/78 | dword_* | Lighting_OwnerGroupEntity/Section | the rendered entity's pair |
| 0x272ed98 / 0x272eda0 | dword_272ED98 / effectHandle | Light_VisibleCount / Light_VisibleHandles | the per-frame in-frustum light list |
| 0x840b20 | dword_840B20 | Light_D3DIndexBase | D3D light index base for dynamic lights |
| 0x2732dfc / 0x2732e00 | dword_* | Light_ActiveD3DList / Light_ActiveD3DCount | the ≤4 enabled-light shortlist |
| 0x8437e0..e8 | dword_8437E0.. | g_DefaultLightDirX/Y/Z | {0, 1, 0} — D3D light 0 direction fallback |
| 0x2be3d30 | unk_2BE3D30 | RenderSlot_Table | 128-B shadow-slot records |
| 0x2be3a90/94/98 | dword_* / frameState | RenderSlot_PendingCursor/Count/PendingList | the slot iteration state |
| 0x2be3c94 / 0x2be3a48 | dword_* | RenderSlot_TextureTable / RenderSlot_Shader | slot render targets + shader |
| 0x2bebd68..70 | outDir / dword_* | RenderSlot_DefaultLightDirX/Y/Z | the sun default for slot lighting |
| 0x31a182c / 0x31a183c | flt_* | PolyTrn_PSConstC1_LightColorR / PolyTrn_PSConstC0_SkyColorR | the terrain PS constants (c1 = light, c0 = sky) |
| 0x849930 | dword_849930 | PolyTrn_SunToBlendRatioColor | packed sun/combined ratio for the tile renderers |
| 0x319f7d0/d4, 0x319f7b0..bc | dword_* | Terrain_TileSetTexture/TexView, Terrain_TileSetAtlasWidth/Height/TilesX/TilesY | the terrain tile-set overlay atlas (`Bms_TileSetName` + `.TGA`; renamed 2026-08-20 from the `Terrain_Lightmap*` misnomers — see tiles/til-re.md, tile-set atlas source) |
| 0x319f79c | dword_319F79C | PolyTrn_ColormapPixels | the CPU 1024×1024 premultiplied colormap |
| 0x2721364..78 | dword_* / srcHeight | Render_Clip1DTexture/PhongMapTexture/AngleMapTexture/CubeNormalizeTexture/CubeRotSpecularTexture/CubeEnvironmentTexture | the shared texture set (slots 196-206); `srcHeight` was a kong misnomer on the ENV CUBE |
| 0x2732db0..e0 | dword_* | Light_DOT3PixelShader, Light_Tex{Light2D,Spot2D,Spot1D,DepthGradW,DepthGradT,Corner} | the Lighting_InitTextures resource set |
| 0x27213a0 / 0x27213d4 | unk_/dword_ | Render_DeviceCapsBlock / Render_DeviceCapsFlags | the caps block the cube fills gate on |
| 0xb7654c / 0xb76550 | dword_* | g_NVGActive / g_NVGBrightnessLevel | the NVG state (written by Player_ToggleWeaponScope / input bindings) |

## Open questions

- `dword_A890C8` (the NVG-branch suppressor read beside `g_NVGActive`) — a
  view-mode byte written by the player camera path; identify at the
  viewmodel/scope slice.
- The interior daylight-openness float's WRITER (interior model data +536 —
  the reader side is witnessed at three sites); world-record scope with the
  interior system.
- `Color_UnpackToFloat4 @ 0x578900` reads the NVG pair — the sky-dome color
  unpack's NVG dim; env-record scope when the dome NVG look lands.
- The PhongMap texture content (`Render_PhongMapTexture @ 0x2721368` — filled
  where?) — the VS_PHONG* specular lookup (D-RLIT-5's phong leg).
