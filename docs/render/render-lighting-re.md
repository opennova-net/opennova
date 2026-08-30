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
`godot/src/env/weather_core.cpp` (the ticked chain),
`godot/src/env/{weather,mission_environment}.cpp` +
`godot/src/object/object_model.cpp` (the uniform feed),
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
| Iris auto-exposure sampling | MATCHING (full marched port) | the curve was already ported (env record §Iris); the 3-point camera-ray march is complete as of 2026-08-22: `Simulation::compute_iris_samples` clips the 8 u camera ray against terrain and the local player's candidate solids, samples thirds, classifies blink interiors only through that same fixed candidate slice, preserves the per-sample interior-group state transition, and casts all three outdoor rays against both pool-1 and pool-2 slice entries behind the local-player count gate; `WeatherCore::set_exposure_from_iris_samples` applies ceiling/floor indoors, ×level/8 sun outdoors, and INT /3 average `[orig: Environment_ApplyFogAndAmbient @ 0x57e440; compute_ambient_light_along_direction @ 0x5c7a00; terrain_sector_compute_lighting @ 0x5c7550; raycast_entity_collision @ 0x413760; raycast_find_collision_entity @ 0x539a70]`; the outdoor sample stays the no-world fallback |
| World lighting block (per-pass build + ctx store) | MATCHING (math ported) | `renderer::build_world_lighting` `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; RenderBatchCtx_StoreLightingConstants @ 0x5d89e0]` incl. the NVG hemi rewrite, vehicle-scope grey, NVG world dim, and the two hemisphere averages; `renderer_state_vectors` section 5 |
| Per-entity uniforms (slots 227-230) + interior daylight lerp | MATCHING (math ported; reimpl transfer wired; delivery shape ported 2026-08-26) | `renderer::compute_entity_lighting` `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`; the aux float = the parent interior's daylight openness (model+536), NOT a dual-LOD fade. The reimpl now parses `items.def light_transfer` as a clamped percentage, carries it through `ItemDatabase`, and applies the normalized value to interior ROBJ sections and contained player/viewmodel lighting. Delivery follows retail's split: the world block is the per-pass constant set (`opennova_light_block_*` global shader parameters written by `MissionEnvironment` once per env change, the `RenderBatchCtx` block), and the per-entry factors (effectScale, interior flag, daylight t) are one `u_entity_light` instance uniform stamped on the entity's surface instances on change; `shared.gdshaderinc` evaluates the same lerp/scale per draw, with `renderer::compute_entity_lighting` as the pinned oracle. No material carries lighting or fog state and nothing restamps per frame |
| Object NORMAL-pass lighting | MATCHING except named cube-content residual | Fixed function: `renderer::ff_vertex_light` + checked-in `fixed`/`flag` techniques reproduce ambient + directional + hemisphere delta lights and selected point lights at vertex rate, saturated then MODULATE2X `[orig: Lighting_SetHemisphereD3DLights @ 0x5d8cb0; D3D light 0 @ 0x5d9ce2..0x5d9d76; _FFP.fx TBoringFFP]`. Highest-quality DOT3/Phong effects retain their authored mapped-normal pixel N.L/N.H. Source-specific vertex factors stay distinct: ordinary bump/Phong effects carry attenuation × geometric self-shadow, BDiffT2's point stage carries attenuation only, and SkBDiffT2/SkBDiffO2 also use geometric/Gouraud hemisphere with no directional self-shadow `[orig: _vsDiffT/O, _vsPhongT/O, _vsSkDfT/O, _vsSkPhT/O; BDiffT2.fx; SkBDiffT2.fx; SkBDiffO2.fx; _psDiff.fx; _psPhong.fx]`. The 24-technique decoded-source ledger and raster response matrix are `object/technique_validation.json` + the `render_swatch` probe. |
| Per-entity sun visibility (effectScale source) | MATCHING (all roles, 2026-08-23) | `renderer::sun_visibility_factor` `[orig: Entity_ComputeSunVisibility @ 0x5c6800; stack write @ 0x5c7fa5]`; local/authority rows use `world::CollisionWorld::sun_visibility_blocked_rays`, while decoded rows use `wire_sun_visibility_blocked_rays` over a separately wire-keyed candidate arena rebuilt on the same 17-tick edge. Both cast the witnessed one-segment/three-radius query from the exact scaled bbox midpoint and only against the source's own `+0x1BC`/`+0x1C0`-equivalent slice. `Simulation::get_draw_lighting_changes` emits typed identity triples; `WirePresentPass::set_entity_lighting_context` caches the factor across cold body/held-weapon construction. The local player's third-person body dims while FP parts keep the witnessed effectScale=1 exemption (D-RLIT-3). |
| EffectWorld dynamic point lights (instance pool, spawn/query/select, color × modulator × RgbGen, {1,0,15/r²,1}, ≤3 per batch entry, owner/interior groups, fade/decay lifecycle, transient spawners, coronas) | **MATCHING on the locked highest-quality path (≤3 per strip) (2026-08-23; Spot/Target and foliage delivery premises witnessed DEAD)** | `renderer::LightScene` hosts the 4096×176B pool, safe generation leases, target-disable gates, nearest-64 query, group-passing nearest-THREE select (the batch-entry cap; the 4 of `Light_SelectAndEnableForDraw` is the transient D3D enable count), and per-draw delivery `[orig: Light_InstanceTable @0x2732e28; collect_nearby_zones_by_aabb @0x5aa250; Light_SelectAndEnableForDraw @0x5ab9d0; collect_render_objects_for_batch @0x5d9229; CRenderBatchQueue_FlushBatches @0x5da26b/@0x5da5de]`. `EffectLightDirector` routes mission-start and powerup-respawn LGHT plus four transient families. Authored positions use the entity matrix once and remain spawn-fixed; `subobject` selects an owner section only. The final spawned handle shares entity+0x1B4 with muzzle glow, a husk swap does not rescan, and entity removal clears only that final handle `[orig: Entity_SpawnGlowEffects @0x56c82d..0x56c92c; powerup callback @0x442b40/@0x442ba0 registered @0x442ce6; Entity_UpdateMuzzleGlowEffect @0x56c960; Entity_Destroy @0x43e903..0x43e916]`. BUILDING ObjectModels and static atlas rows retain exact per-ROBJ bounds/owner sections; static props carry containing-blink groups. Native/GUT tests pin selection, isolation, lifecycle, and replacement; the 24-technique Forward+ D3D12 probe proves atlas/live raster identity. Coronas and terrain projected lights are ported end to end `[orig: EffectWorld_RenderLightCoronas @0x5aaf40; Light_SetupTerrainProjectedPass @0x5aa830]`. The apparent foliage leg is inert in retail's max-quality program: the call at `Foliage_RenderFarPatches @0x60a5dc` enables D3D lights, but `Foliage_WindSwayVS` declares no normal/light input and writes `oD0=c6`, leaving no consumer outside the failed-VS fixed-function fallback `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupFarSlotDraw @0x60087a]`. |
| Model-authored `LGHT` chunks | **MATCHING on the runtime path (2026-08-23)**: consumed at spawn via EffectWorld, never via per-material uniforms | `Entity_SpawnGlowEffects @0x56c7c0` walks the model's light array (count +0xC4, records +0xC8, stride 120) and spawns one instance per record: entity-matrix position, white base color, radius = atten_end × 65536, RGB-gen block, subobject/blink owner group, and disable flags 512/1024/2048 `[orig: @0x56c82d..0x56c92c]`. There is no authored-light position update caller and no husk-model rescan. The per-material `u_local_light_*` path has no gameplay caller (`u_local_light_count = 0`); gameplay illumination flows through `renderer::LightScene`. |
| Terrain surface c0/c1 | MATCHING (ported) | c0 = SKY block, c1 = LIGHT block (both [0] ÷255): `renderer::terrain_surface_light`, `terrain_lighting.gdshaderinc` corrected from the gobj-era combined/fill guess `[orig: terrain_setup_lighting_and_shader @ 0x604420; init_terrain_lighting_color_ramps @ 0x604ee0 ← Render_TerrainScene @ 0x610c80]` |
| Dynamic projected entity shadows | WITNESSED / reimpl-native approximation | Retail allocates the independent projected render-slot path for people (and the local player) or ItemDef `DynamicShadow`; attached third-person weapons join their entity, while the first-person viewmodel does not cast. ItemDef `NoShadow` does not gate this path. Mission placement carries that admission policy (the aspirational streamed-model resolver twin was deleted 2026-08-11 — unreferenced since birth) `[orig: Entity_InitFromModel @ 0x40E1BC..0x40E1F7; GUT: mission_object_placer_test, object_model_runtime_gate_test]` |
| Static sector/model sun shadows onto terrain/foliage | WITNESSED / hosted page-alpha subset | pool-2 buildings cast unless `NoShadow`; pool-1 items additionally require `StaticShadow`; every ROBJ in the selected LOD enters a black PROJSHAD temporary RT which is composited into terrain-tile alpha, not back onto sector models. Runtime now collects typed static sources, resolves selected LOD/all-ROBJ geometry and every diffuse-alpha frame, evaluates AlphaGen/full UV transforms and time/control flipbooks through the ordinary-object runtime functions, rasterizes A-only projections into the shared terrain/foliage page, and retires the global directional surrogate. The max-quality c7/c8 projection, skinned rigid collapse, material pass state, animation evaluator, and final ONE/ONE composite are exact `[orig: c7/c8 @ 0x60A220..0x60A34F; Terrain_CollectAndRenderTileModels @ 0x60D250; submit tick @ 0x5DAD9D; batch CTRL snapshot/restore @ 0x5D91AB..0x5D91DE / 0x5DA1B8..0x5DA1FD; material consumer @ 0x58DB80; PolyTrn_RenderTile composite @ 0x60E0C6..0x60E19D]`; scorch/order and cache cadence/edge/address/mip tails remain D-TERRAIN-7, while whole-process CTRL/RNG ordering remains D-3DI-2 |
| Foliage/sector-model lighting constants | MATCHING (witnessed; values and max-quality non-response pinned) | the blend PS inherits the terrain device's c0/c1 and consumes cached-tile `t1.a*c1+c0`; `Foliage_WindSwayVS` writes `oD0=c6` and declares no normal/light input. The per-patch `Light_SelectAndEnableForDraw @0x60a5dc` call therefore mutates state with no consumer while that VS is bound; only the excluded failed-VS FVF fallback could consume it `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupFarSlotDraw @0x60087a; Foliage_RenderFarPatches @0x609de0]`. |
| Lighting textures + DOT3 dynamic-light shader | witnessed / reimpl-native equivalent | procedural falloff set + the last embedded PS outside FrameFX `[orig: Lighting_InitTextures @ 0x5a94f0]` — the ps.1.1 DOT3 per-pixel light is the fixed-function era's OmniLight; the reimpl's real per-pixel lights serve the intent |
| Cubemap and Phong lookup sources | MATCHING | the highest-quality 256² CubeEnvironment shell synchronously re-renders the exact sky + sun/moon callback on all six faces every 128 frames, applies the 0x60 gamma-byte dim (the faces reach the published RD cubemap through a RenderingDevice copy leg, no CPU readback), and adds the rotated static lobe `[orig: init_render_textures @ 0x58f6e0; update_environment_cubemap @ 0x6106a0; callback @ 0x5c3700]`; the Forward+ D3D12 axis/orientation/byte probe validates the final samplerCube. Static sun-glint cube = white pow-800 + warm pow-40 along −Z, rotated by MatRotSpecular `[orig: Render_FillStaticCubemaps @ 0x58f290 → generate_cubemap_lighting @ 0x685bb0]`; normalization cube `[orig: generate_normalmap_cubemap @ 0x685570]`; `Render_CreateSystemTextures @ 0x58aca0` creates the exact 256×256 `gsys_phong` lookup with N.H exponents 4/16/64 in RGB and N.L in alpha. |
| Render-slot (entity ground shadow) pipeline | **PORTED with open 03TR low-sun defect (2026-08-24)** | the full slot family is witnessed and hosted: frame-open sun default with the 0.25 vertical clamp then negation `[orig: render_shadow_pass @ 0x5d7b70]`, slot registration + LOD `[orig: RenderSlot_AllocSlot @ 0x5d5690]`, priority scoring / 24-patch / 12-RT assignment `[orig: RenderSlot_SortAndAssign @ 0x5d6530]`, RT size chain `[orig: RenderSlot_InitTextureChain @ 0x5d5320]`, dominant-light pick + anchor march `[orig: RenderSlot_UpdateEntityLight @ 0x5d6a30]`, slot render lighting `[orig: RenderSlot_SetupNextLighting @ 0x5d7250]`, refresh cadence `[orig: RenderSlot_RenderEntityAndChildren @ 0x5d7690]`, and the terrain drape + authored blob decal `[orig: RenderSlot_DrawAllDrapes @ 0x5d6e20; RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0; RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]`. Planning/color laws portable in `engine/runtime/renderer/render_slot_shadow` (ctest `renderer_render_slot_shadow`) with the PROJSHAD coverage and blend-state tables in `object_shader_template` (ctest `renderer_material_classify`); the device (since 2026-08-29) is `godot/src/env/slot_shadow.cpp` publishing one typed request per armed slot, `godot/src/render/slot_capture_adapter.cpp` drawing them as a PRE_OPAQUE RenderingDevice pass into the twelve resolve targets (the retail white clear, black under the technique's coverage and blend state, GPU bone-palette skinning) and `godot/shaders/slot_shadow_drape.gdshader` sampling them (GUT `slot_shadow_test` headless + windowed, `sun_shadow_test`). This supersedes the earlier ADR-0023-era "Shadow_/Scar_ family exclusion" note for the RenderSlot_* half; the Scar_ decal family remains out of REN scope. Open symptom: the 03TR dawn M939 drape has the same excess tail at rest and visibly flickers under lateral camera movement; see the note below. |

**2026-08-22 object-point-light correction.** The broad "point lighting is
vertex-rate" wording in the historical EffectWorld row applies only to fixed
function. The highest-quality `.fx` NORMAL passes settle the previously open
VS-technique question: DOT3/Phong techniques run one authored point pass per
selected light (`PASSRULE_ONCE_PER_POINTLIGHT`). N.L/N.H uses the mapped normal
in the pixel stage; `CalcPointLightAttenuation` and `CalcSelfShadowTerm` run in
the vertex shader and interpolate in oD0. OpenNova now preserves that split
with `v_point_light_diffuse` for fixed function and
`v_pixel_point_factor` for pixel-shader effects that author geometric
self-shadow. BDiffT2's fixed-function point pass and SkBDiffT2/SkBDiffO2 use
the separate `v_pixel_point_attenuation` path with no self-shadow; the skinned
DOT3 base hemisphere is geometric/Gouraud while unskinned BDiffT2's is mapped.
Flag and both Glass NORMAL
techniques have no point pass; the per-technique response ledger/probe rejects
inventing one.

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
- **Per sample** (`terrain_sector_compute_lighting @ 0x5c7550`): its first
  argument is the **local-player entity**, not a terrain sector (the caller
  loads `g_local_player_entity` and pushes it at `Environment_ApplyFogAndAmbient
  @ 0x57e51d`). The function reuses that entity's fixed
  `+0x1BC`/`+0x1C0` candidate slice for every sample and tests only type-5
  building entries
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
  the local player's candidate count (`entity[112]`, byte offset `+0x1C0`,
  @ 0x5c7707 — with an empty slice the rays cannot hit); directional = light
  block [1] × level/8 (`light_scale = level × 1/2040 @ 0x5c77e9`), sky/ground
  = sky/ground [1]; light group cleared (0, 0). Then the iris curve
  ([env-tod-re.md](../env/env-tod-re.md) §Iris auto-exposure; luminance
  weights 0.25/0.5/0.25).

