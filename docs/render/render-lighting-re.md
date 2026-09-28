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
`engine/formats/env/env_weather_core.cpp` (the ticked chain),
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
| Modulator → shader gain unpack (÷64, 64 = identity) | MATCHING | `renderer::unpack_modulator_scale` `[orig: Render_UnpackModulatorToLightScale @ 0x58db30 → g_RenderLightScaleR/G/B @ 0x8409f4..fc; EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaef0 → @ 0x840b24..2c]`; `renderer_state_vectors` section 5 (identity 0x404040 → exactly 1.0, 0xFF → 255/64) |
| The modulator chain (modulator2 → modulator → every color block, same tick) | MATCHING (ported; env #17 CLOSED) | `env::ModulatorChain` + the witnessed block order `[orig: Environment_UpdateWeatherTick block sequence @ 0x57ef97..0x57f03c]`; 62-tick target chase `[orig: @ 0x57e512..0x57e538; ColorBlock_SetStepDeltas @ 0x57d940]`; env vectors re-dumped surgically (exactly the 8 weather checkpoint rows; hand-check wb/k064 `0x31·61/64 = 0x2E`) |
| Iris auto-exposure sampling | MATCHING (full marched port) | the curve was already ported (env record §Iris); the 3-point camera-ray march is complete as of 2026-08-22: `Simulation::compute_iris_samples` clips the 8 u camera ray against terrain and the local player's candidate solids, samples thirds, classifies blink interiors only through that same fixed candidate slice, preserves the per-sample interior-group state transition, and casts all three outdoor rays against both pool-1 and pool-2 slice entries behind the local-player count gate; `WeatherCore::set_exposure_from_iris_samples` applies ceiling/floor indoors, ×level/8 sun outdoors, and INT /3 average `[orig: Environment_ApplyFogAndAmbient @ 0x57e440; Environment_ComputeAmbientLightAlongDirection @ 0x5c7a00; Terrain_SectorComputeLighting @ 0x5c7550; Entity_RaycastCollision @ 0x413760; Physics_RaycastFindCollisionEntity @ 0x539a70]`; the outdoor sample stays the no-world fallback |
| World lighting block (per-pass build + ctx store) | MATCHING (math ported; production-fed 2026-09-10) | `renderer::build_world_lighting` `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; RenderBatchCtx_StoreLightingConstants @ 0x5d89e0]` incl. the NVG hemi rewrite, applied by `EnvironmentState::build_light_values` to the four hemisphere colours (sky/ground per channel, ceiling/floor on the R term `@ 0x5c82a5..0x5c82e9`) under the `g_CameraMode == 0` gate (2026-09-24, "Keep the env colour blocks raw under NVG; rebuild the terrain NVG sky as bytes" and "Gate the NVG world treatment on camera mode 0"); the colour getters stay RAW, matching the consumers that read the raw blocks (`g_EnvWaterColorLit @ 0x57f177`, `g_EnvTerrainLightCombined @ 0x57f0d5`, the slot drape `@ 0x5d5f66..0x5d5f6d`), the thermal-view grey (`Player_IsOpticalViewVisible && Def flags2 & Thermal` — `Player_IsHeldWeaponThermal` reads EquippedSlot+0x118 → Def+0x20 → flags2+0x0C & 4; fed by `EnvironmentState::set_thermal_view` from the local view frame, only shipped trigger WPN_EMP50BD), the thermal frame's BySide-wave 0.25 block (the bool arg; NOT an NVG dim; retail brackets BOTH BySide calls with it, `@ 0x5c9511` and `@ 0x5c95f8`, and the port carries it on the live per-instance lane `u_entity_light.w` + `u_thermal_wave_draw` in `shared.gdshaderinc`), and the two hemisphere averages; the terrain leg `Render_TerrainScene @ 0x610e51..0x610e65` (0x101010/0xF0F0F0 ramps, def bit + camera mode 0, NVG outranks) rides `build_terrain_uniforms`; `renderer_state_vectors` section 5 |
| Per-entity uniforms (slots 227-230) + interior daylight lerp | MATCHING (math ported; reimpl transfer wired; delivery shape ported 2026-08-26) | `renderer::compute_entity_lighting` `[orig: Render_SetupEntityLightingAndShaderConstants @ 0x5d98a0]`; the aux float = the containing building's ItemDef+0x218 `light_transfer` / 100 (written only by `ItemDef_ParseProperty @ 0x4a1a2c..0x4a1a50`), NOT a dual-LOD fade. The reimpl parses `items.def light_transfer` as a clamped percentage, carries it through `ItemDatabase`, and (2026-09-24, "Give every contained drawn entity the interior lighting lerp and group") applies the witnessed wave split: every building's ROBJ 1+ lerps by its own transfer, a contained person by its building's, a contained non-person with t = 0 (see the per-entity reader and the "Hosted `light_transfer` wiring" section). Delivery follows retail's split: the world block is the per-pass constant set (`opennova_light_block_*` global shader parameters written by `MissionEnvironment` once per env change, the `RenderBatchCtx` block), and the per-entry factors (effectScale, interior flag, daylight t) are one `u_entity_light` instance uniform stamped on the entity's surface instances on change; `shared.gdshaderinc` evaluates the same lerp/scale per draw, with `renderer::compute_entity_lighting` as the pinned oracle. No material carries lighting or fog state and nothing restamps per frame |
| Object NORMAL-pass lighting | MATCHING except named cube-content residual | Fixed function: `renderer::ff_vertex_light` + checked-in `fixed`/`flag` techniques reproduce ambient + directional + hemisphere delta lights and selected point lights at vertex rate, summed and saturated per vertex, Gouraud-interpolated, then MODULATE2X `[orig: Lighting_SetHemisphereD3DLights @ 0x5d8cb0; D3D light 0 @ 0x5d9ce2..0x5d9d76; _FFP.fx TBoringFFP]`. Only the D3D fixed-function point lights carry the 1.5× diffuse boost; every shader pass reads the unboosted `Light_GetPointLightParams` colour (2026-09-24, "Deliver the unboosted point-light colour; keep the 1.5 on the FF lights"; see the EffectWorld witness below). Highest-quality DOT3/Phong effects retain their authored mapped-normal pixel N.L/N.H. Source-specific vertex factors stay distinct: ordinary bump/Phong effects carry attenuation × geometric self-shadow, BDiffT2's point stage carries attenuation only, and SkBDiffT2/SkBDiffO2 also use geometric/Gouraud hemisphere with no directional self-shadow `[orig: _vsDiffT/O, _vsPhongT/O, _vsSkDfT/O, _vsSkPhT/O; BDiffT2.fx; SkBDiffT2.fx; SkBDiffO2.fx; _psDiff.fx; _psPhong.fx]`. The lit skinned effects take every light into the vertex's first palette entry's frame against the undeformed tangent frame, their point lights measured from the vertex that entry carries, while SkBasic lights the blended normal (2026-09-27; [3di-gp-format-re.md](../threedi/3di-gp-format-re.md) "Retail skinned vertex blend"). The 26-technique decoded-source ledger and raster response matrix are `object/technique_validation.json` + the `render_swatch` probe. |
| Per-entity sun visibility (effectScale source) | MATCHING (all roles, 2026-08-23) | `renderer::sun_visibility_factor` `[orig: Entity_ComputeSunVisibility @ 0x5c6800; stack write @ 0x5c7fa5]`; local/authority rows use `world::CollisionWorld::sun_visibility_blocked_rays`, while decoded rows use `wire_sun_visibility_blocked_rays` over a separately wire-keyed candidate arena rebuilt on the same 17-tick edge. Both cast the witnessed one-segment/three-radius query from the exact scaled bbox midpoint and only against the source's own `+0x1BC`/`+0x1C0`-equivalent slice. `Simulation::draw_lighting_changes` (ex `get_draw_lighting_changes`, over `inmatch::EntityLightingFeed` since 2026-09-24) emits the typed per-identity context; `EntityPresenter::set_entity_lighting_context` caches the factor across cold body/held-weapon construction. The local player's third-person body dims while FP parts keep the witnessed effectScale=1 exemption (D-RLIT-3). |
| EffectWorld dynamic point lights (instance pool, spawn/query/select, color × modulator × RgbGen, {1,0,15/r²,1}, ≤3 per batch entry, owner/interior groups, fade/decay lifecycle, transient spawners, coronas) | **MATCHING on the locked highest-quality path (≤3 per strip) (2026-08-23; Spot/Target and foliage delivery premises witnessed DEAD)** | `renderer::LightScene` hosts the 4096×176B pool, safe generation leases, target-disable gates, object first-63 overlap query (general/terrain first-64), group-passing nearest-THREE select (the batch-entry cap; the 4 of `Light_SelectAndEnableForDraw` is the transient D3D enable count), and per-draw delivery `[orig: g_LightInstanceTable @0x2732e28; Light_CollectNearbyZonesByAABB @0x5aa250; Light_SelectAndEnableForDraw @0x5ab9d0; Render_CollectRenderObjectsForBatch @0x5d9229; CRenderBatchQueue_FlushBatches @0x5da26b/@0x5da5de]`. `EffectLightDirector` routes mission-start and powerup-respawn LGHT plus four transient families. Authored positions use the entity matrix once and remain spawn-fixed; `subobject` selects an owner section only. The final spawned handle shares entity+0x1B4 with muzzle glow, a husk swap does not rescan, and entity removal clears only that final handle `[orig: Entity_SpawnGlowEffects @0x56c82d..0x56c92c; powerup callback @0x442b40/@0x442ba0 registered @0x442ce6; Entity_UpdateMuzzleGlowEffect @0x56c960; Entity_Destroy @0x43e903..0x43e916]`. BUILDING ObjectModels and static atlas rows share one initialized entity query cube across their ROBJ owner sections; static props carry containing-blink groups (D-RLIT-11, minted and closed 2026-09-13). Native/GUT tests pin selection, isolation, lifecycle, and replacement; the 26-technique Forward+ D3D12 probe proves atlas/live raster identity. Coronas and terrain projected lights are ported end to end (the terrain lights on the served `ps.1.1` pass since 2026-09-26) `[orig: EffectWorld_RenderLightCoronas @0x5aaf40; Light_SetupTerrainProjectedPassPS @0x5aab30]`. The apparent foliage leg is inert in retail's max-quality program: the call at `Foliage_RenderDetailPatches @0x60a5dc` enables D3D lights, but `g_FoliageWindSwayVS` declares no normal/light input and writes `oD0=c6`, leaving no consumer outside the failed-VS fixed-function fallback `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupDetailSlotDraw @0x60087a]`. |
| Model-authored `LGHT` chunks | **MATCHING on the runtime path (2026-08-23)**: consumed at spawn via EffectWorld, never via per-material uniforms | `Entity_SpawnGlowEffects @0x56c7c0` walks the model's light array (count +0xC4, records +0xC8, stride 120) and spawns one instance per record: entity-matrix position, white base color, radius = atten_end × 65536, RGB-gen block, subobject/blink owner group, and disable flags 512/1024/2048 `[orig: @0x56c82d..0x56c92c]`. There is no authored-light position update caller and no husk-model rescan. The per-material `u_local_light_*` path has no gameplay caller (`u_local_light_count = 0`); gameplay illumination flows through `renderer::LightScene`. |
| Terrain surface c0/c1 | MATCHING (ported) | c0 = SKY block, c1 = LIGHT block (both [0] ÷255): `renderer::terrain_surface_light`, `terrain_lighting.gdshaderinc` corrected from the gobj-era combined/fill guess `[orig: Terrain_SetupLightingAndShader @ 0x604420; Terrain_InitLightingColorRamps @ 0x604ee0 ← Render_TerrainScene @ 0x610c80]` |
| Static sector/model sun shadows onto terrain/foliage | WITNESSED / hosted page-alpha subset | pool-2 buildings cast unless `NoShadow`; pool-1 items additionally require `StaticShadow`; every ROBJ in the selected LOD enters a black PROJSHAD temporary RT which is composited into terrain-tile alpha, not back onto sector models. Runtime now collects typed static sources, resolves selected LOD/all-ROBJ geometry and every diffuse-alpha frame, evaluates AlphaGen/full UV transforms and time/control flipbooks through the ordinary-object runtime functions, rasterizes A-only projections into the shared terrain/foliage page, and retires the global directional surrogate. The max-quality c7/c8 projection, skinned rigid collapse, material pass state, animation evaluator, and final ONE/ONE composite are exact `[orig: c7/c8 @ 0x60A220..0x60A34F; Terrain_CollectAndRenderTileModels @ 0x60D250; submit tick @ 0x5DAD9D; batch CTRL snapshot/restore @ 0x5D91AB..0x5D91DE / 0x5DA1B8..0x5DA1FD; material consumer @ 0x58DB80; PolyTrn_RenderTile composite @ 0x60E0C6..0x60E19D]`; the scorch/order and cache cadence/edge/address/mip tails closed with D-TERRAIN-7 (2026-09-24 and 2026-09-26: the page samples CLAMP at every pass), while whole-process CTRL/RNG ordering remains D-3DI-2 |
| Foliage/sector-model lighting constants | MATCHING (witnessed; values and max-quality non-response pinned) | the blend PS inherits the terrain device's c0/c1 and consumes cached-tile `t1.a*c1+c0`; `g_FoliageWindSwayVS` writes `oD0=c6` and declares no normal/light input. The per-patch `Light_SelectAndEnableForDraw @0x60a5dc` call therefore mutates state with no consumer while that VS is bound; only the excluded failed-VS FVF fallback could consume it `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupDetailSlotDraw @0x60087a; Foliage_RenderDetailPatches @0x609de0]`. |
| Lighting textures + DOT3 dynamic-light shader | witnessed; the DOT3 program PORTED for the terrain pool lights (2026-09-26) | procedural falloff set + the last embedded PS outside FrameFX `[orig: Lighting_InitTextures @ 0x5a94f0]` — the ps.1.1 DOT3 program is the terrain pool-light pass (t0 the cube-normalize map, t1 the detail coefficient map, t2/t3 `texlight2d`), served with its textures by the terrain projected pass below |
| Cubemap and Phong lookup sources | MATCHING | the highest-quality 256² CubeEnvironment shell synchronously re-renders the exact sky + sun/moon callback on all six faces every 128 frames, applies the 0x60 gamma-byte dim (the faces reach the published RD cubemap through a RenderingDevice copy leg, no CPU readback), and adds the rotated static lobe `[orig: Render_InitTextures @ 0x58f6e0; EnvCube_Update @ 0x6106a0; callback @ 0x5c3700]`; the Forward+ D3D12 axis/orientation/byte probe validates the final samplerCube. Static sun-glint cube = white pow-800 + warm pow-40 along −Z, rotated by MatRotSpecular `[orig: Render_FillStaticCubemaps @ 0x58f290 → GTexture_GenerateCubeMapLighting @ 0x685bb0]`; normalization cube `[orig: GTexture_GenerateNormalMapCubeMap @ 0x685570]`; `Render_CreateSystemTextures @ 0x58aca0` creates the exact 256×256 `gsys_phong` lookup with N.H exponents 4/16/64 in RGB and N.L in alpha. |
| Render-slot (entity ground shadow) pipeline | **PORTED (2026-09-24 rendering parity pass; the 2026-08-24 03TR low-sun symptom not reproduced, see "The render-slot side")** | the full slot family is witnessed and hosted: admission for people (and the local player) or ItemDef `DynamicShadow`, not gated by `NoShadow`, attached third-person weapons joining their entity and the first-person viewmodel never casting `[orig: Entity_InitFromModel @ 0x40E1BC..0x40E1F7]`; frame-open sun default with the 0.25 vertical clamp then negation `[orig: Render_ShadowPass @ 0x5d7b70]`, slot registration + LOD `[orig: RenderSlot_AllocSlot @ 0x5d5690]`, priority scoring / 24-patch / 12-RT assignment `[orig: RenderSlot_SortAndAssign @ 0x5d6530]`, RT size chain `[orig: RenderSlot_InitTextureChain @ 0x5d5320]`, dominant-light pick + march start + anchor march `[orig: RenderSlot_UpdateEntityLight @ 0x5d6a30]`, refresh cadence and the unlit black capture `[orig: RenderSlot_RenderEntityAndChildren @ 0x5d7690]` (the receiver lighting `RenderSlot_SetupSectorReceiverPass_Dead @ 0x5d7250` is dead behind the always-zero `RenderSlot_CollectReceiverSlots_Stub @ 0x5d7240`), and the per-slot patch-mesh terrain drape `[orig: RenderSlot_DrawAllDrapes @ 0x5d6e20; RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0; RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130]` (the authored blob leg `RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0` is dead in JO). Planning/color laws portable in `engine/runtime/renderer/render_slot_shadow` (ctest `renderer_render_slot_shadow`) with the PROJSHAD coverage and blend-state tables in `object_shader_template` (ctest `renderer_material_classify`); the device (since 2026-08-29) is `godot/src/env/slot_shadow.cpp` publishing one typed request per armed slot, `godot/src/render/slot_capture_adapter.cpp` drawing them as a PRE_OPAQUE RenderingDevice pass into the twelve resolve targets (the retail white clear, black under the technique's coverage and blend state, GPU bone-palette skinning) and `godot/shaders/slot_shadow_drape.gdshader` sampling them (GUT `slot_shadow_test` headless + windowed, `sun_shadow_test`). This supersedes the earlier ADR-0023-era "Shadow_/Scar_ family exclusion" note for the RenderSlot_* half; the Scar_ decal family remains out of REN scope. The 2026-09-24 pass ported the per-slot fade/cull/fog, the per-slot patch mesh and lift, the march start, the attached-light drape, the capture axes and the Flags-&-1 slot gate; since 2026-09-26 the drape draws at `kRungSlotDrape` over a stencil mark of the terrain and the dome, the order retail's later draws give it, and closes with the terrain gate indoors (*The port*); the device folds are D-RLIT-10's capture eye, the multisampled capture and the display-frame pick cadence. |

**2026-08-22 object-point-light correction.** The broad "point lighting is
vertex-rate" wording in the historical EffectWorld row applies only to fixed
function. The highest-quality `.fx` NORMAL passes settle the previously open
VS-technique question: DOT3/Phong techniques run one authored point pass per
selected light (`PASSRULE_ONCE_PER_POINTLIGHT`). N.L/N.H uses the mapped normal
in the pixel stage; `CalcPointLightAttenuation` and `CalcSelfShadowTerm` run in
the vertex shader and interpolate in oD0. OpenNova now preserves that split
with the vertex-rate point sum folded into `v_ff_diffuse` for fixed function
and `v_pixel_point_factor` for pixel-shader effects that author geometric
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
edge** — the 16 `Environment_InterpolateWeatherColor` calls at
`[orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c]` (decoded from
the `mov ecx, imm32` block addresses), so the exposure propagates SAME-TICK.
The modulator's target is the iris sample replicated to gray
(`0x10101 × gain`) and chased over 62 ticks: `ColorBlock_SetStepDeltas
@ 0x57d940` sets each channel's max step to
`|target_byte<<20 + frames/2 − current| / frames`
`[orig: Environment_ApplyFogAndAmbient @ 0x57e512..0x57e538]`. Retail
re-targets on every render pass (so every tick) while a local player exists and
the WAC `autogain` value is nonzero (`@0x57E50B..0x57E51B`; seeded 1, so the gate
is open unless a script writes 0), else the modulator keeps its last target
(env-tod-re, the fog-and-ambient walk row 8; ported 2026-09-23). `Environment_ComputeAmbientLightAlongDirection
@ 0x5c7a00` produces the gain (fully pinned 2026-07-18, ported at the marched-iris
slice):

- **Ray**: end = camera + camera-forward × 8.0 — the `(0x80000, 0, 0)` vector
  rotated through the camera Euler matrix (camera globals @ 0xA78364..78)
  `[orig: @ 0x5c7a56..0x5c7a6f]`, then `Entity_RaycastCollision @ 0x413760`
  clips the end IN PLACE: terrain first (`Terrain_RaycastHeightmapHiRes_0
  @ 0x60e710`, called `(start, end, end)`), then the entity collision-model
  nearest hit `[orig: the Flags & 0x800000 indoors gate skips the terrain leg
  @ 0x413785]`.
- **March**: samples at end, end + (cam−end)/3, end + 2(cam−end)/3 (truncating
  integer thirds) `[orig: @ 0x5c7ad8..0x5c7b30]`; the iris curve runs
  PER SAMPLE and the three INT gains average `(s0+s1+s2)/3`
  `[orig: @ 0x5c7b45]`.
- **Per sample** (`Terrain_SectorComputeLighting @ 0x5c7550`): its first
  argument is the **local-player entity**, not a terrain sector (the caller
  loads `g_LocalPlayerEntity` and pushes it at `Environment_ApplyFogAndAmbient
  @ 0x57e51d`). The function reuses that entity's fixed
  `+0x1BC`/`+0x1C0` candidate slice for every sample and tests only type-5
  building entries
  (`Entity_TestCollisionSections @ 0x4aef90`, flags 0x8000). INDOOR (hit slot 0
  nonzero): pool-2 entity = `hit >> 20`; **no render model (`pool_entry[12]`,
  the entity's +0x30 render-model pointer, == 0) short-circuits to the curve
  with ALL inputs zero — the ÷2m limb diverges and the clamp serves gain 255**
  `[orig: @ 0x5c7652]` (corrected 2026-09-24: +0x30 is the model pointer the
  collectors skip model-less entities on, `Terrain_CollectVisibleEntitiesForTerrain
  @ 0x5c8cf6..0x5c8cff`, `Terrain_CollectVisibleSectorUserpoints @ 0x5c6c3f`, and
  `Terrain_RenderSectorModels` reads as model data `@ 0x5c5dac`, not interior
  data); else
  directional = 0, sky/ground ← the CEILING/FLOOR blocks' [1] slots
  (@ 0x26C6474 / @ 0x26C64DC), interior light group = `(hit >> 12) & 0x1F`
  (`Lighting_SetInteriorLightGroup @ 0x5a90e0`, pair @ 0x272ED84/0x272ED80).
  OUTDOOR: sun level = 8 − one per BLOCKED sun ray — three entity-only
  raycasts (`Physics_RaycastFindCollisionEntity @ 0x539a70` with allowAllTypes = 1,
  hit → `neg/sbb/add 1` → −1) from the sample to sample + 200 × light_dir,
  clip radii −0x2000/−0x5000/−0x8000 `[orig: @ 0x5c7765..0x5c77d7]`, gated on
  the local player's candidate count (`entity[112]`, byte offset `+0x1C0`,
  @ 0x5c7707 — with an empty slice the rays cannot hit); directional = light
  block [1] × level/8 (`light_scale = level × 1/2040 @ 0x5c77e9`), sky/ground
  = sky/ground [1]; light group cleared (0, 0). Then the iris curve
  ([env-tod-re.md](../env/env-tod-re.md) §Iris auto-exposure; luminance
  weights 0.25/0.5/0.25).

- **The candidate-scoped sun walker** (`Physics_RaycastFindCollisionEntity
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
  dynamics and pool-2 statics both block); `Physics_RaycastAgainstEntityPool
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
`g_RenderLightScaleR/G/B @ 0x8409f4..fc` = modulator bytes ÷ 64; its SOLE
consumer is `Material_ApplyShaderParameters @ 0x58e05d` binding **ColorSrcGlobalGain
(handle slot 232)** — in the shipped `.fx` corpus the SELFLUM emissive
(`SelfLumColor × ColorSrcGlobalGain`) and the glass / mirror FFP emissive
(`ReflectColor × ColorSrcGlobalGain`), both saturated by the lighting stage
before MODULATE2X (`_FFP.fx` SELFLUM block, `Glass.fx TGlassFFP`; gain bind
`Material_ApplyShaderParameters @ 0x58e050..0x58e06a`). `EffectWorld_UnpackModulatorToAmbientScale
@ 0x5aaef0` writes `g_EffectWorldAmbientScaleR/G/B @ 0x840b24..2c`, consumed
by `Light_GetPointLightParams @ 0x5a9180`, `Light_FillD3DPointLight
@ 0x5aa450`, `Light_SetupTerrainProjectedPass @ 0x5aa830` (ex `render_foliage_instance`, renamed 2026-08-21 — the per-light terrain projected pass, its argument a `g_LightInstanceTable` slot),
`Light_SetupTerrainProjectedPassPS @ 0x5aab30`, and
`EffectWorld_RenderLightCoronas @ 0x5aaf40` (ex CEffect_RenderFoliageBillboards,
renamed 2026-08-20 — it renders light coronas, not foliage) — dynamic lights
and foliage/effects brightness ride the exposure.

**The world lighting block (writer).** `CTerrainRenderer_BuildLightingShaderConstants
@ 0x5c8090` (callers: `Terrain_RenderWorldScene @ 0x5c93a0` ×5 —
per-wave, mirrored and unmirrored — `Player_RenderFirstPersonViewModel
@ 0x4deea4`, and the offscreen `Water_RenderReflectedWorldScene`) fills a 32-float block from the
env blocks' **[0] render colors** (post-modulator — the exposure reaches
world lighting through the block colors themselves) ÷255: [0] dir-enable
flag, [1..3] ← `g_EnvLightBlock`, [5..7] ← −normalize(`Environment_GetLightDirectionFloat
@ 0x57d870`), [8..10] ← `g_EnvSkyBlock`, [12..14] ← `g_EnvGroundBlock`,
[16..18] ← `g_EnvFloorBlock`, [20..22] ← `g_EnvCeilingBlock`. Overrides: the
NVG hemisphere rewrite (`g_NVGActive @ 0xb7654c` && `g_CameraMode @ 0xa890c8`
== 0, the camera mode written by `Camera_SetTrackedEntity @ 0x439267 /
0x4392c3`: 0 first person, 1 chase, 3 overhead, 4 death lerp):
`c' = c·0.25f' + modulator_byte·f'/640`, `f' = (g_NVGBrightnessLevel+1)·0.2`
(`@ 0x5c8205..0x5c82e9`, ceiling/floor included — R term on all six channels).
Every NVG world/post leg shares that gate
(`CTerrainRenderer_BuildLightingShaderConstants @ 0x5c81c4..0x5c8205`,
`Render_TerrainScene @ 0x610cfc..0x610d10`, `Render_ProcessMainSceneFrame
@ 0x5ca6ab..0x5ca6bf`), so the death lerp camera (mode 4) drops the
treatment; port `world::player_view_nvg_visible` (`camera_mode == 0`,
2026-09-24 "Gate the NVG world treatment on camera mode 0"). The rewrite is the
object block's (and the terrain sky's, below) only: `EnvironmentState::
build_light_values` applies it to the four hemisphere colours, the colour
getters stay raw, and water colour, the underwater fog, particle ambient,
scars and the slot drape are NVG-invariant (2026-09-24, "Keep the env colour
blocks raw under NVG; rebuild the terrain NVG sky as bytes"). The sky dome's
colour unpack `Color_UnpackToFloat4 @ 0x578900` reads the same NVG pair:
f = (level+1) × 0.2, channel = byte × 2 × 0.25 / 255 + f × 0.0015625
(`@ 0x578913..0x57897b`); the normal branch is byte × 2/255, unclamped,
a = 1 (`@ 0x578985..0x5789c3`); ported in `runtime/environment/sky_frame.cpp`
(`sky_constant`), gated by `EnvironmentState::nvg_view_active()` (2026-09-24,
"Dim the sky dome under NVG, whiten it under thermal, exact light blend"). The
thermal-view grey (`Player_IsOpticalViewVisible` &&
`Player_IsHeldWeaponThermal` = equipped Def flags2 & Thermal(4), no scope test): dir 0.1, all hemis 0.5, flag cleared
(`@ 0x5c8389..0x5c843c`), latched per frame `@ 0x5ca2da..0x5ca2e3` and driving the 0x808080 clear/fog; the thermal BySide-wave block (the bool arg, formerly misread as an NVG world dim): everything 0.25,
dir zeroed (`@ 0x5c8448..0x5c84f0`). `RenderBatchCtx_StoreLightingConstants
@ 0x5d89e0` (ex-misnomer `set_bounding_volume_from_box`) copies the block to
batch-ctx +112 and derives **ctx+208 = (ceiling+floor)/2** and **ctx+224 =
(sky+ground)/2** (the AmbientColor source); flag 0 zeroes the dir color.

**The per-entity reader.** `Render_SetupEntityLightingAndShaderConstants
@ 0x5d98a0` (per FlushBatches entry) pushes the uniform surface — the handle
slots pinned by their `GetParameterByName` stores
`[orig: HLSLEffect_LoadFromFile @ 0x5af3fe..0x5af485]`: **225 CameraPos,
226 DirLightVector, 227 DirLightColor, 228 HemiGroundColor, 229 HemiSkyColor,
230 AmbientColor**. Outdoor path: 227 ← ctx dir color × entry[9]
(effectScale), 228/229/230 ← ground/sky/outdoor-average direct. Under batch
entry flag bit 1 (submit 0x80 for a contained entity and the viewmodel inside
a building; a building's own `push 40h` submits, which the rigid collector
turns into bit 1 for ROBJ != 0 only): 227 additionally × entry[10],
228 ← lerp(floor, ground, t), 229 ← lerp(ceiling, sky, t),
230 ← lerp(indoorAvg, outdoorAvg, t) where **t = entry[10] = the
render-state stack aux**: the containing building's ItemDef+0x218 =
`light_transfer` / 100 (`atol` clamped 0..100 × `flt_7C56A8` = 0.01, written
only by `ItemDef_ParseProperty @ 0x4a1a2c..0x4a1a50`; corrected 2026-09-24,
formerly read as "interior model data +536"). Exactly three sites write the
aux (a full indexed-operand scan): the viewmodel `[orig: @ 0x4def3c]`, the
building batch with its OWN transfer `[orig: Terrain_RenderSectorModels
@ 0x5c5df2..0x5c5e00]`, and the person wave with the parent pool-2 entry's
transfer when its ItemDef is non-null `[orig: Terrain_RenderSectorEntitiesBySide
@ 0x5c7f7a..0x5c7f93]`. The wave split (2026-09-24): the person wave submits
0x80 when entity+0x1D0 != 0 `[orig: @ 0x5c7fb6..0x5c7fc3]`, so a contained
person lerps by its building's `light_transfer`; the non-person wave sets the
same 0x80 `[orig: Terrain_RenderSectorEntities @ 0x5c7c05..0x5c7c14]` but
never writes the aux, which stays the stack base's 0 `[orig:
RenderBatchCtx_BeginFrame @ 0x5d89b6..0x5d89b8]`, so a contained vehicle or
prop gets floor/ceiling only, no sun; every building's ROBJ 1+ (not only
portal buildings) lerps by its own transfer, t = 0 when unauthored `[orig:
Terrain_RenderSectorModels push 40h @ 0x5c5f1f (and its other two submit
sites); Render_CollectRenderObjectsForBatch @ 0x5d9156..0x5d9162]` (type-5
buildings draw only through the building batch:
`Terrain_CollectVisibleEntitiesForTerrain` skips type 5 `@ 0x5c8dd7..0x5c8de0`,
`Terrain_CollectVisibleSectorUserpoints` admits every static-prox building with a
model `@ 0x5c6c3f..0x5c6c43`). A contained entity takes effectScale 1.0
`[orig: Terrain_SetupEffectForEntity @ 0x5c74a3 / @ 0x5c74b7]`. Statics are
contained too: `Game_StartMission` -> `Entity_BuildProximityListsForPools12
@ 0x5240c5 / @ 0x5240fa` runs `Entity_BuildProximityList` (which writes
+0x1D0 `@ 0x4b406b`) for pool 1 and non-building pool 2, and the client runs
the same list for replicated entities (`NetPacket_HandleEntityCreate
@ 0x42f227`, `Entity_UpdatePool1Slot @ 0x4b8e25`, `Entity_UpdateAllEntities
@ 0x4c229c`). A placed record is a building to the renderer and the light
spawner by its items.def TYPE (`Entity_BuildProximityLists_Pool2 @ 0x4b946e /
0x4b9502`; `Entity_SpawnGlowEffects` `cmp [eax+5Ch], 5` `@ 0x56c7e8`), not by
its BMS record family: a pool-2 decoration or foliage row is an entity, so its
static-row lane is the entity wave's (the 0x80 lerp over daylight 0 when
contained, `Terrain_SetupEffectForEntity @ 0x5c74a0`, `@ 0x5c7c05`), its LGHT
records run the blink query, and its mirror clip is the entity form. Reimpl
(2026-09-27): `mission::placed_record_is_building`; `EffectLightDirector` and
the placer's mirror-clip wave use it. GUT `static_decoration_lighting_test.gd`
(windowed; the CP03 Armry03 crate in HN_Bld1, which drew brighter than retail
while its BMS family made it a building). **entry[9] = the sun-visibility factor**.
`Entity_ComputeSunVisibility @ 0x5c6800` returns 1.0 when the entity's
proximity-source count at +0x1C0 is zero. Otherwise its origin is entity
position plus the uniformly scaled collision-AABB midpoint stored at
+0x1FC..+0x204, and its endpoint is 200 units along the active fixed light
direction. It calls `Physics_RaycastFindCollisionEntity @ 0x539a70` at radii
−0x2000, −0x5000, and −0x8000; zero means blocked, and the result is
(4−blocked)×0.25, written into the pushed stack top at 0x5c7fa5. The midpoint
and signed-rounding scale are built by `Entity_InitFromModel` at
0x40df2e..0x40e03c; raw type 6 with ItemDef attrib 0x20 uses a zero center
instead (`@ 0x40defc..0x40df16`). `Entity_BuildProximityListsFromPools
@ 0x4b8eb0` creates source slices only for pool 0 and eligible active pool 1
entities (the attrib-0x20 exclusion is waived for item type 1), so pool-2
statics naturally remain at full sun. An interior-parent reference at +0x1D0
bypasses the rays and keeps 1.0
`[orig: Terrain_SetupEffectForEntity @ 0x5c74ae..0x5c74f3]`. The reader
then fills **D3D light 0** (directional, diffuse = the scaled dir color,
direction = ctx dir or the defaults `g_DefaultLightDir{X,Y,Z} @ 0x8437e0`
= {0,1,0} when the flag is clear — with the color already zeroed) and calls
`Lighting_SetHemisphereD3DLights @ 0x5d8cb0` (ex-misnomer
`setup_d3d_clip_planes`): **D3D light 2** = directional {0,−1,0} with
diffuse = sky − ambientAvg, **D3D light 3** = {0,+1,0} with ground −
ambientAvg — the fixed-function hemisphere as two DELTA lights pivoting on
the average (identically `mix(ground, sky, N.y·0.5+0.5)`). ctx+841
(`RenderBatchQueues+0x349`) is a per-flush FIRST-ENTRY latch, not a mirror
gate (corrected 2026-09-24): `RenderBatchCtx_BeginFrame @ 0x5d89c4` and every
`CRenderBatchQueue_SortAndFlush @ 0x5dae4e` set it, the reader tests it
`@ 0x5d98c9` and clears it at the first entry `@ 0x5d9f34`. On the first entry
of every flush slot 226 gets the re-negated direction −(ctx+0x84..0x8C) with
w = 0.8 (`flt_7C6F9C`), with MatTexClipPlane and FloatTicks
`[orig: Render_SetupEntityLightingAndShaderConstants @ 0x5d995f..0x5d99a6]`, so
Flag.fx and the VS effects always read the live direction; the FF path lights
through D3D light 0. The FF hemisphere D3D lights 2/3 (and LightEnable 0) are
per-flush state too: they are re-set only on the first entry and on an
interior/outdoor transition, tracked by ctx+0x34A. The FF hemisphere latch
(`@ 0x5d9d78..0x5d9da2`, `@ 0x5d9eee..0x5d9f14`) inherits the first interior
entry of a run in flush order; that order's key carries residual stack bits
15..31 (`@ 0x5d92b9..0x5d92c2`), so the sharing is part of the D-RORD-6
garbage register ([render-order-re.md](render-order-re.md)), not a separate
divergence; the port lights each draw by its own lerp. The combined FF vertex
diffuse is summed and saturated PER VERTEX and the saturated colour
Gouraud-interpolates (`v_ff_diffuse`); SELFLUM saturates
SelfLumColor × gain the same way; the output stage doubles it:
`TSSColor(0, Modulate2x, Texture, Diffuse)` — **pixel = tex × min(lit,1) × 2**
(VS_TRACER alone modulates 1×).

**Hosted `light_transfer` wiring (2026-07-29).** The `items.def` parser now
recognizes `light_transfer`, clamps the authored integer percentage to
`0..100`, and stores its normalized `0.0..1.0` value through the def FFI and
`ItemDatabase` (Ihq01 is the shipped witness: `20` → `0.2`). Mission
placement supplies that value before building the model's section materials.
The building split leaves ROBJ 0 on outdoor lighting and interpolates ROBJ 1+
toward the floor/ceiling blocks by the building's own transfer (every
building, not only portal buildings; corrected 2026-09-24, see the per-entity
reader). A player inside the
building's blink volume, including first-person arms/weapon lighting, inherits
the same parent value even when the coarse `local_player_indoors` state is
false. This closed the interior half of D-RLIT-3; the outdoor half landed
2026-08-18 — the occlusion frame now runs retail's one-segment/three-radius
sun query per drawn entity against the entity's OWN proximity-candidate
slice (`world::CollisionWorld::sun_visibility_blocked_rays` — the
`Physics_RaycastFindCollisionEntity @ 0x539a70` walker iterates entity_a's
`+0x1BC` arena block bounded by the `+0x1C0` count, so only pool-1/pool-2
solids whose inflated sphere overlaps the entity's bubble ever block; a far building's
long shadow leaves it in full sun; clip radii −0x2000/−0x5000/−0x8000 shared
with the iris march) and applies the `(4 − blocked) × 0.25` factor through
`ObjectModel.set_entity_lighting_context` as a per-bms change list.
Pool-2 statics, slice-less rows, and the FP viewmodel keep the witnessed
full-sun default. Since 2026-09-24 ("Give every contained drawn entity the
interior lighting lerp and group") the per-drawn-identity feed is
`inmatch::EntityLightingFeed` (`engine/runtime/inmatch/role_feeds.h`, ex
SunQualityFeed): it emits (quality, interior, t, interior building BMS id,
section) per drawn identity, placed and wire, following the wave split above
(a contained entity keeps factor 1.0, takes the 0x80 lerp and its building's
interior light group; only a contained person carries the building's
transfer). Joiner rows without a registry twin resolve containment through
`CollisionWorld::query_wire_blink_boxes_at_point` (the def type 1/3 candidate
walk `[orig: Entity_BuildProximityList @ 0x4b3f3e..0x4b3f93]`, else the static
building prefix `[orig: @ 0x4b3e65..0x4b3f39]`); static MultiMesh rows carry
the lane in the atlas row's last texel (`renderer::static_row_entity_lighting`).
The feed replaces `Simulation::get_entity_interior_groups` (removed): the
director reads the interior group off the model stamp
(`ObjectModel::set_interior_light_group`), so wire rows now get their interior
light group for the point-light select too.

**EffectWorld dynamic point lights.** 176-byte records in `g_LightInstanceTable
@ 0x2732e28`, handle-indexed; per-frame in-frustum handles in
`g_LightVisibleHandles @ 0x272eda0` (count @ 0x272ed98). Params
(`Light_GetPointLightParams @ 0x5a9180`): position fixed→float Y-negated,
pos.w = 65536/range_fixed, color = record RGB × `EffectWorld_AmbientScale` ×
intensity (+ optional RgbGen animation via `RgbGen_EvaluateColor @ 0x5b23d0`,
gen block ticked by `Light_TickGenBlock @ 0x5a8ae0`), attenuation
`{1, 0, 15/range², 1}` with range = fixed × 1.25/65536 — the same set the
OED preview emits (`PrepareLightParams @ 0x46A500` in ModSuperOed.exe). `Light_FillD3DPointLight
@ 0x5aa450` fills a D3DLIGHT9 (type POINT) with the same values and a
**1.5× diffuse boost** (`flt_7D4B80` `@ 0x5aa4b2`). Only the D3D
fixed-function lights carry it: the per-frame SetLight upload
(`Render_ProcessMainSceneFrame @ 0x5ca6a1` -> `Light_UploadAllD3DPointLights` ->
`Light_FillD3DPointLight`) and the flush's re-enable (`Light_ApplyAsD3DLight
@ 0x5da61a`). Every shader pass (DOT3/Phong/env/mirror) reads the unboosted
`Light_GetPointLightParams` colour (the `POINTLIGHT_VARIATIONS` arrays
`@ 0x5da6a8`, the per-light pass `@ 0x5da8ae`; decoded `_vsDiffT.fx`:
`Out.Diff = PointLightColor * selfshadowterm * atten`). Port (2026-09-24,
"Deliver the unboosted point-light colour; keep the 1.5 on the FF lights"):
every object select delivers the params colour (`godot/src/lights/light_scene.cpp`
`kObjectLightD3DFill = false`) and the FF evaluation `obj_point_light_one`
(`shared.gdshaderinc`) multiplies by 1.5. Group culling: each light carries an owner pair
(entity ptr + section at record +76/+80); `Light_PassesActiveGroups
@ 0x5a9120` accepts lights owned by the sample position's interior
(`Lighting_SetInteriorLightGroup @ 0x5a90e0`, section-matched) or by the
entity being rendered (`Lighting_SetOwnerLightGroup @ 0x5a9100`);
`Light_SelectAndEnableForDraw @ 0x5ab9d0` (`Light_UpdateSlots @ 0x5abc50` is
its xref-less twin) D3D-enables at most **4 concurrent D3D lights** (indices
offset by `g_LightD3DIndexBase @ 0x840b20`), maintaining the active list
@ 0x2732dfc — but that enable set is NOT what a lit strip consumes (next
paragraph).

**The per-draw select is per ENTITY, the lit count is THREE (witnessed
2026-08-23).** Both sector walks build the query box from the drawn entity's
own bound — min/max = position ∓/± `boundRadius` (entity+0) per axis — and
hand it to the live select `[orig: Terrain_SetupEffectForEntity
@ 0x5c74fb..0x5c753a; Terrain_RenderSectorModels @ 0x5c5ea6..0x5c5ee4 →
@ 0x5c5f12 / @ 0x5c602e]`. `Light_SelectAndEnableForDraw` performs its own overlap scan and nearest
bubble sort into `g_LightVisibleHandles`/`g_LightVisibleCount` (≤ 63: the
pre-increment guard at `@0x5ABA7F..0x5ABA8D` excludes overlap 64), then LightEnables the
first ≤ 4 `[orig: the > 4 clamp @ 0x5abbeb; LightEnable(base + slot, 1)
@ 0x5abc1a]`. The lights a strip actually receives come from the BATCH
ENTRY: each collector re-walks the sorted visible list in order, gates
`Light_PassesActiveGroups @ 0x5a9120` and the objects-enable flag (render
flag 0x800 clear, `@ 0x5a9010`, called `@ 0x5d920c` / `@ 0x5d96cc`), and
stores AT MOST THREE handles in entry dwords 5..7 with the count in dword 8
`[orig: Render_CollectRenderObjectsForBatch @ 0x5d91e0..0x5d9229 — `cmp esi, 3;
jge` @ 0x5d9226..0x5d9229; the twin in Render_CollectRenderBatchesForEntity
@ 0x5d96a4..0x5d96e9]`. `CRenderBatchQueue_FlushBatches @ 0x5d9f50` walks
exactly those three slots (`X[2] = 3` @ 0x5da26b, splitting spot vs point),
pushes the point survivors as `PointLightCoordArray`/`PointLightColorArray`/
`PointLightAttenArray` with `CurNumPointLights` = the entry count for the
shader pass `[orig: Light_GetPointLightParams @ 0x5da6a8; the ID3DXEffect
count-setter vtable call @ 0x5da6ed; the three vector-array vtable calls
@ 0x5da71a/@ 0x5da740/@ 0x5da766 through the handles stored
@ 0x5af51b..0x5af566]`, and for the fixed-function pass first disables EVERY
enabled D3D light (`Light_DisableActiveD3DLights @ 0x5da5de`) and
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
`g_EnvTerrainColorRecip` bytes × 1/128 into the SEPARATE `flt_2732DAC/DA8/DA4`
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
  shooter's skinned person draws (the owner-group producers below). Once the five-tick fade kills the slot, the cached nonzero
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

`g_LightingOwnerGroupEntity`/`InteriorGroup*` are per-DRAW-CONTEXT state, so
owned lights illuminate only the draws that declare them. Both groups are now
supplied per draw:

- **Owner group** — `Lighting_SetOwnerLightGroup @ 0x5a9100` (pair
  `@ 0x272ED7C/78`). Producers (witnessed 2026-09-24): a nonzero owner
  entity is set ONLY by the person wave, the drawn entity at section 0
  before its head submit and again before its body submit
  (`Terrain_RenderSectorEntitiesBySide @ 0x5c7fb1 / @ 0x5c8004`); the RIGID
  collector re-scopes every ROBJ it collects to `(0, robjIndex)`
  (`Render_CollectRenderObjectsForBatch @ 0x5d8ff7`), which is what
  section-matches a building's own interior lights to the room being drawn,
  while the SKINNED collector keeps whatever is set (`Render_SubmitEntity`
  dispatches on the model's skinned flag `@ 0x5daddc..0x5dae2d`); the pair
  resets to (0, 0) at the wave tail (`@ 0x5c806e..0x5c808a`) and in
  `Terrain_RenderSectorBatch @ 0x609685`; the non-person wave and
  `Player_RenderFirstPersonViewModel` never set one. Consequence: an owned
  light (the MF_Light muzzle glow, a subobject lamp) reaches only its owner's
  skinned person draws, plus the interior case where the owner is the
  containing building; a subobject-attached record's light therefore lights
  none of its owner's rigid draws (corona only, unless the owner is a person).
  Port (2026-09-24, "Scope owned point lights by the owner group each submit
  declares"): `renderer::submit_owner_group`, `static_light_row_groups` (every
  static row `(0, robj)`), the per-model pass keyed on
  `ObjectModel::is_active_level_skinned` plus the person flag; first-person
  parts declare no owner.
- **Interior group** — `Lighting_SetInteriorLightGroup @ 0x5a90e0` (pair
  `@ 0x272ED84/80`). An entity draw pushes its containing building + blink
  section, read straight off the entity's packed blink ref (entity+464, the
  `blink_hits[0]` quad); retail additionally gates on the containing entry
  carrying a render model (`pool_entry + 0x30`, the entity's render-model
  pointer, the same field the iris march's short-circuit reads), and the
  port's "the containing building resolves to a presented model" check is
  that literal gate, not a stand-in (corrected 2026-09-24)
  `[orig: Terrain_SetupEffectForEntity @ 0x5c74a0]`. The same pair feeds the
  render-slot dominant-light pick `[orig: RenderSlot_UpdateEntityLight
  @ 0x5d6b40..0x5d6b77]`. A building draw pushes itself + section 0 `@ 0x5c5e07`. The
  terrain batch clears BOTH pairs `@ 0x60967c/@ 0x609685`, so only unowned
  lights reach terrain.

The gate reading them is `Light_PassesActiveGroups @ 0x5a9120`: an unowned
light always passes; an owned one passes when its owner is the interior group
entity AND its section matches the interior section (falling back to the OWNER
section when the interior section is 0 — the building-draw case), or when its
owner is the owner-group entity outright. `LightScene::select` is that
function; `LightDrawContext::groups` carries the pair per draw, the interior
half read off the model's stamp (`ObjectModel::set_interior_light_group`, fed
by `inmatch::EntityLightingFeed` for placed and wire rows alike since
2026-09-24) and the local player's through `local_player_interior_group` (the
first-person parts inherit the local player's group, so a room's lights reach
the arms and weapon).

Both retained ObjectModels and batched statics preserve retail's draw
granularity. A BUILDING expands to one context per visible ROBJ; its interior
group names the building at section zero and its owner section is re-scoped
to the ROBJ, so grouped lights reach only their authored section
`[orig: Terrain_RenderSectorModels @0x5c5e07; Render_CollectRenderObjectsForBatch
@0x5d8ff7]`. The query box of every such context is retail's ENTITY box —
position ∓/± `boundRadius` per axis, collected once per entity
`[orig: Terrain_RenderSectorModels @0x5c5ea6..0x5c5ee4; Terrain_SetupEffectForEntity
@0x5c74a0, query stores/call @0x5c74fb..0x5c753a]` — the per-ROBJ step only
re-gates that ordered list. `renderer::entity_light_draw_context` is the
shared typed producer: live ObjectModels retain the exact initialized Q16
entity radius, and `StaticEffectSource` carries that radius alongside the
base entity transform. Every split row uses this source cube, regardless of
its ROBJ geometry box. MultiMesh rows retain immutable per-entity/per-ROBJ
atlas identities, diagnostic geometry bounds, owner section, and
containing-blink stamp. The same selection function fills live instance
uniforms and static RGBAF rows, and the D3D12 24-technique probe requires those
routes to rasterize byte-identically. Wire handle zero occupies a tagged owner
domain and cannot alias the unowned/world sentinel.

**Entity query cube correction (D-RLIT-11, minted and closed 2026-09-13; the
host-side query mismatch D-RLIT-4's 2026-08-23 closure recorded).** The
shared `world::entity_bound_radius_q16` initializer folds authored Q16
scale into the base model sphere, takes a signed max with the **unscaled
first husk only**, and adds `0x1000`. The CDTA pointer gates the stamp; a
present empty block still receives the padding, and `huskFinal` is never a
fallback operand `[orig: Entity_InitFromModel @0x40dc30, collision-block gate
@0x40de8f, base scale @0x40e052, first-husk compare @0x40e062..0x40e06f,
pad store @0x40e076]`. Collision and both placement forms now consume that
same initializer. The static light binding takes entity origins and Q16
radii rather than geometry AABB pairs; each row keeps its own group filter
and cached-handle reselect. A zero radius stays an origin-only query.

Fresh retail disassembly and four Unicorn probes execute every instruction
of `Terrain_SetupEffectForEntity @0x5c74a0` with mixed-sign positions,
zero radius, and radii `84361` / `124494`. Captured arguments at
`Light_SelectAndEnableForDraw @0x5ab9d0` match position ± radius on all axes.
Only unrelated sun-visibility/interior-group service boundaries and the
selector sink are stubbed; the target function bytes remain unchanged in
retail PE SHA-256 `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`.
This is bounded query-construction evidence, not a full scene raster oracle.
Native `renderer_light_scene` / `world_model_geometry` regressions cover
the cube, zero-radius query, scale/husk/padding, four-world-light ordering,
and per-section admission before the three-light cap. GUT
`per_model_light_isolation_test` moves two real building parts apart and
checks live/static delivery through their production bindings;
`mission_object_placer_test` pins the real crate fixture's scaled/unscaled
metadata and matching live selection boundary. `effect_light_world_test`
retains its cached-vs-rebuilt atlas comparison over generated colors/fades.

**Adversarial caller follow-up (2026-09-13).** Object draws use
`Light_SelectAndEnableForDraw @0x5AB9D0`, whose overlap count starts at zero,
increments before testing `<64`, and excludes candidate 64 and all later
slots [orig: @0x5AB9EC, @0x5ABA7F..0x5ABA8D]. The sorted full candidate list
is published at `@0x5ABBC0`; the later D3D enable cap of four does not truncate
it. `Render_CollectRenderObjectsForBatch @0x5D91F0..0x5D9234` re-gates this list
per material, then stops after three passing lights. The shared draw selector
now uses **63 object candidates** while preserving the independent **64**
limit of `Light_CollectNearbyZonesByAABB @0x5AA250` for general/terrain queries.
A native regression places the closest light in slot 64 and checks both
indexed and linear object selection against the unchanged general query.

The query source also belongs to the entity when a draw uses a different
render model. `Player_RenderFirstPersonViewModel @0x4DEEA9..0x4DEEB0` queries
`g_LocalPlayerEntity` before its gun/arms submits; neither a camera offset
nor a gun model's sphere replaces that source. The native Simulation query
now supplies the local entity origin/radius through the existing typed
`LightScene` draw seam. Head and individual-husk child models receive the
root entity's query and groups; top-level held weapons declare their entity
owner separately from their posed transform [orig: Terrain_RenderSectorEntitiesBySide
@0x5c7f9a..0x5c8020, the one `Terrain_SetupEffectForEntity` call the head
and body submits share]. Static-to-live husk grafts carry
the original source radius and stable static identity, and restore the same
atlas row without respawning lights. `first_person_entity_light_test` builds
real gun/arms models through the normal world and presenter, then moves the
camera and changes model radii without changing the native query.
`per_model_light_isolation_test` checks displaced head/held parts, owner
motion and late nested replacement; `destruction_present_pass_test` checks
actual static carve, live husk selection and restoration.

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
toward the camera, the `texlightcrn` procedural radial, the 100-wu cull
(`@ 0x5ab09d..0x5ab0f6`, the compare `@ 0x5ab0f1`), flag 512 disable, the
owner visible-section gate). The whole corona is also skipped when the
(jittered, re-centred) light centre's view depth is not beyond the viewport
near value: the fixed view-matrix row-0 dot (`shrd eax, edx, 16h`) plus the row
translation `dword_A78428`, then `cmp eax, dword_A783D8; jle`
(`@ 0x5ab0fc..0x5ab143`); that near is 0x800 = 1/32 wu, written by
`Viewport_BuildProjectionMatrix @ 0x411093` (viewport+0x78). Port 2026-09-24
("Drop a corona whose light centre is inside the viewport near depth").
`EffectWorld_ResetPoolAndInitLighting @ 0x5ab890` is
the mission-start reset (zero pool, identity ambient, `Lighting_InitTextures
@ 0x5a94f0`).

**Terrain/foliage lighting constants.** `Render_TerrainScene @ 0x610c80`
(per frame) calls `Terrain_InitLightingColorRamps @ 0x604ee0` with
**(g_EnvLightBlock[0], g_EnvSkyBlock[0])** — NVG rebuilds the sky arg as bytes,
`trunc(sky_byte·0.25f' + modulator_byte·f'·0.0015625·255)` per channel under
a chop control word (`or eax, 0C00h; fldcw; fistp` `@ 0x610d16..0x610e22`;
`flt_7C3340` = 0.2, `flt_7C333C` = 0.25, `flt_7D00A8` = 0.0015625,
`flt_7CA29C` = 255; the alpha byte stays the sky block's), then
`Terrain_InitLightingColorRamps(g_EnvLightBlock, nvgSky)` `@ 0x610e36`
(port `EnvironmentState::nvg_terrain_sky`, 2026-09-24), the vehicle scope forces
`(0x101010, 0xF0F0F0)`. The ramps function stores **c1 = light ÷255
(`PolyTrn_PSConstC1_Light @ 0x31a182c`)** and **c0 = sky ÷255
(`PolyTrn_PSConstC0_Sky @ 0x31a183c`)**, seven VS ramp blocks of
{1,1,1,1} and {light·0.707 + sky} (the `g_EnvTerrainLightCombined` blend),
and the packed sun/blend ratio `g_PolyTrnSunToBlendRatioColor @ 0x849930`
(consumed by the tile renderers). `Terrain_SetupLightingAndShader
@ 0x604420` pushes c0/c1 as PS constants and picks the PS variant
(camera below `g_EnvWaterHeightFixed` swaps the stage-3 dp3 INPUT to the
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
`g_PolyTrnTileBakeDot3LightPass @ 0x60e385..0x60e39e` writes
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
c0/c1** — `Foliage_SetupDetailSlotDraw @ 0x6007c0` binds the PS without
touching the constants. The sector-model lightmap pass
(`Foliage_RenderDetailPatches @ 0x609de0` — renamed 2026-08-15, ex render_terrain_lightmaps) bubble-sorts visible sectors
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
currently named `Render_FoliageBillboards @ 0x607b30`; it is not a second
foliage-lightmap path. Detail foliage instead samples the composed terrain tile
RT selected by `Terrain_FindSectorPatchRT @ 0x6042a0`, exactly as the hosted
shared 128-layer terrain/foliage cache does. The CPU-side
1024×1024 premultiplied colormap (`g_PolyTrnColormapPixels @ 0x319f79c`) is
sampled by `Terrain_SampleColorMapTinted @ 0x606030`, but the fresh
2026-07-13 foliage trace follows the generated detail vertex to its final
write: that sampled packed diffuse is overwritten by the source-height bend
carrier before emission. It is therefore not a rendered instance-color
source. See the corrected D-FOLIAGE-1 disposition and
[foliage-re.md](../foliage/foliage-re.md).

**Underwater `PSShadow*` and static model shadows are different systems
(2026-07-29 correction).** `Terrain_SetupViewAndLighting @ 0x60FE40`
stores `cameraY < g_EnvWaterHeightFixed` in view field `+0x74`
(`@ 0x60FEE0`), and `Terrain_RenderVisibleSectors @ 0x6090C0` copies it to
`dword_319FB3C @ 0x60915F`. Under that flag,
`PolyTrn_BindStageTextures @ 0x604330` calls `sub_5C0190 @ 0x6043ED` and
binds its result, `g_WaterNoiseColorTexture @ 0x28EE8B8`, at t3; shader
selection occurs at `Terrain_SetupLightingAndShader @ 0x6044B1`.
Amended by the 2026-08-13 selector decode (the full section lives in
[terrain-re.md](../terrain/terrain-re.md)): the TOP tier keeps
`PS14SplatNormalMap` and merely swaps its stage-3 dp3 input to the water
noise; `g_PolyTrnPSShadowBasic` / `g_PolyTrnPSShadowNormalMap` (the
`4·t3²·t0.a` coefficient) serve only the partially authored ps.1.1 tiers.
Either way they are underwater animated wave-shadow/noise
variants, not geometry shadow-map receivers. `dword_319FB8C` is likewise not
shadow state: all four writers store one (`@ 0x6040DD`, `0x60910C`,
`0x60E89B`, `0x60EB25`), and `Terrain_RenderSectorBatch` reasserts it at
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
the ortho's near plane first (`RenderSlot_SetupShadowCascadeMatrices_0 @ 0x58D5F8..
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
catcher are retired. The ordered contributions, scorch updates, and final
cache cadence/edge/address/mip behavior closed with D-TERRAIN-7 (the last
facet, the page's CLAMP address mode, 2026-09-26); the generic
process-global CTRL/RNG lifetime is tracked by D-3DI-2.

**Lighting textures + the DOT3 light shader.** `Lighting_InitTextures
@ 0x5a94f0` builds the procedural set: `texlight2d`/`texlightspot2d` (64²
falloff via `Texture_GenerateProceduralFalloffTexture @ 0x5a92c0` modes 2/1),
`texlightspot1d` (64×8 cone-angle ramps), `texdepthgradw` (alpha ramp
col<<26) and `texdepthgradt` (soft depth window) — the DepthGrad pair the
AMODE_DEPTHTEST beam passes consume — `texlightcrn` (128² radial, black
borders), a 32³ volume falloff, mode-0x600/0x602 two-texture shaders, and —
device-gated ps.1.1 + ≥4 stages — the LAST embedded shader outside FrameFX:
`dp3_sat(t0_bx2, t1_bx2) × t2 × t3 × c0` (t0 the light-direction cube ·
t1 the detail coefficient map × two attenuation textures × light color; the
terrain pool-light pass, see the terrain projected pass below), built
additive-opaque and additive-blend. This is the per-pixel dynamic-light path — the
fixed-function era's per-pixel OmniLight.

**Cubemaps.** `Render_InitTextures @ 0x58f6e0` creates CubeEnvironment
(64/128/256 by quality `dword_8409E4`), CubeRotSpecular (always 128) and
CubeNormalize (64..256), then `Render_FillStaticCubemaps @ 0x58f290`
(cube-caps gated via `g_RenderDeviceCapsFlags @ 0x27213d4` bit 0x40, set by
the caps query `sub_676850`, disable mask `Render_ApplyCubemapCapsDisableMask
@ 0x5899e0`) fills CubeNormalize (`GTexture_GenerateNormalMapCubeMap @ 0x685570`,
packed 128+127.5·n normals) and **CubeRotSpecular** via
`GTexture_GenerateCubeMapLighting @ 0x685bb0` — per-texel `Σ pow(max(0, N·L), exp)
× intensity × color` over an inline 2-light list: **white {255,255,255}
power 800 intensity 1.4 + warm {255,248,240} power 40** along −Z (light
records: dir float3, B/G/R bytes @ +12..14, intensity @ +16, power @ +20).
`MatRotSpecular` (built per batch in `Material_ApplyShaderParameters @ 0x58e14b`
from `Math_BuildDirectionLookAtMatrix(Environment_GetLightDirectionFloat())`)
rotates lookups so the glint tracks the live sun — the Glass.fx GLOW
technique's source, closing the former D-RORD-5 specular-cube question.
**CubeEnvironment** is live but it is not a whole-world scene capture.
`EnvCube_Update @ 0x6106a0` renders all six faces initially/on
force and every 128 render frames. Highest quality selects 256²
(`Render_InitTextures @ 0x58f6e0`; lower settings select 64/128). The eye is
the local-player origin +1 Y, raised again to at least terrain+10. Each face
uses a 90° square LH view, near 0.5/far 1000
(`GTexture_RenderCubeMapFace @ 0x6864d0`). Face directions/up are
`+X/+Y, -X/+Y, +Y/-Z, -Y/+Z, +Z/+Y, -Z/+Y` in retail render-float axes.

The callback at `0x5c3700` draws only the sky dome and sun/moon, with lights
disabled; it does not submit terrain, world objects, water, stars, glare, or
glint. It then multiplies the completed gamma framebuffer by vertex color
`0xFF606060` under `SRC=DESTCOLOR, DST=ZERO`, and additively draws the
sun-aligned CubeRotSpecular sphere through `Render_SkyMesh @ 0x5ac680` and
the `g_EnvLightBlock` MODULATE2X descriptor. OpenNova mirrors that topology.
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
pow-40 lobe times the live light block. Since 2026-09-24 ("Align the
environment-cube sun lobe in Godot axes and light it raw") the analytic lobe
aligns with the light block's Godot-world −normalize(light dir)
(`opennova_light_block_dir`), the same vector the Q3 glass copy uses; the raw
render-float `opennova_sun_direction` tuple (x/z swapped against Godot) is
never compared with a Godot-world reflection (retail rotates the lobe through
`Math_BuildDirectionLookAtMatrix(Environment_GetLightDirectionFloat)`,
`Material_ApplyShaderParameters @ 0x58e146..0x58e1cb`). The lobe colour is the RAW
`g_EnvLightBlock` (`opennova_env_light_block`, `EnvShaderGlobals.light_block`):
the face callback (IDB `EnvCube_RenderFaceCallback`, which is the cube
face callback) pushes `g_EnvLightBlock` `@ 0x5c3863` into `sub_5AC840` /
`Render_SkyMesh @ 0x5ac680`, so the first-person thermal terrain ramp
(0x101010) never reaches it. The render-float→Godot X/Z swap, the
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
(`CGfxShader_CreateFromChannelDesc(28, 29, ..., 0x1520000)` — slot indices, the word's low 16
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
avatar body/head parts alike — as `ObjectModel::set_bound_radii_q16(model
sphere, entity bound)`: the MODEL SPHERE (gpm[5], the header's origin sphere)
sizes the capture extent and the depth clip (`RenderSlot_RenderEntityAndChildren
@ 0x5d7835` reads the model's +0x14, never entity+0), the ENTITY BOUND is that
authored-scale-adjusted sphere raised to the unscaled first husk stage's and
padded + 0x1000 only when the
graphic carries a collision block (`Entity_InitFromModel @ 0x40dc30`'s
gpm[44] gate `@ 0x40de8f`; a collision-less model keeps entity+0 = 0, so its
slot takes the lod floor 6) and sizes the lod/patch and the light query. The
dominant-light pick runs for every bound slot, as
`RenderSlot_UpdateEntityLight` does from the entity update. The D-COL-3 def
scale/collision-gate/first-husk fold now feeds the entity-bound slot stamp.
Residual: a model the placer did not build (no stamp) falls back to half its
render-bounds diagonal for both radii. (2026-09-24: the patch is now the
drape's own mesh, see *Drape* and *The port*; the projection itself is
unbounded, no near/far band or projector UV box is tested: the clamped
samplers cover the rest of the patch, the RT's white clear at its edge texels,
and shadowztex's clamped rows. The technique's stage-0 sampler clamps
(intrinsic 0x1000000) and shadowztex is created clamp `@ 0x5d62a9`.)

*Registration* — `Entity_InitFromModel @ 0x40E1C8..0x40E1F7`: persons
always, items via the `DynamicShadow` attrib2 bit, gated on the
shadow-detail option (`dword_24D2054`); the local player registers via
`PlayerClass_InitEntity @ 0x4b10f1`. `RenderSlot_AllocSlot @ 0x5d5690`
(ex `shadow_decal_alloc_slot`) finds or allocates the entity's 128-byte
record in the 256-slot `g_RenderSlotTable @ 0x2be3d30`. Both JO callers pass
shadow type 1 (dynamic silhouette); the type-0 blob-only alloc leg is
caller-less. LOD at alloc: `(boundRadius >> 15) + 1` (2·radius + 1 u)
clamped [6, 20]; the dead type-0 leg reads `max(shadow w, l) + 7` clamped
[2, 20] from the authored decal size.

*Frame open* — `Render_ShadowPass @ 0x5d7b70` loads the sun into the slot
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
dot, Q16 98304), halved for the local player OR the entity it rides
(`cmp ecx, [ebp+16Ch]` `@ 0x5d669c..0x5d66a6`, `>> 1` `@ 0x5d6864`);
excluded (score 0x40000000): entities with Flags & 1, the hidden / carried
bit written by `WacCmd_HideSsn @ 0x4f779d`, `Entity_AttachCarriedObject
@ 0x43c14a` and `Server_KillPlayerAndNotify @ 0x519e92` (`@ 0x5d657d`; NOT
the dead bit 2, corrected 2026-09-24; the render-occlusion claim, blink hits
and the outdoors three-ray latch, never removes a slot or its capture, and
`RenderSlot_RenderEntityAndChildren` has no visibility test), seat-parented entities
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
down to a 32 px floor (`g_RenderSlotTextureTable @ 0x2be3c94`).

*Per-slot light + anchor* — `RenderSlot_UpdateEntityLight @ 0x5d6a30`
(ex `Entity_UpdateRenderState`): default = the clamped sun trio, then the
brightest passing point light near the entity wins when NTSC luminance
0.3R + 0.6G + 0.1B over `dist²·quadratic + constant` exceeds the threshold
0.1 (`flt_7C69F4`); the winning direction is `normalize(entity − light)`.
The candidate list (witnessed 2026-09-24) is the collector over the ENTITY
origin ± entity+0 radius (`@ 0x5d6af1..0x5d6b24`) with output limit 4
(`push 4` `@ 0x5d6b00`; `Light_CollectNearbyZonesByAABB @ 0x5aa250` returns
min(found, limit) `@ 0x5aa418..0x5aa425` after its 64-cap scan and nearest
sort), the interior group from entity+0x1D0 (`@ 0x5d6b40..0x5d6b77`), each
candidate gated only by `Light_PassesActiveGroups` (`@ 0x5d6b89`) and
`Light_GetPointLightParams` validity (`@ 0x5d6bad`, the `jnz` `@ 0x5d6bb7`):
NO objects-disable gate and NO three-cap. A contained entity zeroes the
threshold (`@ 0x5d6adc..0x5d6aed`, also slot+0x78 = 1). Port:
`LightScene::slot_light_candidates` + `SlotShadow` at the entity origin with
the model's stamped interior group ("Give every contained drawn entity the
interior lighting lerp and group"). The shadow anchor then marches from the
march start along the light direction in unit-planar steps down to
`Terrain_GetHeightAtPosition @ 0x606720`. The march start
(`@ 0x5d6ce7..0x5d6d31`: `cmp dword [edi+24h], 0; jz`) is the rotated
collision-bbox centre when the entity Flags dword is zero
(`Math_BuildFixedPointMatrixFromEulerAngles(entity+4)` then
`Math_FixedPointTransformPoint22(matrix, entity+0x1FC)`, the result
`@ 0x5d6d25..0x5d6d2d`; entity+0x1FC is the collision-bbox centre written by
`Entity_InitFromModel @ 0x40df0a / @ 0x40df2e / @ 0x40dfd6` and
`Entity_ComputeBoundingSphere @ 0x5c6a8c`, the CMDL midpoint, scaled), else
the entity position; with a raised centre the march walks the patch down the
light, so a tall caster's long tail stays inside its patch. Vehicles never take
the centre start: their Flags carry the REFLECTABLE bit 0x400 that
`Entity_InitFromModel @ 0x40e208..0x40e20a` sets on every ItemDefType 1, so in
JO only Flags-clear props and persons start at the rotated centre (the 03TR
retail frame confirms it: the M939 tail ends at the origin-anchored lod-20
patch edge). Port (2026-09-24, "Port the slot march start, the patch lift and
the attached-light drape", "Read the vehicle REFLECTABLE bit in the slot
march's Flags test", "Drape each slot over its own patch mesh and carry the
march start on the present rows"): `renderer::slot_march_start_offset` +
`slot_entity_flags_zero` (the sim keeps the vehicle 0x400 as an item-type
trait, not in `Entity::engine_flags`), carried for every role on the present
rows (`world::PF_SLOT_MARCH_OFFSET_X/Y/Z`); a joiner row composes its retail
client entity's Flags dword (`inmatch::replica_entity_flags_dword`: the
load-stream dword with the compact low byte, the runtime latch bits, the Player
class bit 0x100) and reads its type's resolved collision shape for
entity+0x1FC. The vertical step
keeps the direction's own rate and is SUBSTITUTED by 0.5 u of drop (fixed
−32768) only when it would not descend (`test eax, eax; jl` `@ 0x5d6cd7..
0x5d6cdd` — corrected 2026-08-22; the earlier "clamped to ≥ 0.5 u" reading
steepened every shallow sun). The anchor PLACES the drape patch; the
projected UV matrices land the silhouette.
The slot LOD refreshes grazing-scaled: `(0.5 + |0.5/dirY|)·baseLod`
clamped [6, 20] (`@ 0x5d6d5c..0x5d6dac`).

*The light update's CALL SITE* (witnessed 2026-09-16) — retail runs
`RenderSlot_UpdateEntityLight` from the 62.5 Hz entity walk, not from a render
leg: `Entity_UpdateAllEntities @ 0x4c25e0..0x4c2602`, immediately after the
eligible pool-0 update callback, does
`if (enabled > 0 && (uint16_t)entity[+0x1B6] != 0)
 RenderSlot_UpdateEntityLight(entity[+0x1B6], entity)` — `g_RenderSlotShadowQualityCfg @ 0x24D2054`
is the shadow-quality cfg word and `entity+0x1B6` the entity's slot handle (the
adjacent `entity+0x1B8` feeds the scar-decal position restamp `sub_57FFD0
@ 0x4c2617` in the same tail). **Divergence, accepted:** OpenNova drives the
same pick from the Godot shadow device leg once per DISPLAY frame
(`godot/src/env/slot_shadow.cpp`, the per-assignment `pick_dominant_light`
block), not once per entity update. The pick is a pure function of the entity
origin, its bound radius, its interior group, the clamped sun default and the
nearby-zone light pool, so the only observable difference is the sampling
cadence: above 62.5 fps
the direction is re-picked more often than retail would, below it less, and a
light that appears and disappears inside one display frame can be missed. The
`[orig:` citation lives here rather than at the port site because `godot/src`
carries no witness tags (ADR 0042 d7 / ADR 0043); the pick block's comment in
`slot_shadow.cpp` names the same call site in prose.

*Silhouette render* — `RenderSlot_RenderEntityAndChildren @ 0x5d7690`
renders the entity plus its standing/mounted children into the slot RT
(ortho extent = radius·1.25 clamped radius + 0.75,
`RenderSlot_SetupShadowCascadeMatrices @ 0x58d300`: a rotation-only D3D view from
the slot direction through `Math_BuildDirectionLookAtMatrix @ 0x612c90` —
forward = normalize(dir), right = normalize(fwd.z, 0, −fwd.x), up = fwd ×
right, a vertical direction leaving right/up ZERO — with the entity rendered
at that view's origin (`Entity_RenderWithLODCallback @ 0x5d6ef0` zeroes the
position for a null origin, children at their offset `@ 0x5d795a/0x5d79c8`)
under an orthographic projection of scale 1/extent over the depth band
0.2..5000.2 (`@ 0x58d38b..0x58d3a3`: 1/(far − near) = 0.0002, −near/(far −
near) = −0.00004); the look-at is the one engine `renderer::direction_look_at`
(`direction_look_at.h`, which the addeweap attachment frame in
`world/mounted_pose.cpp` consumes too) with the `kSilhouetteCaptureNear/Far`
constants; as read, that band starts 0.2 u in front of the view origin the
entity is rendered at, and the render state that admits the entity's near
half is unwitnessed — D-RLIT-10 records the shell's eye). The capture view
is the look-at matrix itself (`RenderSlot_SetupShadowCascadeMatrices @ 0x58d31e` ->
SetTransform VIEW `@ 0x58d368`): camera x = right, y = up, depth = forward, a
proper rotation in render axes, and the PROJSHAD pass culls CCW over it
(`CRenderBatchQueue_FlushBatches @ 0x5da3e4..0x5da401`), keeping the
light-facing faces. Presentation axes are the render axes with x and z
swapped (a reflection), so the capture pose maps the retail right row to
presentation as the negated look-at right of the presentation direction
(`renderer::slot_capture_view_axes`: x = −right, y = up, z = −forward,
right-handed); an unmapped look-at basis is mirrored (det −1) and its
back-face cull keeps the light-averted faces (fixed 2026-09-24, "Map the slot
capture view into presentation axes and key it on the entity origin"; the
former `silhouette_capture_basis` is gone). The capture view origin, the drape
projection and the light query centre on the entity origin (the capture
renders the entity at the view origin `Entity_RenderWithLODCallback
@ 0x5d6fc4..0x5d6fd9`, the light query box is entity+4 ± entity+0
`@ 0x5d6af1..0x5d6b39`, the drape texgen uses slot+0x54..0x5C = the entity
position `@ 0x5d6a5f..0x5d6a9d`), never the render-bounds centre, which moves
with part animation and RLOD switches. The refresh runs on the detail-scaled
cadence: `(frame & mask) == (slotIndex & mask)` with mask 7 below detail 2,
3 at 2, 1 at 3, every frame at 4+; the local player OR the entity it rides
refreshes every frame from detail 3 (`@ 0x5d7707..0x5d7734`); the dirty bit
forces. The capture sets no lighting beyond
`RenderBatchCtx_StoreLightingConstants(0)`: the PROJSHAD black pass needs
none. `RenderSlot_SetupSectorReceiverPass_Dead @ 0x5d7250` (ex
`setup_entity_render_lighting`; c21..c23 = (c + lum) × 0.5 × −3, the
darkening math this record formerly assigned to the capture) is the dead
sector-model receiver path: `RenderSlot_CollectReceiverSlots_Stub @ 0x5d7240` only zeroes
`g_RenderSlotPendingCount` and returns 0, and its callers gate the receiver
path on that result (`@ 0x5c5f04..0x5c5f0c`, `@ 0x5c7c8d..0x5c7c8f`)
(corrected 2026-09-24, "Delete the dead slot receiver darkening law"). The PROJSHAD effect output is
black, not the normally lit material: `_vsPost.fx::vsPostBlackT1` writes
`Diff = (0,0,0,1)` and `_vsSkPost.fx::vsSkinPostBlackT1` writes `Diff = 0`;
every PROJSHAD pass selects that diffuse for color (`TSSColor SelectArg2
Diffuse`; `_FFP.fx TBoringFFPProjShad` zeroes the material), so RGB is black
over the RT's white clear, with the effect-family diffuse/detail/AlphaGen
policy supplying coverage.

*Drape* — `RenderSlot_DrawAllDrapes @ 0x5d6e20` (detail > 0; an entity with
Flags & 1 skips, the same hidden / carried bit the assignment tests
`@ 0x5d6e6a`; the LOCAL player in first person skips while prone-latched
(`g_PlayerStanceProneLatch @ 0xb76484`) or below detail 2): a dynamic slot
with a live RT draws `RenderSlot_DrawSilhouetteDrape @ 0x5d5ca0` — the
silhouette projected over the slot's own terrain-following patch mesh
(`RenderSlot_RebuildPatchVertexBuffer @ 0x5d5130`: (lod + 1)² vertices,
vertex (i, j) = index i·(lod+1)+j at mission (origin_x + i, origin_north − j)
`@ 0x5d5246..0x5d52f7`, height = `Terrain_GetHeightAtPosition @ 0x606720`
(POINT sample, `u16 << 8`) plus the lift (lod + 1) × 0.004 u (`flt_7DB864`;
the resolution slot+0x28 = lod + 1 is stored `@ 0x5d6da9..0x5d6dac`;
`@ 0x5d5201..0x5d529f`), normal (0, 1, 0) `@ 0x5d52b1..0x5d52c3`, FVF 0x212
stride 40, rebuilt when origin/lod/resolution change `@ 0x5d51a3..0x5d51bc`;
indices from `RenderSlot_InitPatchIndexBuffers @ 0x5d53d0`: per level L,
2L² triangles, per cell (a, d, c), (a, b, d) with a = (i, j), b = (i, j+1),
c = (i+1, j), d = (i+1, j+1) `@ 0x5d5474..0x5d54d0`; lod is 6..20, so the
441-vertex region is the lod-20 maximum). The distance fade is taken ONCE per
slot from the 3D entity-to-camera distance (slot+0x18..0x20 against the
camera fixed position `@ 0x5d5cc4..0x5d5d14`): a slot at ≥ 0x500000 (80 u)
returns without drawing (`@ 0x5d5d3b..0x5d5d40`; its capture still
refreshes), else `f = (d − 0x280000) × 1/2621440` (`@ 0x5d5d46..0x5d5d53`) is
folded into the material ambient every patch vertex shares
(`@ 0x5d5f63..0x5d6008`). A SECOND texture
stage clips the projection by depth — `shadowztex`
(`Shadow_SystemInitResources @ 0x5d62d2`: a 32×4 white/black step, one
gray texel at the boundary, row 3 all white) addressed by
`RenderSlot_BuildShadowCascadeUVMatrices @ 0x58cf10`'s detail matrix (u = depth
along the clip direction × 0.5/half_size + 0.5, v = 0.333 × (0.5/half_size)²
× depth along the light direction + 0.5 — the v row multiplies the already
k-scaled primary depth column by 0.333 k, `@ 0x58d222..0x58d249`; corrected
2026-08-22) — where person-type entities (itemdef +0x5C == 3)
steepen the CLIP direction's vertical component 4× (`flt_7C44B8`
@ 0x5d5d85: the copy at slot+108 feeding lookat_dir2 only; the silhouette
projection keeps the unscaled lookat_dir1 — re-witnessed 2026-08-21, the
earlier "elongated 4× along the projection direction" reading was wrong),
and the **sun ambient law** per channel:
`ambient_c = 1 − (1−f)·q_c`, `q_c = L_c·|y| / (L_c·|y| + S_c)` with
`L = g_EnvLightBlock`, `S = g_EnvSkyBlock` and `y = fabs(slot+0x6C)`, the
stored RAW clamped-negated vertical (`@ 0x5d5f63`; −0.25 at a dawn sun under
the clamp), not a normalized copy; the shadow removes only the
direct sun term scaled by the projection vertical, never the sky ambient.
The drape IS fogged: `RenderSlot_DrawAllDrapes` selects
`CD3DDevice_SetFogAndBlendMode` mode 3 before it (`@ 0x5d6ea1`): mode & ~3 = 0
is the primary fog block (`@ 0x67782c..0x6778b2`) and mode & 3 = 3 the fog
colour WHITE (`@ 0x6778f9`), so the multiply fades to a no-op with distance;
the technique's intrinsic pass flags 0x1520000
(`Shadow_SystemInitResources @ 0x5d62f7`, stored by
`CGfxShader_SetRenderStateDesc` at +0x3C) carry 0x20000 = FOGENABLE
(`CGfxShader_ApplyPass @ 0x683241..0x68324f`), 0x100000 z-write off,
0x400000 cull none and 0x1000000 stage-0 clamp. An attached-light slot
lights its patch through D3D light 4 instead: D3DRS_AMBIENT = 0xFFFFFF
(`SetRenderState(0x8B)` `@ 0x5d5e50..0x5d5e63`), light 4 filled from the
attached handle slot+0x74 through `Light_FillD3DPointLight @ 0x5aa450`
(type point; diffuse = rgb × intensity × AmbientScale × 1.5 (`flt_7D4B80`)
× RgbGen; position the YNegated float; range = radius_fixed × 1.25/65536
(`flt_7D9F90`); attenuation {1, 0, 15/range²} (`flt_7D9F8C` = 15); the fill
fails for an empty or hidden slot), diffuse_c rescaled to
(c + lum) × 0.5 × (1 − f) × −2 (lum = 0.3r + 0.6g + 0.1b,
`@ 0x5d5e89..0x5d5f0d`), `SetLight(4)` + `LightEnable(4, TRUE)`
(`@ 0x5d5f14..0x5d5f23`), then `SetMaterial` with diffuse = ambient =
(1, 1, 1, 0) (`@ 0x5d5f25..0x5d5f4c`); each patch vertex takes
sat(1 + diffuse × atten × N.L) on its (0, 1, 0) normal, zero past the range.
A failed fill still sets the white material: that drape darkens nothing.
A bound slot WITHOUT a live RT calls
`RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0` (fog mode 0), which draws
nothing in JO: its second gate reads ItemDef+0x114 (`cmp dword ptr [eax+114h],
0; jz` `@ 0x5d59f4..0x5d59fb`, eax = entity+0x20, the ItemDef
`RenderSlot_AllocSlot` reads the `shadow` w/l from `@ 0x5d572a..0x5d5754`),
and JO never assigns +0x114 a non-null value: a full store scan finds only two
writers, both zeroing it (`Entity_InitAllFromModels @ 0x40e486` over every
def, and the orphan loop at `0x40d1a6`); `ItemDef_ParseProperty
@ 0x49f3a5..0x49f44c` stores only the TGA name (+0xA0) and the w/l/ox/oy
floats (+0x11C..+0x128) and never loads a texture, and
`EntityDef_LoadModelsAndCallbacks @ 0x439f50` loads only `huskshadow` (+0xB0,
a 3DI model) into +0x118 (`@ 0x43a369..0x43a38a`). The authored `shadow` line
is parsed and unused (corrected 2026-09-24, "Remove the authored blob drape,
dead in JO"; the earlier reading had it drape a heading-rotated decal). As a
note on the dead function only, its UV matrix decodes to
u = (a cos t + b sin t)/w + 0.5 + ox, v = (−a sin t + b cos t)/l + 0.5 + oy
over (render x − slot+0x54, render z − slot+0x5C), t = heading + 2^31 BAM.
Entity ground shadows therefore land on TERRAIN ONLY — the patches are
terrain-following meshes.

*The port* — planning and color laws are portable in
`engine/runtime/renderer/render_slot_shadow.{h,cpp}` (direction clamp,
alloc/grazing LOD, RT chain, cadence, scoring/24-12 assignment with sticky
captures, dominant-light pick, march start and anchor march, the patch
mesh and lift, the capture view axes and depth band, the per-slot fade + the
sun ambient law, the attached-light drape colour and the silhouette-combine
laws; ctest
`renderer_render_slot_shadow` pins each), and the per-technique PROJSHAD
coverage source in `object_shader_template` (`object_projected_shadow_coverage`
beside the pass-state table; ctest `renderer_material_classify`). The device
half (`godot/src/env/slot_shadow.cpp` + `godot/src/render/slot_capture_adapter.cpp`
+ `slot_shadow_drape.gdshader`; `engine/formats/def` parses the `shadow`
line, which nothing consumes, as in retail; GUT `slot_shadow_test`) realizes the capture
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
(packed once per surface and per mesh: a caster's surface slot swaps a
different ArrayMesh onto the same node per authored RLOD level, so each
level's arrays are their own retained entry and a crossing selects the
level's packed stream, `slot_shadow_test` "capture follows the caster's
authored RLOD switch"; skinned strips pack their bone indices/weights
and skin on the GPU from the frame's bone palette). Device folds, each
serving the same observable: the capture eye backs off along −forward in
`slot_shadow.cpp` (D-RLIT-10 below) where retail renders the entity at the
origin of its rotation-only view, and the zenith degeneracy takes the
world x axis; the pass rasterizes each capture 4x multisampled and resolves
it into the single-sample target the drape reads, where retail's chain is
single-sampled end to end (`RenderSlot_InitTextureChain @ 0x5d5320` ->
`GRenderTarget_CreateSurfaces @ 0x67f7b0`: a plain `D3DUSAGE_RENDERTARGET`
texture and a `D3DMULTISAMPLE_NONE` depth surface) and its drape samples the
RT bilinearly — the resolve keeps a partial-coverage edge so a hard-aliased
1-2 px silhouette line does not scintillate against the breathing
first-person camera; held
weapons ride their owner's slot via the capture-with link
(`ObjectModel.set_slot_shadow_capture_with` — the
`RenderSlot_RenderEntityAndChildren` child walk) while tree-parented
riders fold into the ancestor exclusion; and `scene_output` leaves the factor in
the retail gamma-byte domain for the shared FrameFx display decode. Since
2026-09-24 ("Drape each slot over its own patch mesh and carry the march start
on the present rows", "Fade, cull and fog the slot drape per slot like
retail", "Port the slot march start, the patch lift and the attached-light
drape") `SlotShadow` draws each drape over its own RenderingServer patch mesh
(`renderer::slot_patch_vertices` / `slot_patch_indices`, rebuilt on retail's
origin/lod/resolution gate, the heights from `TerrainData::get_height_world`
plus `renderer::slot_patch_lift`), every instance sharing one drape material at
`renderer::kRungSlotDrape` (-12) with its slot in the instance uniform `u_slot`; both
texgens and the attached-light colour (`renderer::drape_attached_light_color`,
sat(1 + diffuse × atten × N.L) with the 1.5× D3D boost and the range cutoff)
run per patch vertex and Gouraud-interpolate as in retail; the fade and the
80 u cull are one per slot (`drape_fade` / `drape_culled`) and the drape fogs
toward white with the terrain's primary fog config. The former terrain-surface
stand-in for the patch, its per-pixel evaluation and the per-slot attenuation
fold at the entity are retired. The drape reads a stencil mark the terrain
and the sky dome write in their colour pass, where only the front-most
surface passes depth (the opaque colour pass compares EQUAL against the
prepass), and draws at `kRungSlotDrape`, after the sky group and the
viewmodel and before every world rung, so every model overwrites it as
retail's later draws do (`Terrain_RenderMainSectorPass` calls
`Terrain_RenderSectorBatchLit` `@ 0x610c34`, then `RenderSlot_DrawAllDrapes`
`@ 0x610c47`; drape Z-write off, flags 0x1520000 `@ 0x5d6362`, pass 0x100000
`@ 0x5d5e30`); an overhanging drape edge multiplies the dome, or the clear
colour while the sky bracket is closed (the dome then draws at alpha 0,
stencil only). The indoors letter 0x2 skips `Terrain_RenderMainSectorPass`
(`@ 0x5ca84d..0x5ca84f`) and every drape with it while the slot captures run
earlier (`Render_ShadowPass`, called `@ 0x610cb5` from `Render_TerrainScene`); ported:
`OcclusionFrame::apply_scene_pass_gates` closes
`SlotShadow::set_terrain_pass_drawn` with the terrain gate (the witness sits
at `kRungSlotDrape` in `render_order.h`; 2026-09-26). The other callers of
`RenderSlot_DrawAllDrapes` are `NVG_RenderSceneToTarget @ 0x5d0833`,
`Render_HUDOverlay @ 0x5d82ac`, `Terrain_RenderSceneWithLOD @ 0x610155`,
`Terrain_RenderSceneScaled @ 0x6102af` and
`Terrain_SetupViewAndRender_ScaledWithLOD @ 0x61040b`. The
object wrappers carry no PROJSHAD branch (the twelve-layer capture-camera
signature, its `OBJ_PROJSHAD_*` defines and the reserved `Water` layers
were retired 2026-08-30 with the shader hash golden regenerated); the
render-swatch `projshadow` probe now proves the RenderingDevice pass on the
synthetic fixtures (opaque, alpha-tested single and _MT, alpha-blend, and
the additive-LUM / glass no-pass surfaces) beside the beauty frame. The
packaged runtime serves shadow detail 3, retail's highest SHADOWQUALITY
(`options.mnu` rows 0..3; `Settings_ClampGraphicsOptions` clamps to 3
`@ 0x54d546`; the 0x34-byte settings block copy `@ 0x551500` lands it in
`g_RenderSlotDetailLevel` unchanged via `Terrain_Init @ 0x60fcc5` ->
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

**Low-sun drape defect (revalidated 2026-08-24, 03TR dawn, PR #560;
not reproduced 2026-09-24, see below).** At the 03TR spawn the M939 drape remains visibly different, and
strafing exposes temporal flicker in the same projection. The first attempted
fix changed the PROJSHAD edge representation and drape combine to a
black-on-white additive form; an in-game before/after check showed no visible
improvement, so that explanation is rejected as the cause. The live retail
slot observation (`type=1, rt_live=1, order=6/2`) still establishes that this
is the silhouette-drape leg, but the capture/update/projection path must now be
measured under deterministic lateral motion before another correction is
claimed.

*2026-09-24 re-measurement.* A strafe capture at the 03TR fixture pose (13
lateral steps of 0.5 u, terrain "Detail levels" draw mode) found the ground
under every 03TR drape within the 80 u drape range to be one flat terrain LOD
family, with the shadow locked to the truck across the strafe and no shift or
flicker. The retail frame's M939 tail ends at the origin-anchored lod-20 patch
edge: a vehicle's Flags carry the REFLECTABLE bit 0x400, so it marches from its
position, the extent the ported march start, patch mesh and per-slot fade
reproduce (the *Per-slot light + anchor* and *Drape* paragraphs above). No
open symptom is recorded after the pass.

## The per-light terrain projected pass (witnessed + ported 2026-08-21)

The pool's GROUND leg: each terrain batch is re-drawn once more PER LIGHT
through a two-stage projected-texture technique, then composited with the
page and the fogged ordinary pass (the composite below; corrected 2026-09-24,
the earlier reading added every light on top of the batch). Witnessed in
full this round (read-only decompiles; the facts are cited in
`engine/runtime/renderer/light_terrain_pass.h`):

- **Per-batch collect** `[orig: Terrain_RenderSectorBatch @ 0x6092a0,
  @ 0x609641..0x609953]`: the batch's own AABB (sector origin + the record's
  float corners through `Math_FloatToFixedPoint3_YNegated`) collects the pool
  with `Light_CollectNearbyZonesByAABB(min, max, &count, 16)` `@ 0x60966f` — a
  cap of SIXTEEN, not the object pass's 64 — then CLEARS both light groups
  (`Lighting_SetInteriorLightGroup(0,0) @ 0x60967c`,
  `Lighting_SetOwnerLightGroup(0,0) @ 0x609685`), so only UNOWNED lights ever
  reach the terrain; a counting pass `@ 0x609690..0x6096c9` keeps the leg only
  when a handle passes `Light_PassesActiveGroups @ 0x60969f` AND
  `LightInstance_IsAliveAndLightsTerrain @ 0x6096b3` (alive, flags without
  0x400); the per-light loop `@ 0x60984c..0x609953` re-draws the batch once per
  passing handle with NO four-light cap. The loop pushes `use_alt_texture = 1`
  `@ 0x609850/@ 0x60989a`, so the normal pass applies `dword_2732DC8` (state
  0x600); the adapter caps `g_TerrainAdapterCapsStorage+0x34` bit 0x100
  `@ 0x609890` selects `Light_SetupTerrainProjectedPassPS @ 0x6098a5` (0.4/r)
  over the fixed-function `@ 0x6098b4`; the port serves the PS pass
  (2026-09-26): the `ps.1.1` program at 0x7d9fa0,
  `sat(dp3(t0_bx2, t1_bx2)) x t2 x t3 x c0`, t0 = the cube-normalize map
  (`GTexture_GenerateNormalMapCubeMap @ 0x685570`, 256 at the top
  object-detail setting) along (light - point) in D3D (z, x, y) order, t1 =
  texture slot 8 = the detail coefficient map
  `Texture_GenerateNormalMap(polytrn_detailmap, B, 1/32)` (`@ 0x60b164`, bound
  by `PolyTrn_BindStageTextures @ 0x6043ad`) at the second UV set, t2/t3 =
  `texlight2d` (slot 23 on BOTH stages; `texlightspot1d` sits in the unread
  slot 22) at the disc and height coordinates, c0 = rgb x blend x
  AmbientScale x recip factor (x RgbGen), no 0.66/0.5 fold. Skipped when
  `g_PolyTrnUsePixelShaderPath == 0 @ 0x6095e4` or `dword_319FB84 != 0 @ 0x60983f`.
- **The pass setup** `[orig: Light_SetupTerrainProjectedPass @ 0x5aa830]`: the
  argument is a `g_LightInstanceTable` slot `@ 0x5aa844`; `inv = 32768 /
  radiusQ16` (0.5/r) `@ 0x5aa864..0x5aa873`; two texture matrices from the
  runtime camera→world matrix `flt_27219C0` (written by
  `Render_SetViewAndProjectionMatrices @ 0x58d947` /
  `Render_SetViewAndProjectionFromMatrices @ 0x58d752`): stage 0
  (`g_LightTexLight2D`, 64²) `u = (world·col0 + (F0 − pos.x))·inv + 0.5`,
  `v = (world·col2 + (F8 − pos.z))·inv + 0.5`; stage 1, the height stage,
  `u = (world·col1 + (F4 − pos.y))·inv + 0.5`, `v = 0.5`, samples
  `texlight2d` too (the pair is created with slots 23 and 23,
  `sub_679250(23, 23, 0x600) @ 0x5a98e0`; `texlightspot1d`, slot 22, is read
  by no terrain light pass) — the texgen is
  camera-space, so engine-side it reduces to the light-relative world offset
  × inv + 0.5. Pixel constants: `c0 = (0, 0, 0, 1)`, `c1 = 0.5 · (rgb · blend ·
  EffectWorld_AmbientScale · flt_2732DA{C,8,4} · 0.66)` `@ 0x5aa9c8..0x5aaab3`
  (× `RgbGen_EvaluateColor` when the record carries a gen block `@ 0x5aaa05..
  0x5aaa5f`), alpha 1. The 0.5 exists because stage 0 runs MODULATE2X in the
  applied shader (`GfxShader_ApplyPassChecked @ 0x5aaafe/@ 0x5aab16`): 0.66 ×
  0.5 × 2 = 0.66; stage 1's 2× is left uncompensated.
- **The per-channel factor** is `g_EnvTerrainColorRecip` unpacked × 1/128 into
  `flt_2732DAC/DA8/DA4` by `EffectWorld_TickInstancesAndLightScale @ 0x5aa1ef..
  0x5aa23f` — the environment default `0x808080` `[orig: Environment_InitDefaults
  @ 0x57c050..0x57c065]` divides to exactly (1, 1, 1). It is NOT the ambient
  scale (see the gain unpacks above).
- **The textures** `[orig: Texture_GenerateProceduralFalloffTexture @ 0x5a92c0;
  Lighting_InitTextures @ 0x5a95a6..0x5a9612]`: mode 2 = `trunc(255·exp(−4x²)·
  exp(−4y²))` opaque grey (`g_LightTexLight2D`), mode 1 = `trunc(255·(1 − (x² +
  y²)))` clamped ≥ 0 with alpha = i, `x = (2·col − 65)/64` (c0 = 2.0, flt_7C44B8
  = 4.0, dbl_7D9F98 = 255), border 0; the 64×8 `g_LightTexSpot1D` strip is built
  inline: `(col·(row+1)) >> 1` grey, end columns white.
- **The composite** (witnessed and ported 2026-09-24, "Composite terrain pool
  lights with the page the way retail's lit batch does") `[orig:
  Terrain_RenderSectorBatch @ 0x60984c..0x609ab6]`: a batch with pool
  lights never draws its ordinary pass alone. It draws each light unfogged,
  the first drawn light with blending off (`g_LightTerrainPassPSFirst` on the PS path /
  `dword_2732DC8` mode 0x600 on the fixed-function path) and later ones
  ONE/ONE (`g_LightTerrainPassPSAdd` / `dword_2732DC4` mode 0x602) (`Lighting_InitTextures
  @ 0x5a9a56..0x5a9b04`, `@ 0x5a98d2..0x5a9933`), so the target holds the
  saturated sum `pool`; then multiplies the target by the page with
  `dword_319F934` (`sub_6791A0(1, 0x1000628) @ 0x60c499..0x60c4ad`:
  MODULATE2X(page, the lit white diffuse), DESTCOLOR/SRCCOLOR, unfogged;
  drawn `@ 0x609960..0x609a19`); then adds the fogged ordinary pass
  (`Terrain_SetupLightingAndShader(2)` -> `dword_319F930` mode 0x1020002;
  `@ 0x609a49..0x609ab6`). Pixel = sat(sat(2 × sat(2 × page) × pool) +
  fogged lit) (`renderer::terrain_light_pool_composite`).

**Port.** `LightScene::collect_terrain_pass_rows` (engine/runtime/renderer/
`light_terrain_pass.{h,cpp}`) runs the collect + gates per patch and publishes
≤ 16 `TerrainLightRow{position, inv_scale, pixel_rgb}` per patch; since
2026-09-26 the rows are the served `ps.1.1` pass's (0.4/r, c0 without the
0.66/0.5 folds, `terrain_light_ps_constant`, clamped to the ps_1_x register
range). `falloff_texture_light2d_argb` and the cube-normalize texel generator
`cube_normalize_texel_argb` (256 per face, `GTexture_GenerateNormalMapCubeMap
@ 0x685570`, sized by `Render_InitTextures @ 0x58f6e0`) are the generators,
and the detail coefficient map (`terrain::build_detail_coefficient_map`) is
the t1 texture; `env::terrain_color_recip_*` + `EnvironmentState::
terrain_color_recip_packed` serve the factor. The device (`godot/src/lights/
light_scene`, `terrain_lighting.gdshaderinc`'s `terrain_point_light_pool`,
`GameWorld::render_terrain_light_leg` in `godot/src/world/game_world_frame.cpp`)
uploads the generated disc and cube textures once, binds the coefficient map
as `u_terrain_light_normal`, and pushes per-patch light rows, summing
`sat(N.L) x disc x height x c0` per light (N.L between the cube lookup of
(light - point) and the coefficient texel, both `_bx2`) into the saturated
pool, which
`terrain.gdshader` composites as retail's lit batch does:
sat(sat(2 × sat(2 × page) × pool) + the fogged ordinary pass) for a patch with
pool lights, the ordinary fogged pass alone otherwise; the rows are
published in `render_light_frame` for the NEXT terrain frame (one-frame
latency at 62 Hz, stated as the device fold). Pinned by ctest
`renderer_light_terrain_pass` (selection, gates, the 0.5/r and 0.4/r scalars,
the recip default, the projection u/v, the 2× round trip) and the GUT terrain
shader contract / light isolation tests, and for the PS pass GUT
`terrain_light_ps_pass_test.gd` and `terrain_light_pass_device_test.gd`.

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
  `pixel = tex × sat(mix(HemiGround, HemiSky, N.y·0.5+0.5) +
  DirLightColor·max(0,N·L) + points) × 2`, summed and saturated PER VERTEX
  and Gouraud-interpolated for the fixed-function families (Fixed,
  FixedDetail, FixedSkinned, Flag); SELFLUM = `tex × sat(SelfLumColor ×
  ColorSrcGlobalGain) × 2`; FFP_GLASS = `cube × sat(ReflectColor ×
  ColorSrcGlobalGain) × 2` (corrected 2026-09-24, "Port the object combiner
  rules the render-parity review found diverging": the earlier text read
  `min(lit, 1)` per pixel and SELFLUM as `min(ColorSrcGlobalGain, 1)`); uniform surface
  `u_hemi_sky_color/u_hemi_ground_color/u_dir_light_dir/u_dir_light_color/
  u_color_src_global_gain` (D-RMAT-5 closed in
  [render-material-re.md](render-material-re.md)).
- `engine/runtime/renderer/render_slot_shadow`: the render-slot entity
  ground-shadow planner (2026-08-20) — direction clamp, alloc/grazing LOD,
  RT chain, refresh cadence, priority scoring + 24-patch/12-capture
  assignment with sticky orders, dominant-light pick, march start and anchor
  march, the patch mesh and lift, the per-slot drape fade + the per-channel
  sun ambient law + the attached-light drape colour, the capture view axes and
  depth band: ctest
  `renderer_render_slot_shadow`; the PROJSHAD coverage source per technique
  in `object_shader_template`. Device: `godot/src/env/slot_shadow.cpp`
  (the per-armed-slot capture requests, the per-slot patch meshes and the
  drape uniform push) +
  `godot/src/render/slot_capture_adapter.cpp` (the PRE_OPAQUE
  RenderingDevice capture pass over the shared Q3 geometry cache) +
  `godot/shaders/slot_shadow_drape.gdshader` (the drape material the patch
  meshes share); `engine/formats/def` parses the authored `shadow` decal line
  (unused, as in retail).
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
submit matrix `[orig: Environment_UpdateSunGlare @ 0x5ad1ba..0x5ad213]` independently
confirms the map). Every low-sun frame front-lit where retail backlights.
Fixed via `godot/src/util/axes.h` at each seam: the `MissionEnvironment`
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

Fixed 2026-09-13 (**D-RLIT-11**, minted-and-closed in one round, the entity-query
correction): the host-side query mismatch the 2026-08-23 D-RLIT-4 closure recorded
(each ROBJ part's own AABB and the model AABB, 64 admitted object candidates) is
replaced by the entity's position +- boundRadius cube and the 63-candidate object
limit; see the row below and the "Entity query cube correction" section above.

| ID | Ours | Original | Disposition |
|---|---|---|---|
| D-RLIT-1 | Hosted weather runs the complete 16-block chain: modulator2/modulator plus all 14 color blocks | 16 blocks modulate in witnessed order (including skyfog and the static ceiling/cloud/floor trio) `[orig: @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-21)** — `SkyWeatherColorBlocks` preserves the witnessed order; skyfog gets its lightning additive, horizon blend, and tail double; and `MissionEnvironment` writes every color current back. `SkyDome` and the frame clear share the final doubled skyfog, the flat dome consumes smoothed `cloud_rgb`, and indoor iris samples consume the pre-modulated ceiling/floor currents. |
| D-RLIT-2 | Iris exposure uses the marched 3-point camera-ray average with per-sample indoor/outdoor classification and sun occlusion | 3-point average marched back from the terrain/entity-clipped camera ray. `Environment_ApplyFogAndAmbient` passes `g_LocalPlayerEntity`; all samples reuse its `+0x1BC/+0x1C0` candidate slice for building blink tests and the nonzero-count outdoor ray gate; the three allow-all-types rays therefore test pool-1 dynamics and pool-2 statics. Indoor-with-data sets the interior light group, indoor-no-data retains it, and outdoor clears it `[orig: Environment_ApplyFogAndAmbient @ 0x57e440/0x57e51d; Environment_ComputeAmbientLightAlongDirection @ 0x5c7a00; Terrain_SectorComputeLighting @ 0x5c7550; Entity_RaycastCollision @ 0x413760; Physics_RaycastFindCollisionEntity @ 0x539a70]` | **FIXED (2026-08-22)** — `compute_iris_samples` now uses the shared nearest-collision clip, candidate-scoped blink query, exact local-player count gate, and candidate-scoped radiused walker; pool-1 source slices use the witnessed ItemDef gate instead of vehicle physics. The mutable iris light-group pair preserves the exact set/retain/clear sequence while typed per-draw groups own every actual light selection, avoiding a compatibility global. The original march/curve, indoor gain-255 short circuit, sun level 8−hits at −0x2000/−0x5000/−0x8000, and INT /3 average remain unchanged. Native `collision` regressions pin nearest clipping, indoor terrain bypass, candidate-only blink, dynamic blockers, and pool-1 eligibility. Measurement note (2026-08-20): frozen render fixtures previously published the modulator's mission-reset identity gain; capture now stamps the marched samples and settles the chase at the fixture pose (`Weather.settle_exposure`, capture-seam only). |
| D-RLIT-3 | `items.def light_transfer` drives interior ROBJ sections plus the contained player/viewmodel, and drawn outdoor pool-0/pool-1 entities dim DirLightColor by the witnessed 3-radius sun query | interior-parented entities lerp to floor/ceiling ambience by the aux daylight t: the containing building's ItemDef+0x218 `light_transfer` / 100 for a contained person, the stack base 0 for a contained non-person, a building's own transfer for its ROBJ 1+ (corrected 2026-09-24; formerly read as "model+536"). Eligible outdoor pool-0/pool-1 entities cast one 200-u sun segment at clip radii −0x2000/−0x5000/−0x8000 from position + collision-AABB midpoint, walking the entity's OWN `+0x1BC`/`+0x1C0` candidate slice (self excluded at slice build; only bubble-overlapping solids can block); each blocked cast steps DirLightColor 1.0→0.75→0.5→0.25; contained entities, empty-slice sources, and pool-2 statics stay 1.0 `[orig: Terrain_SetupEffectForEntity @ 0x5c74a0; Entity_ComputeSunVisibility @ 0x5c6800; Physics_RaycastFindCollisionEntity @ 0x539a70; Entity_BuildProximityListsFromPools @ 0x4b8eb0; the FP-pass discard @ 0x4deeb0]` | **FIXED (2026-08-23)** — local/authority and decoded-client draw paths now share the exact bound/center and three-radius method. Wire pool-0 and eligible pool-1 sources receive separately keyed 17-tick slices; explicit registry-twin identity is the only self-exclusion, so equal H/L packed values cannot alias. `get_draw_lighting_changes` (since 2026-09-24 `Simulation::draw_lighting_changes` over `inmatch::EntityLightingFeed`) replaces the bms-only API with `[wire,bms,quality]` triples, and the wire presenter applies/caches the factor for both bodies and late-built held weapons. Native collision proofs pin the 16-tick empty window, wire/local key separation, all-three-ray hit, and pool-1 EWeap exclusion; `wire_present_pass_test` pins body/weapon directional-light delivery. 2026-09-24 ("Give every contained drawn entity the interior lighting lerp and group"): the interior half follows the witnessed wave split for every contained drawn entity, placed and wire, through `inmatch::EntityLightingFeed` (see the per-entity reader). |
| D-RLIT-4 | The portable EffectWorld core, decay lifecycle, safe opaque handles, mission-start/powerup model-light spawn, four transient routes, target gates, both active groups, live/static per-draw selection, top-tier technique response, terrain projection, and coronas are hosted. Authored LGHT is spawn-fixed; static rows keep exact per-entity/per-ROBJ bounds and atlas identity; node teardown uses the one shared model/muzzle entity handle. | Retail transforms each LGHT point by the entity matrix once, uses `subobject` only for owner grouping, stores only the final handle at entity+0x1B4, reuses it for MF_Light, does not rescan husks, and clears it once at Entity_Destroy. Object draws query nearest 64 with the ENTITY's position ± boundRadius box and the batch entry keeps the first THREE group-passing, objects-enabled handles (the 4 in `Light_SelectAndEnableForDraw` is the D3D enable count FlushBatches tears down per entry) `[orig: Entity_SpawnGlowEffects @0x56c7c0; Entity_UpdateMuzzleGlowEffect @0x56c960; Entity_Destroy @0x43e903; Render_CollectRenderObjectsForBatch @0x5d8ff7/@0x5d9229; Light_CollectNearbyZonesByAABB @0x5aa250; Light_SelectAndEnableForDraw @0x5ab9d0; Terrain_SetupEffectForEntity @0x5c74fb; CRenderBatchQueue_FlushBatches @0x5da26b/@0x5da5de]`. Coronas use the owner-section visibility gate; Spot/Target delivery is dead code. Foliage also has no max-quality point-light consumer: its selector call at `0x60a5dc` is followed by a VS/PS pair whose only light fold is cached-tile c0/c1 and whose `oD0` is authored c6 `[orig: Terrain_CreateFoliageVertexShaders @0x5ff630; Foliage_SetupDetailSlotDraw @0x60087a]`. | **FIXED (2026-08-23; cap corrected to three the same day)** — live ObjectModels, static MultiMesh rows, blink/interior groups, per-ROBJ sections, spawn/respawn/destruction lifecycle, highest-quality VS/PS technique consumption, terrain projection, and coronas are closed. The per-ROBJ/model query box must be the entity's position ± boundRadius cube stamped on every split draw (the engine contract `renderer::LightDrawContext` documents; `light_scene.cpp` currently stamps each ROBJ part's own AABB and the model AABB — the one remaining host-side mismatch of this row). `effect_light_world_test`, `per_model_light_isolation_test`, native `renderer_light_scene`, the 24-technique D3D12 swatch, and the foliage shader contract pin the routes. Generation leases intentionally reject retail's stale-handle write-through memory alias. |
| D-RLIT-5 | The earlier implementation substituted hemisphere-along-reflection for CubeEnvironment and approximated Phong channels | NORMAL now samples a synchronously hosted 256² CubeEnvironment with the exact callback/cadence/origin/camera/dim/static-lobe chain; PhongMap is generated byte-for-byte and every ordinary/point lobe and Diffuse1-alpha role is source-pinned `[orig: EnvCube_Update @ 0x6106a0; callback @ 0x5c3700; Render_CreateSystemTextures @ 0x58aca0; _psPhong.fx; _psPhong2.fx]` | **FIXED (2026-08-22)** — `EnvironmentCubeCapture`, the generated PhongMap binder, the 24-technique swatches, and the Forward+ D3D12 cubemap axis/orientation/gamma-byte probe close the former source and hosting residuals; 2026-08-30: the faces are copied into the published RD cubemap on the RenderingDevice (`EnvironmentCubeBlit`), the former per-publish CPU readback stall gone and the byte parity kept exact. The same cutover's `FrameFx` closes the former D-RORD-5 GLOW post backend. |
| D-RLIT-6 | Shared terrain/foliage page cache hosts the configured tile-set strip, ordered `.til` RGBA, and DOT3 lighting alpha | `Terrain_LoadTileSetAtlas` loads `polytrn_tilestrip`/`g_BmsTileSetName` + `.TGA`, divides it into 64-pixel cells, and `PolyTrn_RenderTile` indexes those cells for `.til` overlay quads before the DOT3 pass; no independently loaded mission-lightmap TGA exists `[orig: Terrain_LoadEnvironmentConfig @ 0x610940; Terrain_LoadTileSetAtlas @ 0x604A90; tile bind/index/draw @ 0x60DDD4..0x60DF1B]` | **FIXED / false premise closed (2026-08-23)** — `TerrainTilePageSourceView::tilestrip` and `compose_terrain_tile_page` host that exact producer; terrain and detail foliage consume the same published cache layer. Static model silhouettes and the remaining RT mechanics were scoped to D-TERRAIN-7, closed 2026-09-26. |
| D-RLIT-7 | Static mission objects (the placer's MultiMesh batches) froze the env lighting harvested at load — the throwaway template's materials had no live owner, so TOD/weather/iris advances relit animated models but not the static world (the load-time snapshot even carried the pre-first-iris-tick modulator: gain 1.0 vs the settled 60/64) | retail relights EVERY entity from the current lighting block each frame `[orig: Render_SetupEntityLightingAndShaderConstants @ 0x5d98a0 ← CRenderBatchQueue_FlushBatches]` | **FIXED (2026-07-06, the model-parity slice; mechanism re-ported 2026-08-26)**: first closed by registering every harvested batch ShaderMaterial and re-stamping it from the live env per frame (generation-gated, single-sourced with the per-model stamp; verified batch uniforms == live-model uniforms after settle, dir 159/255, gain 60/64). The per-material restamp was the reimpl's own mechanism (retail keeps ONE block per pass and no per-entity lighting state `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 -> RenderBatchCtx_StoreLightingConstants @ 0x5d89e0; Environment_ApplyFogAndAmbient @ 0x57e440]`) and cost ~1 ms per frame on 03TR (WORLD_WEATHER + MODEL_ENVIRONMENT + the render-thread material re-uploads); the world block now lives in the `opennova_light_block_*` / `opennova_fog_*` global shader parameters `MissionEnvironment` writes once per env change, the object shaders read it at draw time exactly like the batch context, and static batches, live models, and previews all follow the live env with no owner and no stamp. Object fog now takes the pass fog end (`fog_end_distance`, the overcast-scaled value `Render_SetFogState` receives) instead of the un-scaled `fog_level` the per-material copy carried |
| D-RLIT-8 | The object per-material hemisphere mixed color spaces: `hemi_sky` came from `MissionEnvironment.get_sky_ambient()` = the RAW TOD keyframe (never smoothed, never iris-modulated) while `dir_color`/`hemi_ground` came from the smoothed+modulated weather writeback — off-noon the modulator brightens every block toward the exposure target but the un-modulated sky half stays dark (the sky-facing half of every building too dark at night; the terrain/foliage GLOBALS path was already correct via `get_smooth_sky()`) | retail feeds ALL entity lighting from the post-modulator block render colors — the world-block writer fills [8..10] ← `g_EnvSkyBlock[0]` ÷255 exactly like light/ground `[orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090; the blocks smooth + modulate in the weather tick @ 0x57ef97..0x57f03c]` | **FIXED (2026-07-06, the REN-6 session)**: the sky block joins the per-tick env writeback seam — `Weather` pushes `get_smooth_sky()` through the new `MissionEnvironment.set_sky_ambient_rt` (mirroring fill/sun/fog, generation-gated), `get_sky_ambient()` serves the smoothed current and re-seeds from the keyframe on discrete TOD recomputes (the `_fill_light` contract); GUT pins the seam (`env_parity_vectors_test.test_sky_ambient_serves_smoothed_writeback`); golden env grid/weather rows byte-identical (the grid collects bare env nodes; the weather checkpoints already read `get_smooth_sky`) |
| D-RLIT-10 | The render-slot silhouette capture places its RenderingDevice eye two model-sphere diameters plus 2 u behind the caster center along −forward with a 0.05..(2·eye + r) depth band (`godot/src/env/slot_shadow.cpp`, beside the pose) | retail renders the entity at the origin of the rotation-only look-at view under the 0.2..5000.2 ortho band `[orig: RenderSlot_SetupShadowCascadeMatrices @ 0x58d300 (proj[10] = 0.0002, proj[14] = −0.00004 @ 0x58d38b..0x58d3a3); Entity_RenderWithLODCallback @ 0x5d6ef0 zeroes the position]`; read literally, the band's near plane sits 0.2 u in front of the entity's own origin and would clip its near half, and the render state that admits it (a disabled D3D clip, an unwitnessed z state) has not been found | PERMANENT (class C, 2026-08-30): an orthographic silhouette is invariant under a translation along the view axis, so the shell's eye reproduces the witnessed image exactly while keeping the whole model sphere inside the RD clip band; reproducing the literal band would manufacture the unwitnessed clip. Reopen only with the retail render state that resolves the band |
| D-RLIT-11 | Object draws queried the light pool with each ROBJ part's own AABB and the model AABB, admitted 64 candidates, and first-person, head and husk parts derived their query from the posed replacement mesh (the host-side mismatch D-RLIT-4's 2026-08-23 closure recorded) | Every render part of an entity queries with the ENTITY's position +- initialized boundRadius cube stamped once before its split submits; the object query admits the first 63 overlapping slots while the general/terrain query keeps 64 `[orig: Terrain_SetupEffectForEntity @0x5c74a0 (query stores @0x5c74fb..0x5c753a); Terrain_RenderSectorEntitiesBySide @0x5c7f9a..0x5c8020; Player_RenderFirstPersonViewModel @0x4deea9..0x4deeb0; Light_SelectAndEnableForDraw @0x5aba7f..0x5aba8d; Light_CollectNearbyZonesByAABB @0x5aa250; Entity_InitFromModel @0x40de8f..0x40e076; Render_SectorEntity @0x5c41a9]` | **FIXED (2026-09-13; minted and closed in the same PR)**: `renderer::entity_light_draw_context` is the one typed producer (`EntityLightQuery` = entity origin + Q16 radius), `world::entity_bound_radius_q16` initializes that radius (authored scale before the unscaled first-husk max, the CDTA gate, the +0x1000 pad), `kObjectQueryLimit` = 63 and `kQueryLimit` = 64, and live ObjectModels, static atlas rows, first-person parts, heads, held weapons and husks all pass the same cube. Native `renderer_light_scene` / `world_model_geometry` and GUT `per_model_light_isolation_test` / `first_person_entity_light_test` / `mission_object_placer_test` pin it; four Unicorn probes execute the query stores. The 2026-08-23 D-RLIT-4 closure is unchanged. |
| D-RLIT-9 | Godot device seams consumed the render-float (D3D-world) celestial/light tuple as if it were Godot world — the bases differ by the x/z swap `godot = (z, y, x)_render`, so the sun/moon/glare/glint bodies, the object directional term, the glare jitter plane, the sun-veil dot, the dome uniforms and the star placement all sat 90° off in yaw and mirrored; every low-sun frame front-lit where retail backlights | mission `(x, y, z)` -> render `(-y, z, x)` `[orig: Math_FixedPointToFloat3_YNegated @ 0x611210]`; the water-glint submit matrix independently confirms the map `[orig: Environment_UpdateSunGlare @ 0x5ad1ba..0x5ad213]` | **FIXED (2026-08-20, the 03tr-sun-sky fixture slice; id minted 2026-08-21)**: one mapping seam (`godot/src/util/axes.h`) applied at every consumer listed in the axis section above; the raw tuple stays on the `opennova_sun_direction` global and the terrain `u_sun_direction` uniform, which re-swizzle into the engine texture basis themselves and are byte-parity-verified there; measured by the registered `03tr-sun-sky` fixture |

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
| 0x5d7250 | setup_entity_render_lighting | RenderSlot_SetupSectorReceiverPass_Dead | pops the pending slot list; the sector-model receiver lighting (dead in JO behind the always-zero `RenderSlot_CollectReceiverSlots_Stub @ 0x5d7240`, 2026-09-24) |
| 0x5d6a30 | Entity_UpdateRenderState | RenderSlot_UpdateEntityLight | the shadow-slot dominant-light pick + terrain anchor march |
| 0x604ce0 | sub_604CE0 | Terrain_LoadScorchTextures | trscrch1-3.tga + qburn01.tga (the brief's "4 lightmap materials" guess was wrong — scorch decals) |
| 0x606030 | sample_terrain_lightmap | Terrain_SampleColorMapTinted | samples the CPU colormap (not the tile-set atlas or composed tile RT) × tint >>7; foliage instance colors |
| 0x58f290 | sub_58F290 | Render_FillStaticCubemaps | fills CubeNormalize + CubeRotSpecular at init (caps-gated) |
| 0x5899e0 | sub_5899E0 | Render_ApplyCubemapCapsDisableMask | clears g_RenderDeviceCapsFlags bits from the disable mask |
| 0x58b220 | generate_sky_cubemap | EnvCube_RenderFaceOrAnalyticFill | callback → live scene face render; no callback → the dead analytic 5-light fill |
| 0x2732e28 | unk_2732E28 | g_LightInstanceTable | 176-B light records, handle-indexed |
| 0x272ed84/80 | dword_* | g_LightingInteriorGroupEntity/Section | the sample position's interior pair |
| 0x5a9fd0 | (undefined code) | LightPool_SpawnSpotProjectorEffect | defined 2026-08-20: the spot/projector spawner (flag 0x10000, projection matrix at instance bytes 88..152) — CALLER-LESS dead code in Jointops.exe |
| 0x5aa830 | render_foliage_instance | Light_SetupTerrainProjectedPass | 2026-08-21: the per-light terrain projected pass setup (slot index arg `@ 0x5aa844`, 32768/range scale, 0.66 × 0.5 colour, g_LightTexSpot1D/Light2D, pass dword_2732DC4/DC8); nothing foliage about it |
| 0x5aa170 | (comment) | EffectWorld_TickInstancesAndLightScale | 2026-08-21 (the wire-up round): the entry comment reworded — the tail unpacks `g_EnvTerrainColorRecip` × 1/128 into the terrain pass's own `flt_2732DA{C,8,4}` triple; it does NOT derive `EffectWorld_AmbientScale` (that is the modulator × 1/64 `@ 0x5aaf1d..0x5aaf37`) |
| 0x5a92c0 / 0x5a94f0 / 0x58d947 / 0x6092a0 | (comments) | Texture_GenerateProceduralFalloffTexture / Lighting_InitTextures / Render_SetViewAndProjectionMatrices / Terrain_RenderSectorBatch | 2026-08-21 (the wire-up round): the texel formulas, the inline 64×8 strip, the `flt_27219C0` camera→world writer, and the per-light loop's 16-cap / group-clear / alt-texture facts recorded as entry comments (no renames) |
| 0x5aaf40 | CEffect_RenderFoliageBillboards | EffectWorld_RenderLightCoronas | it walks g_LightInstanceTable drawing additive corona billboards, not foliage |
| 0x5ab9d0 | collect_visible_foliage_slots | Light_SelectAndEnableForDraw | the LIVE per-draw light select: fills g_LightVisibleHandles/Count from the drawn ENTITY's position ± boundRadius box and D3D-enables the first <= 4 (Light_UpdateSlots @ 0x5abc50 is the xref-less standalone variant); the batch entry keeps only the first THREE gate-passers (@ 0x5d9229 / @ 0x5d96e9) and FlushBatches tears the enable set down per entry (@ 0x5da5de) |
| 0x5a9280 | sub_5A9280 | Light_SetDisabledForPass | cubemap-face renders set 1 (no dynamic lights/coronas), Render_TerrainScene resets 0 |
| 0x2732db8 | dword_2732DB8 | g_LightCoronaShader | the 1-texture additive corona shader over g_LightTexCorner ("texlightcrn") |
| 0x2732de8 | dword_2732DE8 | g_EffectWorldLightsDisabledForPass | the pass gate above |
| 0x2732de4 | dword_2732DE4 | g_LightInstanceHighWater | the pool high-water count |
| 0x272ed7c/78 | dword_* | g_LightingOwnerGroupEntity/Section | the rendered entity's pair |
| 0x272ed98 / 0x272eda0 | dword_272ED98 / effectHandle | g_LightVisibleCount / g_LightVisibleHandles | the per-frame in-frustum light list |
| 0x840b20 | dword_840B20 | g_LightD3DIndexBase | D3D light index base for dynamic lights |
| 0x2732dfc / 0x2732e00 | dword_* | g_LightActiveD3DList / g_LightActiveD3DCount | the ≤4 enabled-light shortlist |
| 0x8437e0..e8 | dword_8437E0.. | g_DefaultLightDirX/Y/Z | {0, 1, 0} — D3D light 0 direction fallback |
| 0x2be3d30 | unk_2BE3D30 | g_RenderSlotTable | 128-B shadow-slot records |
| 0x5d5690 | shadow_decal_alloc_slot | RenderSlot_AllocSlot | find-or-alloc + the LOD-at-alloc laws (2026-08-20) |
| 0x5d6530 | terrain_sort_and_assign_render_slots | RenderSlot_SortAndAssign | scoring, exclusions, 24-patch/12-RT binding (2026-08-20) |
| 0x4c25e0 | Entity_UpdateAllEntities | (unchanged) | the per-entity call site of RenderSlot_UpdateEntityLight: `g_RenderSlotShadowQualityCfg @0x24D2054 > 0` + the slot handle at `entity+0x1B6` (2026-09-16); OpenNova runs the same pick per display frame instead — see the render-slot section |
| 0x24d2054 | enabled | RenderSlot_ShadowQualityCfg | the cfg word gating the per-entity light update and the slot family (2026-09-16) |
| 0x5d5320 | init_render_target_chain | RenderSlot_InitTextureChain | the 12-RT halving size chain (2026-08-20) |
| 0x5d5130 | terrain_tile_rebuild_vertex_buffer | RenderSlot_RebuildPatchVertexBuffer | the (lod + 1)² vertex terrain drape patch build in a 441-vertex region (2026-08-20) |
| 0x5d6e20 | sub_5D6E20 | RenderSlot_DrawAllDrapes | the per-slot drape walk + local-FP/prone gates (2026-08-20) |
| 0x5d5ca0 | render_sector_model | RenderSlot_DrawSilhouetteDrape | the projected silhouette drape: fade, the depth-clip stage (person 4x steepens its direction), the sun ambient law (2026-08-20; clip stage 2026-08-21) |
| 0x5d59d0 | render_minimap_tile_overlay | RenderSlot_DrawAuthoredBlobDecal | the items.def `shadow` decal drape (2026-08-20); dead in JO, its ItemDef+0x114 gate is never assigned (2026-09-24) |
| 0x2be3c0c | dword_2BE3C0C | g_RenderSlotDetailLevel | the shadow-detail level driving chain size + cadence (2026-08-20) |
| 0x2be3bb4 | dword_2BE3BB4 | g_RenderSlotCount | live slot-record count (2026-08-20) |
| 0x2be3a90/94/98 | dword_* / frameState | g_RenderSlotPendingCursor/Count/PendingList | the slot iteration state |
| 0x2be3c94 / 0x2be3a48 | dword_* | g_RenderSlotTextureTable / g_RenderSlotShader | slot render targets + shader |
| 0x2bebd68..70 | outDir / dword_* | g_RenderSlotDefaultLightDirX/Y/Z | the sun default for slot lighting |
| 0x31a182c / 0x31a183c | flt_* | g_PolyTrnPSConstC1LightColorR / g_PolyTrnPSConstC0SkyColorR | the terrain PS constants (c1 = light, c0 = sky) |
| 0x849930 | dword_849930 | g_PolyTrnSunToBlendRatioColor | packed sun/combined ratio for the tile renderers |
| 0x319f7d0/d4, 0x319f7b0..bc | dword_* | g_TerrainTileSetTexture/TexView, g_TerrainTileSetAtlasWidth/Height/TilesX/TilesY | the terrain tile-set overlay atlas (`g_BmsTileSetName` + `.TGA`; renamed 2026-08-20 from the `Terrain_Lightmap*` misnomers — see tiles/til-re.md, tile-set atlas source) |
| 0x319f79c | dword_319F79C | g_PolyTrnColormapPixels | the CPU 1024×1024 premultiplied colormap |
| 0x2721364..78 | dword_* / srcHeight | g_RenderClip1DTexture/PhongMapTexture/AngleMapTexture/CubeNormalizeTexture/CubeRotSpecularTexture/CubeEnvironmentTexture | the shared texture set (slots 196-206); `srcHeight` was a kong misnomer on the ENV CUBE |
| 0x2732db0..e0 | dword_* | g_LightDot3PixelShader, g_LightTex{Light2D,Spot2D,Spot1D,DepthGradW,DepthGradT,Corner} | the Lighting_InitTextures resource set |
| 0x27213a0 / 0x27213d4 | unk_/dword_ | g_RenderDeviceCapsBlock / g_RenderDeviceCapsFlags | the caps block the cube fills gate on |
| 0xb7654c / 0xb76550 | dword_* | g_NVGActive / g_NVGBrightnessLevel | the NVG state (written by Player_ToggleWeaponScope / input bindings) |

## 2026-09-24 rendering parity pass

What the pass changed in this record (commit subjects of the integration
branch; the witnesses are in the sections named):

- **NVG scope.** The NVG world treatment is gated on `g_CameraMode == 0`
  ("Gate the NVG world treatment on camera mode 0"); the hemisphere rewrite
  touches only the object block and the terrain sky, which NVG rebuilds as
  bytes; the colour getters stay raw ("Keep the env colour blocks raw under
  NVG; rebuild the terrain NVG sky as bytes"); the dome unpack's NVG dim is
  ported ("Dim the sky dome under NVG, whiten it under thermal, exact light
  blend"). Witnesses: the world lighting block and the terrain/foliage
  constants sections.
- **Per-flush state.** ctx+841 is the first-entry latch (slot 226 and the
  shared constants are pushed on every flush's first entry), and the FF
  hemisphere lights are per-flush state (the per-entity reader).
- **Object point lights.** The 1.5× boost rides the D3D fixed-function lights
  only ("Deliver the unboosted point-light colour; keep the 1.5 on the FF
  lights"); the owner light group follows the per-submit producers ("Scope
  owned point lights by the owner group each submit declares"); coronas drop
  when the light centre is inside the viewport near depth ("Drop a corona
  whose light centre is inside the viewport near depth").
- **Interior lighting.** The aux daylight t is ItemDef+0x218 `light_transfer`;
  the wave split, every building's ROBJ 1+ and contained statics are ported
  through `inmatch::EntityLightingFeed` ("Give every contained drawn entity the
  interior lighting lerp and group"; the per-entity reader, D-RLIT-3).
- **Environment cube.** The analytic sun lobe aligns in Godot axes and takes
  the raw light block ("Align the environment-cube sun lobe in Godot axes and
  light it raw").
- **Render slots.** The per-slot fade, 80 u cull and white fog; the Flags & 1
  gate and the ridden vehicle's local priority; the capture axes and the
  entity-origin key; the march start (with the vehicle REFLECTABLE bit); the
  patch mesh and lift; the attached-light drape; the dead blob and receiver
  legs removed; the dominant-light candidate query ("Fade, cull and fog the
  slot drape per slot like retail", "Keep occluded casters' slots and give
  the ridden vehicle the local priority", "Map the slot capture view into
  presentation axes and key it on the entity origin", "Port the slot march
  start, the patch lift and the attached-light drape", "Read the vehicle
  REFLECTABLE bit in the slot march's Flags test", "Drape each slot over its
  own patch mesh and carry the march start on the present rows", "Remove the
  authored blob drape, dead in JO", "Delete the dead slot receiver darkening
  law"); the drape's draw order at `kRungSlotDrape` and its indoor gate followed
  2026-09-26 (*The port*). `SunShadow` lost its dead static-terrain projection mode ("Drop
  SunShadow's dead static-terrain projection mode"): it carries the dynamic
  receiver/caster masks and the slot direction law and allocates no shadow
  map (the SlotShadow capture pipeline renders the entity ground shadows, the
  tile composer the static ones); the render diagnostics key
  `shadows.sun.projection_mode` is gone.
- **Terrain pool lights.** The lit-batch composite ("Composite terrain pool
  lights with the page the way retail's lit batch does"); the served `ps.1.1`
  per-light pass followed 2026-09-26 (the terrain projected pass above).
- **Object combiner.** Per-vertex saturation and the saturated SELFLUM /
  glass emissive ("Port the object combiner rules the render-parity review
  found diverging"; the ported chain).

## Open questions

The three questions open before the pass are answered in the witness map:
`dword_A890C8` is `g_CameraMode` (the NVG gate), the aux daylight float is
the containing building's ItemDef+0x218 `light_transfer` written only by
`ItemDef_ParseProperty @ 0x4a1a2c..0x4a1a50`, and `Color_UnpackToFloat4
@ 0x578900`'s NVG branch is ported.

### Open after the 2026-09-24 pass

- None.