- **The candidate-scoped sun walker** (`raycast_find_collision_entity
  @ 0x539a70`, the function all three outdoor casts — and
  `Entity_ComputeSunVisibility`'s three — call; witnessed 2026-08-23):
  `(entity_a, entity_b, start, end, radius, allowAllTypes)`. It builds the
  radiused ray context first and returns CLEAR for a degenerate segment
  (`Physics_RaycastIntContext @ 0x539a8a`, < 16 raw → 1 @ 0x539a96); resolves
  each entity's mount chain (+0x268 vehicle mount, then +0x16C parent
  @ 0x539aba..0x539b10) and picks the first of entity_a, entity_b, parent_a,
  parent_b that carries a nonzero `+0x1C0` candidate count (`@ 0x539b18..
  0x539b54`, none → CLEAR); then walks ENTITY_A'S OWN slice regardless —
  `[edi+0x1BC]` entries bounded by `[edi+0x1C0]` (`@ 0x539b5d..0x539b6b`,
  loop bound `@ 0x539bbe`) — so the iris casts (entity_a = entity_b = the
  local player `@ 0x5c777e/@ 0x5c777f`) and the per-entity sun casts test
  only solids whose inflated bubble overlapped the SOURCE on the last 17-tick
  slice build. Per candidate ENTITY pointer: skipped when its +0x28
  owner-link is entity_a (`@ 0x539b77..0x539b7c`); must carry an ItemDef
  (+0x20, `@ 0x539b7e..0x539b83`); a candidate owner-linked to entity_b with
  def attrib 0x20 and raw type != 1 is skipped (`@ 0x539b85..0x539b98`, the
  attached-weapon waiver); unless `allowAllTypes` only raw type 5 is tested
  (`@ 0x539b9a..0x539ba2` — every lighting caller pushes 1, so pool-1
  dynamics and pool-2 statics both block); `raycast_against_entity_pool
  (candidate, 0, 1, ctx, allowAllTypes) @ 0x539baf` returning 0 (obstructed)
  returns 0 = BLOCKED immediately (`@ 0x539bb7..0x539bb9`), otherwise the
  walk continues and a clean walk returns 1. The three iris casts sit at
  `@ 0x5c7765..0x5c77c6` with clip radii pushed as −0x2000 (`@ 0x5c7767`),
  −0x5000 (`@ 0x5c7792`), −0x8000 (`@ 0x5c77ac`); each `neg/sbb/add 1`
  converts a 0 return into one decrement of the level 8 (`@ 0x5c7789..
  0x5c779c`, `@ 0x5c77b6..0x5c77c4`, `@ 0x5c77ce..0x5c77d5`). Port:
  `CollisionWorld::candidate_segment_hits_solid` / `candidate_slice_segment_hits_solid`
  (the slice walk, owner-link skip, Flags&1 / 0x8000000 skips, bound broad
  phase, then the section-local convex clip) consumed by
  `Simulation::compute_iris_samples` and `sun_visibility_blocked_rays`.

Reimpl: `Simulation::compute_iris_samples` (sampling; Godot camera →
mission fixed) + `CollisionWorld::clip_segment_to_nearest_collision`,
`query_candidate_blink_boxes_at_point`, and
`candidate_segment_hits_solid` (the candidate-scoped ray primitives) +
`WeatherCore::set_exposure_from_iris_samples` (per-sample curve + /3
average), stamped per render frame by `GameWorld._stamp_iris_samples` and
consumed by `Weather`'s per-tick re-target; the pure outdoor sample
remains the no-world fallback.

**The gain unpacks.** `Render_UnpackModulatorToLightScale @ 0x58db30` writes
`Render_LightScaleR/G/B @ 0x8409f4..fc` = modulator bytes ÷ 64; its SOLE
consumer is `apply_shader_parameters @ 0x58e05d` binding **ColorSrcGlobalGain
(handle slot 232)** — in the shipped `.fx` corpus the SELFLUM emissive
(`SelfLumColor × ColorSrcGlobalGain`). `EffectWorld_UnpackModulatorToAmbientScale
@ 0x5aaef0` writes `EffectWorld_AmbientScaleR/G/B @ 0x840b24..2c`, consumed
by `Light_GetPointLightParams @ 0x5a9180`, `Light_FillD3DPointLight
@ 0x5aa450`, `Light_SetupTerrainProjectedPass @ 0x5aa830` (ex `render_foliage_instance`, renamed 2026-08-21 — the per-light terrain projected pass, its argument a `Light_InstanceTable` slot),
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
`+0x1BC` arena block bounded by the `+0x1C0` count, so only pool-1/pool-2
solids whose inflated sphere overlaps the entity's bubble ever block; a far building's
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
OED preview emits (`PrepareLightParams @ 0x46A500` in ModSuperOed.exe). `Light_FillD3DPointLight
@ 0x5aa450` fills a D3DLIGHT9 (type POINT) with the same values and a
**1.5× diffuse boost**. Group culling: each light carries an owner pair
(entity ptr + section at record +76/+80); `Light_PassesActiveGroups
@ 0x5a9120` accepts lights owned by the sample position's interior
(`Lighting_SetInteriorLightGroup @ 0x5a90e0`, section-matched) or by the
entity being rendered (`Lighting_SetOwnerLightGroup @ 0x5a9100`);
`Light_SelectAndEnableForDraw @ 0x5ab9d0` (`update_light_slots @ 0x5abc50` is
its xref-less twin) D3D-enables at most **4 concurrent D3D lights** (indices
offset by `Light_D3DIndexBase @ 0x840b20`), maintaining the active list
@ 0x2732dfc — but that enable set is NOT what a lit strip consumes (next
paragraph).

**The per-draw select is per ENTITY, the lit count is THREE (witnessed
2026-08-23).** Both sector walks build the query box from the drawn entity's
own bound — min/max = position ∓/± `boundRadius` (entity+0) per axis — and
hand it to the live select `[orig: setup_terrain_effect_for_entity
@ 0x5c74fb..0x5c753a; Terrain_RenderSectorModels @ 0x5c5ea6..0x5c5ee4 →
@ 0x5c5f12 / @ 0x5c602e]`. `Light_SelectAndEnableForDraw` runs the
`collect_nearby_zones_by_aabb` overlap + nearest bubble sort into
`Light_VisibleHandles`/`Light_VisibleCount` (≤ 64) and then LightEnables the
first ≤ 4 `[orig: the > 4 clamp @ 0x5abbeb; LightEnable(base + slot, 1)
@ 0x5abc1a]`. The lights a strip actually receives come from the BATCH
ENTRY: each collector re-walks the sorted visible list in order, gates
`Light_PassesActiveGroups @ 0x5a9120` and the objects-enable flag (render
flag 0x800 clear, `@ 0x5a9010`, called `@ 0x5d920c` / `@ 0x5d96cc`), and
stores AT MOST THREE handles in entry dwords 5..7 with the count in dword 8
`[orig: collect_render_objects_for_batch @ 0x5d91e0..0x5d9229 — `cmp esi, 3;
jge` @ 0x5d9226..0x5d9229; the twin in collect_render_batches_for_entity
@ 0x5d96a4..0x5d96e9]`. `CRenderBatchQueue_FlushBatches @ 0x5d9f50` walks
exactly those three slots (`X[2] = 3` @ 0x5da26b, splitting spot vs point),
pushes the point survivors as `PointLightCoordArray`/`PointLightColorArray`/
`PointLightAttenArray` with `CurNumPointLights` = the entry count for the
shader pass `[orig: Light_GetPointLightParams @ 0x5da6a8; the ID3DXEffect
count-setter vtable call @ 0x5da6ed; the three vector-array vtable calls
@ 0x5da71a/@ 0x5da740/@ 0x5da766 through the handles stored
@ 0x5af51b..0x5af566]`, and for the fixed-function pass first disables EVERY
enabled D3D light (`CEffectWorld_ClearActiveSamplerStates @ 0x5da5de`) and
re-enables only the entry's (`Light_ApplyAsD3DLight @ 0x5da61a`). So the
highest-quality object path lights with at most THREE dynamic lights per
strip, the 4-light enable never reaches a shader-lit strip, and the per-ROBJ
step is a re-GATE of the entity's ordered list, never a re-collect
(`Lighting_SetOwnerLightGroup(0, robjIndex) @ 0x5d8ff7` only moves the
owner-group section between the walks). Port: `renderer::kLightSelectLimit
= 3` (`LightScene::kSelectLimit`), `LightScene::select_for_draws`; pinned by
`renderer_light_scene` (cap, pre-cap disable, entity-box + per-ROBJ gate) and
`effect_light_world_test`. The object shaders' fourth `u_point_light_*` slot
and the static atlas's fourth pair are now permanently zero — a dead slot,
not a fourth light.

These records ARE fed from model `LGHT` at spawn (2026-08-16 correction; the
old +0xCC xref audit checked the wrong field): `Entity_SpawnGlowEffects
@ 0x56c7c0` walks the model's post-load light array (count at +0xC4, records
at +0xC8, stride 120) and spawns one pool instance per record — the mission
start walks pools 1..2 (`Game_StartMission @0x525d19 →
Game_SpawnAllEntityGlowEffects @0x5227b0`). The only other call site is the
`powerup_respawn` callback `@0x442b40/@0x442ba0`, installed by
`PowerUpDef_RegisterNewEntry @0x442ce6`; there is no destruction/husk call
site. The flicker chain closes
RQ-2: reading an instance with a gen block runs `Light_TickGenBlock
@ 0x5a8ae0`, which hashes the light's fixed position into the weather wave
ring and writes the GLOBAL FLICKER control register (0x83FD00 = value slot
0x83FCE8 + 8·ordinal 3), so style-113 records interpolate start→end by a
position-phased ring sample. The per-material `u_local_light_*` uniforms have
no gameplay caller; gameplay illumination flows through the pool.

**Lifecycle and the transient spawners (2026-08-16, routed; delivery partial).** Record dword
15 is a FADE MODE, dwords 16/17 the countdown (current/initial), float 14
the blend the params multiply as `intensity`. The per-frame tick
(`EffectWorld_TickInstancesAndLightScale @ 0x5aa170`, called from
`Game_ProcessMainFrame @ 0x5267a1`) decrements a positive counter; at
expiry, mode 5 hides the slot (flag bit 2 — the query-skip bit;
`CEffectInstance_SetBlendAmount @ 0x5a8ee0` ≥ 0.001 re-shows it) and every
other mode memsets the slot dead; while counting, modes 2/5 render
`blend = d16/d17`. The same tick's tail `@ 0x5aa1ef..0x5aa23f` unpacks
`Env_TerrainColorRecip` bytes × 1/128 into the SEPARATE `flt_2732DAC/DA8/DA4`
triple that only the terrain projected pass consumes — it does NOT derive
`EffectWorld_AmbientScale`, which is the modulator colour × 1/64 from
`EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaf1d..0x5aaf37` (called from
`Environment_ApplyFogAndAmbient @ 0x57e464`), the gain the reimpl already feeds
(the earlier "the reimpl feeds the env light-state gain instead — tracked" note
and the IDB comment on 0x5aa170 were this misattribution; corrected 2026-08-21,
the recip now parsed as `env::terrain_color_recip_*` and served by
`EnvironmentState::terrain_color_recip_packed`). The instance API around it:
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
  +36): entity+436 is a SHARED effect handle. Every model `LGHT` record
  overwrites it (`Entity_SpawnGlowEffects @0x56c925..0x56c92c`), so a shooter
  with model lights reuses the final authored instance: mode 4 / 5 ticks,
  owner = shooter/section 0, muzzle position, blend 1.0; its authored radius,
  base color, and gen block remain. Only when the word is zero does retail
  spawn radius 1.5/color 0xFFE0A0. Owner ≠ 0 means the result lights only the
  shooter's draws. Once the five-tick fade kills the slot, the cached nonzero
  handle re-arms a dead/reused slot rather than allocating again (the port's
  generation lease rejects the reused-slot write-through safety hazard).
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

Authored model lights are deliberately different from round/muzzle follow.
`Entity_SpawnGlowEffects` builds the entity placement matrix and transforms
the record point once `@0x56c82d..0x56c85d`; its `subobject` byte is not read
until the owner-group branch `@0x56c89a`. `CEffectInstance_SetPositionAndBounds
@0x5a9070` has only projectile and muzzle callers, so no model-light entity,
ROBJ, or bone follow exists. `Entity_SpawnGlowEffects` has exactly three call
sites (the two mission-start pool walks and powerup respawn), none in the death
or husk paths. A husk swap therefore leaves the original instances fixed and
does not instantiate husk `LGHT`. Actual `Entity_Destroy @0x43e810` reads the
single final word at +0x1B4 and calls `CEffectInstance_ClearByHandle` only once
`@0x43e903..0x43e916`; earlier records remain until the mission pool reset.
OpenNova matches this lifecycle, including the shared model/muzzle word and
one-lease entity exit, while `reset()` still clears the complete mission pool.

**Owner groups and the blink-box attach (PORTED 2026-08-21).** The owner-group
write in the model-light spawner is conditional and runs in a fixed branch
order (`@ 0x56c89a..0x56c8db`): a record with a nonzero `subobject` byte is
owned by its entity + section (grouping only, never a position transform); a
record inside a blink box is owned
by the CONTAINING building + section (interior room lights); every other
record — the fire barrels — spawns unowned and lights the world. The blink
query itself runs ONCE per spawning entity, before the record walk
(`Entity_QueryBlinkBoxesAtPoint @ 0x4af350`, called `@ 0x56c7fc`), and only
slot 0 of the packed quad is read: owner = `Pool_GetEntryUnchecked(2, hit >> 20)`
`@ 0x56c8c9`, section = `(hit >> 12) & 0x1F` `@ 0x56c8db`. Retail **skips the
query outright when the spawning entity's ItemDef type is Building**
`@ 0x56c7ec` — the query has no self-exclusion, so without that gate a
building's own unattached records would bind to itself; instead they stay world
lights. The port is the pure `renderer::resolve_model_light_owner`
(engine/runtime/renderer/light_scene.h), fed by the director's per-source
facts.

`Lighting_OwnerGroupEntity`/`InteriorGroup*` are per-DRAW-CONTEXT state, so
owned lights illuminate only the draws that declare them. Both groups are now
supplied per draw:

- **Owner group** — `Lighting_SetOwnerLightGroup @ 0x5a9100` (pair
  `@ 0x272ED7C/78`). The sector walk pushes the drawn entity `@ 0x5c7fb1`, and
  inside the model the batch collector re-scopes it PER RENDER OBJECT as
  `(0, robjIndex)` `@ 0x5d8ff7` — which is what section-matches a building's
  own interior lights to the room being drawn.
- **Interior group** — `Lighting_SetInteriorLightGroup @ 0x5a90e0` (pair
  `@ 0x272ED84/80`). An entity draw pushes its containing building + blink
  section, read straight off the entity's packed blink ref (entity+464, the
  `blink_hits[0]` quad); retail additionally gates on the containing entry
  carrying interior data (`pool_entry + 48`, the same field the iris march's
  no-interior-data short-circuit reads), where the port's stand-in is simply
  that the containing building resolves to a presented model
  `[orig: setup_terrain_effect_for_entity @ 0x5c74a0]`. The same pair feeds the
  render-slot dominant-light pick `[orig: RenderSlot_UpdateEntityLight
  @ 0x5d6b66]`. A building draw pushes itself + section 0 `@ 0x5c5e07`. The
  terrain batch clears BOTH pairs `@ 0x60967c/@ 0x609685`, so only unowned
  lights reach terrain.

The gate reading them is `Light_PassesActiveGroups @ 0x5a9120`: an unowned
light always passes; an owned one passes when its owner is the interior group
entity AND its section matches the interior section (falling back to the OWNER
section when the interior section is 0 — the building-draw case), or when its
owner is the owner-group entity outright. `LightScene::select` is that
function; `LightDrawContext::groups` carries the pair per draw, filled from
`Simulation::get_entity_interior_groups` / `local_player_interior_group` (the
first-person parts inherit the local player's group, so a room's lights reach
the arms and weapon).

Both retained ObjectModels and batched statics preserve retail's draw
granularity. A BUILDING expands to one context per visible ROBJ; its interior
group names the building at section zero and its owner section is re-scoped
to the ROBJ, so grouped lights reach only their authored section
`[orig: Terrain_RenderSectorModels @0x5c5e07; collect_render_objects_for_batch
@0x5d8ff7]`. The query box of every such context is retail's ENTITY box —
position ∓/± `boundRadius` per axis, collected once per entity
`[orig: Terrain_RenderSectorModels @0x5c5ea6..0x5c5ee4; setup_terrain_effect_for_entity
@0x5c74fb..0x5c753a]` — the per-ROBJ step only re-gates that ordered list
(the engine contract on `renderer::LightDrawContext`; the host currently
stamps each ROBJ part's exact world AABB instead, the mismatch noted on
D-RLIT-4). MultiMesh rows retain immutable
per-entity/per-ROBJ atlas identities, exact transformed bounds, owner section,
and containing-blink stamp. The same selection function fills live instance
uniforms and static RGBAF rows, and the D3D12 24-technique probe requires those
routes to rasterize byte-identically. Wire handle zero occupies a tagged owner
domain and cannot alias the unowned/world sentinel.

Mission-start static records and late/powerup ObjectModel nodes enter the same
pool at their spawn-time entity transform. A retained or grafted husk is never
rescanned and restoration does not respawn or move `LGHT`; this is the retail
behavior, not a residual. A live node's actual exit clears only its cached
final entity handle, while mission reset synchronously clears every remaining
instance and published shader row. `effect_light_world_test` pins spawn-fixed
position, shared model/muzzle cache, powerup replacement, and one-handle exit;
`per_model_light_isolation_test` pins both owner domains and ROBJ selection.

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
feeds the wind VS's c7/c8 projection). The texture loaded by
`Terrain_LoadTileSetAtlas @ 0x604a90` is not a mission lightmap. It is the
configured 64-pixel-cell tile-set strip: the loader derives the atlas
dimensions/reciprocals @ 0x319f7b0.., and the live
`PolyTrn_RenderTile @ 0x60da70` consumer binds its view @ 0x319f7d4 before
indexing each `.til` entry by `tile_index % TilesX` / `tile_index / TilesX`
(@ 0x60ddd4..0x60df1b). The only other xref to that view is the uncalled helper
currently named `render_foliage_billboards @ 0x607b30`; it is not a second
foliage-lightmap path. Detail foliage instead samples the composed terrain tile
RT selected by `Terrain_FindSectorPatchRT @ 0x6042a0`, exactly as the hosted
shared 128-layer terrain/foliage cache does. The CPU-side
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
cache at `0x60E10A..0x60E19D`. A targeted live D3D9 probe closes the final
state: the draw has RGBA writes, ONE/ONE blending, no separate-alpha blend,
and `PSDepthAlpha=(0,0,0,tempBlue)`; all eight observed page draws preserved
RGB byte-for-byte and added temp blue to destination A=0. This is hosted by
`composite_terrain_static_shadow_pixel` plus the collapsed alpha-page handoff.
Terrain receives that cache as t0 and foliage
as t1, so both inherit the projected shadow while sector-model floors and
walls do not receive or self-shadow from it. Ihq01 is the concrete parity
case: pool-2 index 40, no `NoShadow`, three ROBJ in every LOD; all three cast,
despite ROBJ 1/2 being interior-lighting sections in the live model draw.
Dynamic people/ItemDef `DynamicShadow` render slots remain independent
(`Entity_InitFromModel @ 0x40E1BC..0x40E1F7`). Full tile-producer record:
[terrain-re.md](../terrain/terrain-re.md).

The reimpl now follows that receiver domain directly for its supported subset.
A typed static-source snapshot follows destruction, husk, and dynamic transforms;
the page provider selects the retail LOD/ROBJ population, resolves supported
opaque/alpha-tested materials (including every authored diffuse-alpha frame),
evaluates the shared AlphaGen/UV/frame runtime at the submission clock, and
rasterizes only page A — since 2026-08-22 clipping each projected triangle at
the ortho's near plane first (`setup_shadow_cascade_matrices_0 @ 0x58D5F8..
0x58D60C`: depth `h·0.0005 − 0.00005`, so geometry lower than 0.1 u above the
caster's ground plane never casts; `renderer`-side port in
`engine/runtime/terrain/terrain_static_shadow_raster.cpp`), and fed the RAW
`Environment_GetLightDirectionFloat` tuple — the projector's (g2,g1,g0) and
the DOT3 (g2,g0,g1) reductions are retail's own packings of that tuple, so the
page path must never receive the Godot-axes vector the celestial getters serve
(the 2026-08-20 getter change had swapped it twice, rotating every static
shadow's azimuth 35.6° at 15:00). Terrain and detail foliage then sample the
same current-frame cache binding, while sector-model floors remain outside
the receiver.
The former terrain-only directional static-shadow light and black next-pass
catcher are retired. Remaining ordered contributions, scorch updates, and
final cache cadence/edge/address/mip behavior remain D-TERRAIN-7; the generic
process-global CTRL/RNG lifetime is tracked by D-3DI-2.

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
technique's source, closing the former D-RORD-5 specular-cube question.
**CubeEnvironment** is live but it is not a whole-world scene capture.
`update_environment_cubemap @ 0x6106a0` renders all six faces initially/on
force and every 128 render frames. Highest quality selects 256²
(`init_render_textures @ 0x58f6e0`; lower settings select 64/128). The eye is
the local-player origin +1 Y, raised again to at least terrain+10. Each face
uses a 90° square LH view, near 0.5/far 1000
(`GTexture_RenderCubeMapFace @ 0x6864d0`). Face directions/up are
`+X/+Y, -X/+Y, +Y/-Z, -Y/+Z, +Z/+Y, -Z/+Y` in retail render-float axes.

The callback at `0x5c3700` draws only the sky dome and sun/moon, with lights
disabled; it does not submit terrain, world objects, water, stars, glare, or
glint. It then multiplies the completed gamma framebuffer by vertex color
`0xFF606060` under `SRC=DESTCOLOR, DST=ZERO`, and additively draws the
sun-aligned CubeRotSpecular sphere through `render_sky_mesh @ 0x5ac680` and
the `Env_LightBlock` MODULATE2X descriptor. OpenNova mirrors that topology.
The witnessed facts (face count and size, the 128-frame cadence, the eye
placement, the 90° square view, the dim byte and its rounded-half-up byte
product) are the engine's `<runtime/renderer/environment_cube.h>`
(`renderer_environment_cube` ctest); `EnvironmentCubeCapture` is the shell's
device side: six layer-isolated 256² SubViewports rendered together in the frame the
cadence fires, then, on the next advance, a RenderingDevice copy leg
(`godot/src/render/environment_cube_blit.cpp`, run through
`RenderingServer::call_on_render_thread`) draws each face's render target by
a full-screen triangle into one layer of an RD cubemap (RGBA8 UNORM, layers
`+X,-X,+Y,-Y,+Z,-Z`), applying the face orientation and the exact 0x60 byte
multiply in integer arithmetic (the gamma value truncated to the framebuffer
byte, then `(byte × 0x60 + 127) / 255`, `environment_cube_dim_byte`), and the
cube is published as a `TextureCubemapRD` through the
`opennova_environment_cube` shader global. No CPU readback, Image or Cubemap
resource exists: the 2026-08-22 shell read the six faces back with
`ViewportTexture::get_image` on every publish, a stall the 00TRa Mission Rows
probe measured at 19.4 ms mean / 52.7 ms max on the publish frame's env-cube
leg (2026-08-30, before); the copy leg measured 0.11 ms mean / 0.18 ms max on the same run shape (after), the publish-frame wall time within 1 ms of its neighbours. The
static sphere is evaluated analytically as the same white pow-800 plus warm
pow-40 lobe times the live light block. The render-float→Godot X/Z swap, the
cube layer order, every face orientation, same-frame publication, and
full/midtone byte values are exercised through the final `samplerCube` by
the `environment_cube_capture` probe
(`godot/probes/render/environment_cube_capture_probe.gd`) on Forward+ D3D12;
`godot/tests/environment_cube_capture_test.gd` pins the `TextureCubemapRD`
publication, the cadence re-copy into the same cube, and the exit-tree
release / re-entry re-copy in a windowed Forward+ run (pending headless).

`EnvCube_RenderFaceOrAnalyticFill @ 0x58b220` also contains a no-callback
analytic five-light sky fill (blue-from-above 0.3/pow2, warm ground bounce
0.25/pow3, three warm pow-50/60 sun lobes); that branch is caller-less dead
code in retail JO.

**The render-slot (entity ground shadow) side** — witnessed end to end and
PORTED (2026-08-20; the earlier ADR-0023-era "Shadow_/Scar_ family
exclusion" is superseded for the RenderSlot_* half — the Scar_ decal family
alone remains out of REN scope). **2026-08-22 — the two extent bounds
ported** (the "objects cast too-tall shadows" report): the device had drawn
the projected silhouette over the whole capture frustum, sized from the
model's render-bounds diagonal. Retail bounds every drape twice. (1) The
PATCH: the drape is drawn only over the lod × lod terrain-following square
`RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130` builds around the marched
anchor — origin `anchor − lod/2` east / `+ lod/2` north, rounded to the lod
band's grid (`(v + 0x8000) & 0xFFFF0000` below lod 10 `@ 0x5d515e`,
2 u for 10..15 `@ 0x5d517d`, 4 u from 16 `@ 0x5d5197`), `(lod + 1)²`
vertices stepping east (`@ 0x5d52f7`) and south (`@ 0x5d527a`) at 1 u, so
no entity's shadow ever covers more than 20 u; lod = the grazing-rescaled
`clamp(2r + 1, 6, 20)` with r = entity+0 boundRadius — the .3di origin
sphere `Entity_InitFromModel @ 0x40dc30` stamps (`gpm[5]` × scale, husk
max, + 0x1000), NOT `Entity_ComputeBoundingSphere`'s collision-AABB sphere
(that is entity+0x208). (2) The DEPTH CLIP: the drape pass is a
fixed-function two-stage desc (`dword_2BE3A50`, written `@ 0x5d6318..
0x5d635e`): blend `SRC = DESTCOLOR, DST = ZERO`; stage 0 `COLOROP ADD
(TEXTURE = the silhouette RT, DIFFUSE = the ambient-law material)`; stage 1
`COLOROP ADD (TEXTURE = shadowztex, CURRENT)`; both stages texgen
`D3DTSS_TCI_CAMERASPACEPOSITION + COUNT2` through texture matrices 12/13
(`0x3266B88`/`0x3266BC8`); samplers from global slots 28/29
(`sub_679630(28, 29, ..., 0x1520000)` — slot indices, the word's low 16
bits zero, the upper bits pass flags). Because the stage ADDs and
saturates, shadowztex's WHITE suppresses the shadow and its BLACK keeps it:
`u2 = 0.5 + k·f2·(p − lp)` with `k = 0.5/half_size`, `lp = entity pos −
dir1`, `f2` = the slot direction with its vertical × 4 for a person then
normalized; `v2 = 0.5 + 0.333·k²·f1·(p − lp)` (the detail v row is the
primary's ALREADY k-scaled depth column × 0.333 k, `@ 0x58d222..0x58d249`);
ground beyond the caster plane (u2 > 0.5, away from the light) draws, ground
toward the light is suppressed, and the far row (v2 ≥ 0.75) fades to white.
Port: `renderer::slot_patch_bounds`, `slot_depth_clip`, `shadowztex_pixels`
(ctest `renderer_render_slot_shadow`), the device publishes the patch and the
clip rows per slot and samples the generated 32×4 texture in
`slot_shadow_drape.gdshader` using the retail texture-stage equation
`min(1, capture_rgb + (1 - (1-f)*q) + ztex)`, the
anchor march probes `TerrainData` (ground-standing casters exit at step 0),
and the placer stamps both radii on every model it builds — items and the
avatar body/head parts alike — as `ObjectModel.set_shadow_bound_radii(model
sphere, entity bound)`: the MODEL SPHERE (gpm[5], the header's origin sphere)
sizes the capture extent and the depth clip (`RenderSlot_RenderEntityAndChildren
@ 0x5d7835` reads the model's +0x14, never entity+0), the ENTITY BOUND is that
sphere raised to the first husk stage's and padded + 0x1000 only when the
graphic carries a collision block (`Entity_InitFromModel @ 0x40dc30`'s
gpm[44] gate `@ 0x40de8f`; a collision-less model keeps entity+0 = 0, so its
slot takes the lod floor 6) and sizes the lod/patch and the light query. The
blob leg places its patch with the slot's stored direction too (the
dominant-light pick runs for every bound slot, as
`RenderSlot_UpdateEntityLight` does from the entity update). The D-COL-3 def
scale/collision-gate/first-husk fold now feeds the entity-bound slot stamp.
Residual: a model the placer did not build (no stamp) falls back to half its
render-bounds diagonal for both radii.

*Registration* — `Entity_InitFromModel @ 0x40E1C8..0x40E1F7`: persons
always, items via the `DynamicShadow` attrib2 bit, gated on the
shadow-detail option (`dword_24D2054`); the local player registers via
`PlayerClass_InitEntity @ 0x4b10f1`. `RenderSlot_AllocSlot @ 0x5d5690`
(ex `shadow_decal_alloc_slot`) finds or allocates the entity's 128-byte
record in the 256-slot `RenderSlot_Table @ 0x2be3d30`. Both JO callers pass
shadow type 1 (dynamic silhouette); the type-0 blob-only alloc leg is
caller-less. LOD at alloc: `(boundRadius >> 15) + 1` (2·radius + 1 u)
clamped [6, 20]; the dead type-0 leg reads `max(shadow w, l) + 7` clamped
[2, 20] from the authored decal size.

*Frame open* — `render_shadow_pass @ 0x5d7b70` loads the sun into the slot
default (`Environment_GetLightDirectionFloat @ 0x57d870` into
`RenderSlot_DefaultLightDir* @ 0x2bebd68`), **clamping the vertical
component to 0.25** — the SAME grazing floor the static tile collector
applies (`@ 0x60d325..0x60d341`) — and negating all three into the
light→surface form, so a low sun never stretches an entity silhouette past
4× height (at the 03TR 06:30 fixture's 7° sun, retail projects entity
shadows as if the sun sat at ~14.5°).

*Assignment* — `RenderSlot_SortAndAssign @ 0x5d6530` (ex
`terrain_sort_and_assign_render_slots`) scores every record:
2D camera distance ÷ 4 (fixed, `flt_7C333C`) × (1.5 − the view-alignment
dot, Q16 98304), halved for the local player or its parent vehicle;
excluded (score 0x40000000): dead entities, seat-parented entities
(parentSlot 1/2/5, or 3 with a live parent — they render as CHILDREN in
the parent's slot), entities standing on a vehicle-type ground entity
(itemdef +0x5C == 1), and anything whose base score exceeds 0x500000 — the
**320 u bind horizon**. Bubble-sorted ascending, the best 24 bind drape
patches (441-vertex terrain-patch VB regions at `441·patchIndex`,
first-free of 24; `RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130` rebuilds
each bound patch), and the first 12 take silhouette RT orders 0..11 —
dword1 = has-RT, dword2 = order, dword3 = dirty **only when the order
changed** (sticky captures).

*RT chain* — `RenderSlot_InitTextureChain @ 0x5d5320`: 12 RTs, base 256 px
(512 at shadow detail ≥ 2, 1024 at ≥ 4), halving after every second slot
down to a 32 px floor (`RenderSlot_TextureTable @ 0x2be3c94`).

*Per-slot light + anchor* — `RenderSlot_UpdateEntityLight @ 0x5d6a30`
(ex `Entity_UpdateRenderState`): default = the clamped sun trio, then the
brightest passing point light near the entity (the same
`collect_nearby_zones_by_aabb @ 0x5aa250` pool as D-RLIT-4, queried at
position ± boundRadius) wins when NTSC luminance 0.3R + 0.6G + 0.1B over
`dist²·quadratic + constant` exceeds 0.1 (zeroed for an interior-parented
entity); the winning direction is `normalize(entity − light)`. The shadow
anchor then marches from the entity (or its rotated bbox-center anchor
when the entity flag word is zero) along the light direction in unit-planar
steps down to `Terrain_GetHeightAtPosition @ 0x606720`; the vertical step
keeps the direction's own rate and is SUBSTITUTED by 0.5 u of drop (fixed
−32768) only when it would not descend (`test eax, eax; jl` `@ 0x5d6cd7..
0x5d6cdd` — corrected 2026-08-22; the earlier "clamped to ≥ 0.5 u" reading
steepened every shallow sun). The anchor PLACES the drape patch; the
projected UV matrices land the silhouette.
The slot LOD refreshes grazing-scaled: `(0.5 + |0.5/dirY|)·baseLod`
clamped [6, 20] (`@ 0x5d6d5c..0x5d6dac`).

*Silhouette render* — `RenderSlot_RenderEntityAndChildren @ 0x5d7690`
renders the entity plus its standing/mounted children into the slot RT
(ortho extent = radius·1.25 clamped radius + 0.75,
`setup_shadow_cascade_matrices @ 0x58d300`: a rotation-only D3D view from
the slot direction through `build_direction_look_at_matrix @ 0x612c90` —
forward = normalize(dir), right = normalize(fwd.z, 0, −fwd.x), up = fwd ×
right, a vertical direction leaving right/up ZERO — with the entity rendered
at that view's origin (`Entity_RenderWithLODCallback @ 0x5d6ef0` zeroes the
position for a null origin, children at their offset `@ 0x5d795a/0x5d79c8`)
under an orthographic projection of scale 1/extent over the depth band
0.2..5000.2 (`@ 0x58d38b..0x58d3a3`: 1/(far − near) = 0.0002, −near/(far −
near) = −0.00004); ported 2026-08-29 as `renderer::silhouette_capture_basis`
(since 2026-08-30 the one engine look-at, `renderer::direction_look_at` in
`direction_look_at.h`, which the addeweap attachment frame in
`simassets/mounted_pose.cpp` consumes too) and the `kSilhouetteCaptureNear/Far`
constants; as read, that band starts 0.2 u in front of the view origin the
entity is rendered at, and the render state that admits the entity's near
half is unwitnessed — D-RLIT-10 records the shell's eye), on the detail-scaled refresh
cadence: `(frame & mask) == (slotIndex & mask)` with mask 7 below detail 2,
3 at 2, 1 at 3, every frame at 4+; the local player (or its parent) skips
only below detail 3; the dirty bit forces. Lighting via
`RenderSlot_SetupNextLighting @ 0x5d7250` (ex
`setup_entity_render_lighting`): D3DRS_AMBIENT white, then either the
attached light (D3D light 4 + luminance-weighted NEGATED colors
`(c+lum)/2 × −3` into PS c21..c23 — the silhouette darkening math) or a
white directional with 0.75 ambient material. The PROJSHAD effect output is
black, not the normally lit material: `_vsPost.fx::vsPostBlackT1` writes
`Diff = (0,0,0,1)` and `_vsSkPost.fx::vsSkinPostBlackT1` writes `Diff = 0`;
every PROJSHAD pass selects that diffuse for color (`TSSColor SelectArg2
Diffuse`; `_FFP.fx TBoringFFPProjShad` zeroes the material), so RGB is black
over the RT's white clear, with the effect-family diffuse/detail/AlphaGen
policy supplying coverage.

*Drape* — `RenderSlot_DrawAllDrapes @ 0x5d6e20` (detail > 0, dead entities
skip; the LOCAL player in first person skips while prone-latched
(`g_PlayerStanceProneLatch @ 0xb76484`) or below detail 2): a dynamic slot
with a live RT draws `RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0` — the
silhouette projected over the 21×21 terrain-following patch, distance fade
`f = clamp((d − 40 u)/40 u)` with a hard skip at ≥ 80 u, a SECOND texture
stage that clips the projection by depth — `shadowztex`
(`shadow_system_init_resources @ 0x5d62d2`: a 32×4 white/black step, one
gray texel at the boundary, row 3 all white) addressed by
`build_shadow_cascade_uv_matrices @ 0x58cf10`'s detail matrix (u = depth
along the clip direction × 0.5/half_size + 0.5, v = 0.333 × (0.5/half_size)²
× depth along the light direction + 0.5 — the v row multiplies the already
k-scaled primary depth column by 0.333 k, `@ 0x58d222..0x58d249`; corrected
2026-08-22) — where person-type entities (itemdef +0x5C == 3)
steepen the CLIP direction's vertical component 4× (`flt_7C44B8`
@ 0x5d5d85: the copy at slot+108 feeding lookat_dir2 only; the silhouette
projection keeps the unscaled lookat_dir1 — re-witnessed 2026-08-21, the
earlier "elongated 4× along the projection direction" reading was wrong),
and the **sun ambient law** per channel:
`ambient_c = 1 − (1−f)·L_c·|dirY| / (L_c·|dirY| + S_c)` with
`L = Env_LightBlock`, `S = Env_SkyBlock` — the shadow removes only the
direct sun term scaled by the projection vertical, never the sky ambient
(attached-light slots instead light the patch with the color scaled
`−(c+lum)·(1−f)`). A bound slot WITHOUT a live RT draws
`RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0` — the items.def
`shadow <name>.tga w l ox oy` decal (name → ItemDef +0xA0, floats →
+0x11C/+0x120/+0x124/+0x128, `ItemDef_ParseProperty @ 0x49f3a5..0x49f44c`;
resolved texture at +0x114) heading-rotated over the same patch at 1/w,
1/l UV scale with the authored UV offset + 0.5, black-ambient material
(plain multiply), no distance fade. Entity ground shadows therefore land
on TERRAIN ONLY — the patches are terrain-following meshes.

*The port* — planning and color laws are portable in
`engine/runtime/renderer/render_slot_shadow.{h,cpp}` (direction clamp,
alloc/grazing LOD, RT chain, cadence, scoring/24-12 assignment with sticky
captures, dominant-light pick, anchor march, the capture view basis and
depth band, fade + ambient/darkening and silhouette-combine laws; ctest
`renderer_render_slot_shadow` pins each), and the per-technique PROJSHAD
coverage source in `object_shader_template` (`object_projected_shadow_coverage`
beside the pass-state table; ctest `renderer_material_classify`). The device
half (`godot/src/env/slot_shadow.cpp` + `godot/src/render/slot_capture_adapter.cpp`
+ `slot_shadow_drape.gdshader` on the terrain material; `engine/formats/def`
parses the `shadow` line; GUT `slot_shadow_test`) realizes the capture
(since 2026-08-29) as one RenderingDevice pass: `SlotShadow` publishes a
typed request per armed slot (order, the capture pose from the witnessed
basis, the ortho projection of the witnessed extent, the caster and its
claimed capture-with children) and the `SlotCaptureCompositorEffect` on
the beauty view's compositor draws them at PRE_OPAQUE — before the terrain
drape samples them in the same frame — into twelve colour targets at the
witnessed chain sizes, cleared to the retail 0x00FFFFFF, black
fragments under the technique's PROJSHAD coverage (Diffuse1.a, × Detail.a
over the transformed UV2 for the _MT FFP blocks, × AlphaGen for the FFP
families, the alpha test where the material carries one) in the
technique's PROJSHAD blend state (the file effects and the _FFP opaque and
multiplicative variants replace; the _FFP alpha-blend variant blends the
black by that coverage, SRCALPHA/INVSRCALPHA over the white clear, so a
translucent strip casts a partial silhouette — `_FFP.fx TBoringFFPProjShad`,
decoded; no pass for tracer/flag/glass, the material-blend additive
variants skipped as a no-op, and an ArrayMesh surface whose material carries
no registered object classification drawn nothing and counted as
`slot_unclassified_surfaces`) with depth test and write inside the target,
resolved into RGBA8 textures the drape samples through `Texture2DRD`s. The geometry rides the shared `Q3GeometryCache`
(packed once per surface; skinned strips pack their bone indices/weights
and skin on the GPU from the frame's bone palette). Device folds, each
serving the same observable: the capture eye backs off along −forward in
`slot_shadow.cpp` (D-RLIT-10 below) where retail renders the entity at the
origin of its rotation-only view, and the zenith degeneracy takes the
world x axis; the pass rasterizes each capture 4x multisampled and resolves
it into the single-sample target the drape reads, where retail's chain is
single-sampled end to end (`RenderSlot_InitTextureChain @ 0x5d5320` ->
`create_render_target_surfaces @ 0x67f7b0`: a plain `D3DUSAGE_RENDERTARGET`
texture and a `D3DMULTISAMPLE_NONE` depth surface) and its drape samples the
RT bilinearly — the resolve keeps a partial-coverage edge so a hard-aliased
1-2 px silhouette line does not scintillate against the breathing
first-person camera; the terrain surface stands in for the 21×21 patch mesh and
the projection is evaluated per pixel, bounded (since 2026-08-22) by the
lod × lod patch the anchor march places (`renderer::slot_patch_bounds` over
`TerrainData`'s height query) and clipped by the shadowztex stage
(`renderer::slot_depth_clip`), both published per slot to the shader; held
weapons ride their owner's slot via the capture-with link
(`ObjectModel.set_slot_shadow_capture_with` — the
`RenderSlot_RenderEntityAndChildren` child walk) while tree-parented
riders fold into the ancestor exclusion; the attached-light drape folds
the light's attenuation at the entity into the per-slot term (retail
varies it per patch vertex); and `scene_output` leaves the factor in
the retail gamma-byte domain for the shared FrameFx display decode. The
object wrappers carry no PROJSHAD branch (the twelve-layer capture-camera
signature, its `OBJ_PROJSHAD_*` defines and the reserved `Water` layers
were retired 2026-08-30 with the shader hash golden regenerated); the
render-swatch `projshadow` probe now proves the RenderingDevice pass on the
synthetic fixtures (opaque, alpha-tested single and _MT, alpha-blend, and
the additive-LUM / glass no-pass surfaces) beside the beauty frame. The
packaged runtime serves shadow detail 3, retail's highest SHADOWQUALITY
(`options.mnu` rows 0..3; `Settings_ClampGraphicsOptions` clamps to 3
`@ 0x54d546`; the 0x34-byte settings block copy `@ 0x551500` lands it in
`RenderSlot_DetailLevel` unchanged via `Terrain_Init @ 0x60fcc5` ->
`@ 0x5d6159`): the 512-base chain, the mask-1 refresh stagger for non-player
slots, every-frame local player. The `>= 4` 1024-base every-frame tier
(`RenderSlot_InitTextureChain @ 0x5d535a`) is unreachable from any retail
config. Measured at the cutover (2026-08-29/30, the Mission Rows probe,
1600x900 Forward+ D3D12, three 8 s windows each, mean per frame): CP01
frame wall 16.14 ms -> 14.98 ms with the retired per-viewport
`render_slot_cpu` 1.27 ms and `render_slot_gpu` 0.68 ms gone,
`world_slot_shadow` 0.29 ms -> 0.44 ms (the compile now runs on the main
thread), 2.5 captures/frame, 19.5 surfaces, 5 GPU-skinned commands; 00TRa
frame wall 16.80 ms -> 14.60 ms with `render_slot_cpu` 2.18 ms and
`render_slot_gpu` 0.70 ms retired, `world_slot_shadow` 0.13 ms -> 0.36 ms,
2.0 captures/frame, 18 surfaces, 8 skinned commands; a stable frame packs
0 vertices on both.

**Low-sun drape defect remains open (revalidated 2026-08-24, 03TR dawn,
PR #560).** At the 03TR spawn the M939 drape remains visibly different, and
strafing exposes temporal flicker in the same projection. The first attempted
fix changed the PROJSHAD edge representation and drape combine to a
black-on-white additive form; an in-game before/after check showed no visible
improvement, so that explanation is rejected as the cause. The live retail
slot observation (`type=1, rt_live=1, order=6/2`) still establishes that this
is the silhouette-drape leg, but the capture/update/projection path must now be
measured under deterministic lateral motion before another correction is
claimed.

## The per-light terrain projected pass (witnessed + ported 2026-08-21)

The pool's GROUND leg: each terrain batch is re-drawn once more PER LIGHT,
additively, through a two-stage projected-texture technique. Witnessed in
full this round (read-only decompiles; the facts are cited in
`engine/runtime/renderer/light_terrain_pass.h`):

- **Per-batch collect** `[orig: render_terrain_sector_batch @ 0x6092a0,
  @ 0x609641..0x609953]`: the batch's own AABB (sector origin + the record's
  float corners through `Math_FloatToFixedPoint3_YNegated`) collects the pool
  with `collect_nearby_zones_by_aabb(min, max, &count, 16)` `@ 0x60966f` — a
  cap of SIXTEEN, not the object pass's 64 — then CLEARS both light groups
  (`Lighting_SetInteriorLightGroup(0,0) @ 0x60967c`,
  `Lighting_SetOwnerLightGroup(0,0) @ 0x609685`), so only UNOWNED lights ever
  reach the terrain; a counting pass `@ 0x609690..0x6096c9` keeps the leg only
  when a handle passes `Light_PassesActiveGroups @ 0x60969f` AND
  `LightInstance_IsAliveAndLightsTerrain @ 0x6096b3` (alive, flags without
  0x400); the per-light loop `@ 0x60984c..0x609953` re-draws the batch once per
  passing handle with NO four-light cap. The loop pushes `use_alt_texture = 1`
  `@ 0x609850/@ 0x60989a`, so the normal pass applies `dword_2732DC8` (state
  0x600); `dword_319FBD4 & 0x100` `@ 0x609890` selects
  `foliage_setup_render_matrices @ 0x6098a5` (the 0.4/r alt pass) instead of
  `Light_SetupTerrainProjectedPass @ 0x6098b4`. Skipped when
  `PolyTrn_UsePixelShaderPath == 0 @ 0x6095e4` or `dword_319FB84 != 0 @ 0x60983f`.
- **The pass setup** `[orig: Light_SetupTerrainProjectedPass @ 0x5aa830]`: the
  argument is a `Light_InstanceTable` slot `@ 0x5aa844`; `inv = 32768 /
  radiusQ16` (0.5/r) `@ 0x5aa864..0x5aa873`; two texture matrices from the
  runtime camera→world matrix `flt_27219C0` (written by
  `Render_SetViewAndProjectionMatrices @ 0x58d947` /
  `Render_SetViewAndProjectionFromMatrices @ 0x58d752`): stage 0
  (`Light_TexLight2D`, 64²) `u = (world·col0 + (F0 − pos.x))·inv + 0.5`,
  `v = (world·col2 + (F8 − pos.z))·inv + 0.5`; stage 1 (`Light_TexSpot1D`,
  64×8) `u = (world·col1 + (F4 − pos.y))·inv + 0.5`, `v = 0.5` — the texgen is
  camera-space, so engine-side it reduces to the light-relative world offset
  × inv + 0.5. Pixel constants: `c0 = (0, 0, 0, 1)`, `c1 = 0.5 · (rgb · blend ·
  EffectWorld_AmbientScale · flt_2732DA{C,8,4} · 0.66)` `@ 0x5aa9c8..0x5aaab3`
  (× `RgbGen_EvaluateColor` when the record carries a gen block `@ 0x5aaa05..
  0x5aaa5f`), alpha 1. The 0.5 exists because stage 0 runs MODULATE2X in the
  applied shader (`GfxShader_ApplyPassChecked @ 0x5aaafe/@ 0x5aab16`): 0.66 ×
  0.5 × 2 = 0.66; stage 1's 2× is left uncompensated.
- **The per-channel factor** is `Env_TerrainColorRecip` unpacked × 1/128 into
  `flt_2732DAC/DA8/DA4` by `EffectWorld_TickInstancesAndLightScale @ 0x5aa1ef..
  0x5aa23f` — the environment default `0x808080` `[orig: Environment_InitDefaults
  @ 0x57c050..0x57c065]` divides to exactly (1, 1, 1). It is NOT the ambient
  scale (see the gain unpacks above).
- **The textures** `[orig: Texture_GenerateProceduralFalloffTexture @ 0x5a92c0;
  Lighting_InitTextures @ 0x5a95a6..0x5a9612]`: mode 2 = `trunc(255·exp(−4x²)·
  exp(−4y²))` opaque grey (`Light_TexLight2D`), mode 1 = `trunc(255·(1 − (x² +
  y²)))` clamped ≥ 0 with alpha = i, `x = (2·col − 65)/64` (c0 = 2.0, flt_7C44B8
  = 4.0, dbl_7D9F98 = 255), border 0; the 64×8 `Light_TexSpot1D` strip is built
  inline: `(col·(row+1)) >> 1` grey, end columns white.

**Port.** `LightScene::collect_terrain_pass_rows` (engine/runtime/renderer/
`light_terrain_pass.{h,cpp}`) runs the collect + gates per patch and publishes
≤ 16 `TerrainLightRow{position, inv_scale, pixel_rgb}` per patch (the 0.5
folded); `falloff_texture_light2d_argb` / `falloff_texture_spot1d_argb` are the
generators; `env::terrain_color_recip_*` + `EnvironmentState::
terrain_color_recip_packed` serve the factor. The device (`godot/src/lights/
light_scene`, `terrain_lighting.gdshaderinc`'s `terrain_point_light_pool`,
`godot/game/world/terrain_light_leg.gd`) uploads the two generated textures once
and pushes per-patch light uniforms, summing `2·2·pixel_rgb·tex2d·tex1d`
additively after the base composite — the explicit `2·`s are the MODULATE2X
fold — published in `render_light_frame` for the NEXT terrain frame (one-frame
latency at 62 Hz, stated as the device fold). Pinned by ctest
`renderer_light_terrain_pass` (selection, gates, the 0.5/r and 0.4/r scalars,
the recip default, the projection u/v, the 2× round trip) and the GUT terrain
shader contract / light isolation tests.

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
- `engine/runtime/renderer/render_slot_shadow`: the render-slot entity
  ground-shadow planner (2026-08-20) — direction clamp, alloc/grazing LOD,
  RT chain, refresh cadence, priority scoring + 24-patch/12-capture
  assignment with sticky orders, dominant-light pick, anchor march, drape
  fade + the per-channel sun ambient law + the attached-light darkening
  constants, the capture view basis and depth band — ctest
  `renderer_render_slot_shadow`; the PROJSHAD coverage source per technique
  in `object_shader_template`. Device: `godot/src/env/slot_shadow.cpp`
  (the per-armed-slot capture requests + the drape uniform push) +
  `godot/src/render/slot_capture_adapter.cpp` (the PRE_OPAQUE
  RenderingDevice capture pass over the shared Q3 geometry cache) +
  `godot/shaders/slot_shadow_drape.gdshader` (the terrain drape next pass);
  `engine/formats/def` parses the authored `shadow` decal line.
- `terrain_lighting.gdshaderinc`: c0/c1 corrected to (sky, light) — the
  prior combined/fill pairing was a gobj-era stand-in. Its tile-alpha path also
  preserves EnvFile's direct retail getter tuple and applies the witnessed
  D3DCOLOR-to-GPU-RGB permutation `(z,x,y)` before byte quantization; analytic
  foliage uses the same correction (D-TERRAIN-10).

## D-RLIT divergence catalog

Fixed 2026-08-20 (**D-RLIT-9**, minted-and-closed — witnessed and closed in
one round, the 03tr-sun-sky fixture slice): several Godot device seams consumed the
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

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RLIT-1 | Hosted weather runs the complete 16-block chain: modulator2/modulator plus all 14 color blocks | 16 blocks modulate in witnessed order (including skyfog and the static ceiling/cloud/floor trio) `[orig: @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-21)** — `SkyWeatherColorBlocks` preserves the witnessed order; skyfog gets its lightning additive, horizon blend, and tail double; and `MissionEnvironment` writes every color current back. `SkyDome` and the frame clear share the final doubled skyfog, the flat dome consumes smoothed `cloud_rgb`, and indoor iris samples consume the pre-modulated ceiling/floor currents. |
| D-RLIT-2 | Iris exposure uses the marched 3-point camera-ray average with per-sample indoor/outdoor classification and sun occlusion | 3-point average marched back from the terrain/entity-clipped camera ray. `Environment_ApplyFogAndAmbient` passes `g_local_player_entity`; all samples reuse its `+0x1BC/+0x1C0` candidate slice for building blink tests and the nonzero-count outdoor ray gate; the three allow-all-types rays therefore test pool-1 dynamics and pool-2 statics. Indoor-with-data sets the interior light group, indoor-no-data retains it, and outdoor clears it `[orig: Environment_ApplyFogAndAmbient @ 0x57e440/0x57e51d; compute_ambient_light_along_direction @ 0x5c7a00; terrain_sector_compute_lighting @ 0x5c7550; raycast_entity_collision @ 0x413760; raycast_find_collision_entity @ 0x539a70]` | **FIXED (2026-08-22)** — `compute_iris_samples` now uses the shared nearest-collision clip, candidate-scoped blink query, exact local-player count gate, and candidate-scoped radiused walker; pool-1 source slices use the witnessed ItemDef gate instead of vehicle physics. The mutable iris light-group pair preserves the exact set/retain/clear sequence while typed per-draw groups own every actual light selection, avoiding a compatibility global. The original march/curve, indoor gain-255 short circuit, sun level 8−hits at −0x2000/−0x5000/−0x8000, and INT /3 average remain unchanged. Native `collision` regressions pin nearest clipping, indoor terrain bypass, candidate-only blink, dynamic blockers, and pool-1 eligibility. Measurement note (2026-08-20): frozen render fixtures previously published the modulator's mission-reset identity gain; capture now stamps the marched samples and settles the chase at the fixture pose (`Weather.settle_exposure`, capture-seam only). |
| D-RLIT-3 | `items.def light_transfer` drives interior ROBJ sections plus the contained player/viewmodel, and drawn outdoor pool-0/pool-1 entities dim DirLightColor by the witnessed 3-radius sun query | interior-parented entities lerp to floor/ceiling ambience by the parent daylight openness (model+536). Eligible outdoor pool-0/pool-1 entities cast one 200-u sun segment at clip radii −0x2000/−0x5000/−0x8000 from position + collision-AABB midpoint, walking the entity's OWN `+0x1BC`/`+0x1C0` candidate slice (self excluded at slice build; only bubble-overlapping solids can block); each blocked cast steps DirLightColor 1.0→0.75→0.5→0.25; contained entities, empty-slice sources, and pool-2 statics stay 1.0 `[orig: setup_terrain_effect_for_entity @ 0x5c74a0; Entity_ComputeSunVisibility @ 0x5c6800; raycast_find_collision_entity @ 0x539a70; Entity_BuildProximityListsFromPools @ 0x4b8eb0; the FP-pass discard @ 0x4deeb0]` | **FIXED (2026-08-23)** — local/authority and decoded-client draw paths now share the exact bound/center and three-radius method. Wire pool-0 and eligible pool-1 sources receive separately keyed 17-tick slices; explicit registry-twin identity is the only self-exclusion, so equal H/L packed values cannot alias. `get_draw_lighting_changes` replaces the bms-only API with `[wire,bms,quality]` triples, and the wire presenter applies/caches the factor for both bodies and late-built held weapons. Native collision proofs pin the 16-tick empty window, wire/local key separation, all-three-ray hit, and pool-1 EWeap exclusion; `wire_present_pass_test` pins body/weapon directional-light delivery. |
| D-RLIT-4 | The portable EffectWorld core, decay lifecycle, safe opaque handles, mission-start/powerup model-light spawn, four transient routes, target gates, both active groups, live/static per-draw selection, top-tier technique response, terrain projection, and coronas are hosted. Authored LGHT is spawn-fixed; static rows keep exact per-entity/per-ROBJ bounds and atlas identity; node teardown uses the one shared model/muzzle entity handle. | Retail transforms each LGHT point by the entity matrix once, uses `subobject` only for owner grouping, stores only the final handle at entity+0x1B4, reuses it for MF_Light, does not rescan husks, and clears it once at Entity_Destroy. Object draws query nearest 64 with the ENTITY's position ± boundRadius box and the batch entry keeps the first THREE group-passing, objects-enabled handles (the 4 in `Light_SelectAndEnableForDraw` is the D3D enable count FlushBatches tears down per entry) `[orig: Entity_SpawnGlowEffects @0x56c7c0; Entity_UpdateMuzzleGlowEffect @0x56c960; Entity_Destroy @0x43e903; collect_render_objects_for_batch @0x5d8ff7/@0x5d9229; collect_nearby_zones_by_aabb @0x5aa250; Light_SelectAndEnableForDraw @0x5ab9d0; setup_terrain_effect_for_entity @0x5c74fb; CRenderBatchQueue_FlushBatches @0x5da26b/@0x5da5de]`. Coronas use the owner-section visibility gate; Spot/Target delivery is dead code. Foliage also has no max-quality point-light consumer: its selector call at `0x60a5dc` is followed by a VS/PS pair whose only light fold is cached-tile c0/c1 and whose `oD0` is authored c6 `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupFarSlotDraw @0x60087a]`. | **FIXED (2026-08-23; cap corrected to three the same day)** — live ObjectModels, static MultiMesh rows, blink/interior groups, per-ROBJ sections, spawn/respawn/destruction lifecycle, highest-quality VS/PS technique consumption, terrain projection, and coronas are closed. The per-ROBJ/model query box must be the entity's position ± boundRadius cube stamped on every split draw (the engine contract `renderer::LightDrawContext` documents; `light_scene.cpp` currently stamps each ROBJ part's own AABB and the model AABB — the one remaining host-side mismatch of this row). `effect_light_world_test`, `per_model_light_isolation_test`, native `renderer_light_scene`, the 24-technique D3D12 swatch, and the foliage shader contract pin the routes. Generation leases intentionally reject retail's stale-handle write-through memory alias. |
| D-RLIT-5 | The earlier implementation substituted hemisphere-along-reflection for CubeEnvironment and approximated Phong channels | NORMAL now samples a synchronously hosted 256² CubeEnvironment with the exact callback/cadence/origin/camera/dim/static-lobe chain; PhongMap is generated byte-for-byte and every ordinary/point lobe and Diffuse1-alpha role is source-pinned `[orig: update_environment_cubemap @ 0x6106a0; callback @ 0x5c3700; Render_CreateSystemTextures @ 0x58aca0; _psPhong.fx; _psPhong2.fx]` | **FIXED (2026-08-22)** — `EnvironmentCubeCapture`, the generated PhongMap binder, the 24-technique swatches, and the Forward+ D3D12 cubemap axis/orientation/gamma-byte probe close the former source and hosting residuals; 2026-08-30: the faces are copied into the published RD cubemap on the RenderingDevice (`EnvironmentCubeBlit`), the former per-publish CPU readback stall gone and the byte parity kept exact. The same cutover's `FrameFx` closes the former D-RORD-5 GLOW post backend. |
| D-RLIT-6 | Shared terrain/foliage page cache hosts the configured tile-set strip, ordered `.til` RGBA, and DOT3 lighting alpha | `Terrain_LoadTileSetAtlas` loads `polytrn_tilestrip`/`Bms_TileSetName` + `.TGA`, divides it into 64-pixel cells, and `PolyTrn_RenderTile` indexes those cells for `.til` overlay quads before the DOT3 pass; no independently loaded mission-lightmap TGA exists `[orig: Terrain_LoadEnvironmentConfig @ 0x610940; Terrain_LoadTileSetAtlas @ 0x604A90; tile bind/index/draw @ 0x60DDD4..0x60DF1B]` | **FIXED / false premise closed (2026-08-23)** — `TerrainTilePageSourceView::tilestrip` and `compose_terrain_tile_page` host that exact producer; terrain and detail foliage consume the same published cache layer. Static model silhouettes and remaining RT mechanics stay scoped to D-TERRAIN-7. |
| D-RLIT-7 | Static mission objects (the placer's MultiMesh batches) froze the env lighting harvested at load — the throwaway template's materials had no live owner, so TOD/weather/iris advances relit animated models but not the static world (the load-time snapshot even carried the pre-first-iris-tick modulator: gain 1.0 vs the settled 60/64) | retail relights EVERY entity from the current lighting block each frame `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0 ← CRenderBatchQueue_FlushBatches]` | **FIXED (2026-07-06, the model-parity slice; mechanism re-ported 2026-08-26)**: first closed by registering every harvested batch ShaderMaterial and re-stamping it from the live env per frame (generation-gated, single-sourced with the per-model stamp; verified batch uniforms == live-model uniforms after settle, dir 159/255, gain 60/64). The per-material restamp was the reimpl's own mechanism (retail keeps ONE block per pass and no per-entity lighting state `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 -> RenderBatchCtx_StoreLightingConstants @ 0x5d89e0; Environment_ApplyFogAndAmbient @ 0x57e440]`) and cost ~1 ms per frame on 03TR (WORLD_WEATHER + MODEL_ENVIRONMENT + the render-thread material re-uploads); the world block now lives in the `opennova_light_block_*` / `opennova_fog_*` global shader parameters `MissionEnvironment` writes once per env change, the object shaders read it at draw time exactly like the batch context, and static batches, live models, and previews all follow the live env with no owner and no stamp. Object fog now takes the pass fog end (`fog_end_distance`, the overcast-scaled value `Render_SetFogState` receives) instead of the un-scaled `fog_level` the per-material copy carried |
| D-RLIT-8 | The object per-material hemisphere mixed color spaces: `hemi_sky` came from `MissionEnvironment.get_sky_ambient()` = the RAW TOD keyframe (never smoothed, never iris-modulated) while `dir_color`/`hemi_ground` came from the smoothed+modulated weather writeback — off-noon the modulator brightens every block toward the exposure target but the un-modulated sky half stays dark (the sky-facing half of every building too dark at night; the terrain/foliage GLOBALS path was already correct via `get_smooth_sky()`) | retail feeds ALL entity lighting from the post-modulator block render colors — the world-block writer fills [8..10] ← `Env_SkyBlock[0]` ÷255 exactly like light/ground `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; the blocks smooth + modulate in the weather tick @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-06, the REN-6 session)**: the sky block joins the per-tick env writeback seam — `Weather` pushes `get_smooth_sky()` through the new `MissionEnvironment.set_sky_ambient_rt` (mirroring fill/sun/fog, generation-gated), `get_sky_ambient()` serves the smoothed current and re-seeds from the keyframe on discrete TOD recomputes (the `_fill_light` contract); GUT pins the seam (`env_parity_vectors_test.test_sky_ambient_serves_smoothed_writeback`); golden env grid/weather rows byte-identical (the grid collects bare env nodes; the weather checkpoints already read `get_smooth_sky`) |
| D-RLIT-10 | The render-slot silhouette capture places its RenderingDevice eye two model-sphere diameters plus 2 u behind the caster center along −forward with a 0.05..(2·eye + r) depth band (`godot/src/env/slot_shadow.cpp`, beside the pose) | retail renders the entity at the origin of the rotation-only look-at view under the 0.2..5000.2 ortho band `[orig: setup_shadow_cascade_matrices @ 0x58d300 (proj[10] = 0.0002, proj[14] = −0.00004 @ 0x58d38b..0x58d3a3); Entity_RenderWithLODCallback @ 0x5d6ef0 zeroes the position]`; read literally, the band's near plane sits 0.2 u in front of the entity's own origin and would clip its near half, and the render state that admits it (a disabled D3D clip, an unwitnessed z state) has not been found | PERMANENT (class C, 2026-08-30): an orthographic silhouette is invariant under a translation along the view axis, so the shell's eye reproduces the witnessed image exactly while keeping the whole model sphere inside the RD clip band; reproducing the literal band would manufacture the unwitnessed clip. Reopen only with the retail render state that resolves the band |
| D-RLIT-9 | Godot device seams consumed the render-float (D3D-world) celestial/light tuple as if it were Godot world — the bases differ by the x/z swap `godot = (z, y, x)_render`, so the sun/moon/glare/glint bodies, the object directional term, the glare jitter plane, the sun-veil dot, the dome uniforms and the star placement all sat 90° off in yaw and mirrored; every low-sun frame front-lit where retail backlights | mission `(x, y, z)` -> render `(-y, z, x)` `[orig: Math_FixedPointToFloat3_YNegated @ 0x611210]`; the water-glint submit matrix independently confirms the map `[orig: update_sun_glare @ 0x5ad1ba..0x5ad213]` | **FIXED (2026-08-20, the 03tr-sun-sky fixture slice; id minted 2026-08-21)**: one mapping seam (`godot/src/env/env_axes.h`) applied at every consumer listed in the axis section above; the raw tuple stays on the `opennova_sun_direction` global and the terrain `u_sun_direction` uniform, which re-swizzle into the engine texture basis themselves and are byte-parity-verified there; measured by the registered `03tr-sun-sky` fixture |

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
| 0x606030 | sample_terrain_lightmap | sample_terrain_colormap_tinted | samples the CPU colormap (not the tile-set atlas or composed tile RT) × tint >>7; foliage instance colors |
| 0x58f290 | sub_58F290 | Render_FillStaticCubemaps | fills CubeNormalize + CubeRotSpecular at init (caps-gated) |
| 0x5899e0 | sub_5899E0 | Render_ApplyCubemapCapsDisableMask | clears Render_DeviceCapsFlags bits from the disable mask |
| 0x58b220 | generate_sky_cubemap | EnvCube_RenderFaceOrAnalyticFill | callback → live scene face render; no callback → the dead analytic 5-light fill |
| 0x2732e28 | unk_2732E28 | Light_InstanceTable | 176-B light records, handle-indexed |
| 0x272ed84/80 | dword_* | Lighting_InteriorGroupEntity/Section | the sample position's interior pair |
| 0x5a9fd0 | (undefined code) | LightPool_SpawnSpotProjectorEffect | defined 2026-08-20: the spot/projector spawner (flag 0x10000, projection matrix at instance bytes 88..152) — CALLER-LESS dead code in Jointops.exe |
| 0x5aa830 | render_foliage_instance | Light_SetupTerrainProjectedPass | 2026-08-21: the per-light terrain projected pass setup (slot index arg `@ 0x5aa844`, 32768/range scale, 0.66 × 0.5 colour, Light_TexSpot1D/Light2D, pass dword_2732DC4/DC8); nothing foliage about it |
| 0x5aa170 | (comment) | EffectWorld_TickInstancesAndLightScale | 2026-08-21 (the wire-up round): the entry comment reworded — the tail unpacks `Env_TerrainColorRecip` × 1/128 into the terrain pass's own `flt_2732DA{C,8,4}` triple; it does NOT derive `EffectWorld_AmbientScale` (that is the modulator × 1/64 `@ 0x5aaf1d..0x5aaf37`) |
| 0x5a92c0 / 0x5a94f0 / 0x58d947 / 0x6092a0 | (comments) | Texture_GenerateProceduralFalloffTexture / Lighting_InitTextures / Render_SetViewAndProjectionMatrices / render_terrain_sector_batch | 2026-08-21 (the wire-up round): the texel formulas, the inline 64×8 strip, the `flt_27219C0` camera→world writer, and the per-light loop's 16-cap / group-clear / alt-texture facts recorded as entry comments (no renames) |
| 0x5aaf40 | CEffect_RenderFoliageBillboards | EffectWorld_RenderLightCoronas | it walks Light_InstanceTable drawing additive corona billboards, not foliage |
| 0x5ab9d0 | collect_visible_foliage_slots | Light_SelectAndEnableForDraw | the LIVE per-draw light select: fills Light_VisibleHandles/Count from the drawn ENTITY's position ± boundRadius box and D3D-enables the first <= 4 (update_light_slots @ 0x5abc50 is the xref-less standalone variant); the batch entry keeps only the first THREE gate-passers (@ 0x5d9229 / @ 0x5d96e9) and FlushBatches tears the enable set down per entry (@ 0x5da5de) |
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
| 0x5d5690 | shadow_decal_alloc_slot | RenderSlot_AllocSlot | find-or-alloc + the LOD-at-alloc laws (2026-08-20) |
| 0x5d6530 | terrain_sort_and_assign_render_slots | RenderSlot_SortAndAssign | scoring, exclusions, 24-patch/12-RT binding (2026-08-20) |
| 0x5d5320 | init_render_target_chain | RenderSlot_InitTextureChain | the 12-RT halving size chain (2026-08-20) |
| 0x5d5130 | terrain_tile_rebuild_vertex_buffer | RenderSlot_RebuildPatchVertexBuffer | the 441-vertex terrain drape patch build (2026-08-20) |
| 0x5d6e20 | sub_5D6E20 | RenderSlot_DrawAllDrapes | the per-slot drape walk + local-FP/prone gates (2026-08-20) |
| 0x5d5ca0 | render_sector_model | RenderSlot_DrawSilhouetteDrape | the projected silhouette drape: fade, the depth-clip stage (person 4x steepens its direction), the sun ambient law (2026-08-20; clip stage 2026-08-21) |
| 0x5d59d0 | render_minimap_tile_overlay | RenderSlot_DrawAuthoredBlobDecal | the items.def `shadow` decal drape (2026-08-20) |
| 0x2be3c0c | dword_2BE3C0C | RenderSlot_DetailLevel | the shadow-detail level driving chain size + cadence (2026-08-20) |
| 0x2be3bb4 | dword_2BE3BB4 | RenderSlot_Count | live slot-record count (2026-08-20) |
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
