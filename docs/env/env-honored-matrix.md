# Environment honored-matrix: which `.env` fields the renderer actually consumes

The runtime parses nearly every `env::Config` field (water height still arrives via
terrain/BMS), but not every field reaches a visible render input yet. This matrix
classifies each parsed field so runtime work can distinguish a live control from a
parsed-but-deferred one, and so renderer changes flip rows only with citations. It is the audit behind the
environment fidelity work; the reverse-engineering record it
leans on is [env-tod-re.md](env-tod-re.md).

Classifications:

- **HONORED** — reaches a render input through the witnessed original behavior.
- **PARTIAL** — reaches the renderer, but through approximated math, an incomplete
  trigger path, or only in some modes. Each PARTIAL row names what is missing.
- **UNCONSUMED** — parsed and round-tripped, but no render effect today.
  Rows marked *faithful* are unconsumed in retail too; the rest await a ported
  consumer and must **not** be faked (an untracked divergence would mis-train
  authors).

Chains below run source `.env` → `EnvFile` (godot/src/env/env_file.h) →
runtime nodes (`MissionEnvironment` / `SkyDome` / `Water` / `Weather` /
`Celestial`, godot/src/env/) → shader. The portable math lives in
`engine/formats/env` (`env.h`, `env_weather.h`, `env_celestial.h`, `env_water_render.h`).

## Scalars, fog, water

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `env_name` | source → `EnvFile.env_name` | UNCONSUMED (*faithful*) | OpenNova extension; retail has no keyword | Metadata/round-trip only. |
| `timeofday` (enum) | `EnvFile` property | UNCONSUMED (*faithful*) | classification string only | Never read by the gradient in retail either. |
| `curtime` | `EnvFile` → `MissionEnvironment.time_of_day` bootstrap | HONORED | [orig: Environment_SetCurrentTime @ 0x57c4b0] | Initial TOD at load. |
| `envscale` | parse-time scale on every `*_rgb` getter | HONORED | [orig: g_EnvParseEnvScale @ 0x840950] | Raw storage + getter-side scaling; cross-file parse-order bleed deliberately not replicated (see env-tod-re.md). |
| `fog_level` | `MissionEnvironment.get_fog_*` → terrain/water shader uniforms | HONORED | [orig: Render_SetFogState @ 0x58a950; Environment_GetFogEndDistance @ 0x57e3e0] | Via `env_render::compute_fog_params`. |
| `fog_type` | same | HONORED | [orig: Render_SetFogState @ 0x58a950] | All four policies (0=exp, 1/2/3 linear variants) in `env_render.cpp`. 2026-09-24: the start the device receives is already overcast-folded (`Render_SetFogState` multiplies the type-2/3 start by (1 - density) @ 0x58a9c5..0x58a9fe) and every consumer uses it as-is; the fog distance follows the retail pass path (FOGMODE_SHADER passes linear in view depth for every type, fixed-function linear fog radial through RANGEFOGENABLE, type 0 device EXP on view depth). See env-tod-re.md §Fog policy. |
| `terrain_tint` (`terrain_rgb`) | `EnvFile.get_terrain_tint` → `EnvironmentState::tile_overlay_tint()` → the `.til` stage colour of the terrain page composer (`terrain_tile_composer`); reciprocal feeds EffectWorld lighting | HONORED | [orig: PolyTrn_SetTerrainTintColors @ 0x605e20; tile quads @ 0x60df0d; EffectWorld_TickInstancesAndLightScale @ 0x5aa170; overwritten foliage sample @ 0x606030] | **#19 FIXED; corrected 2026-07-13:** the `.til` tile overlay is the observable terrain renderer consumer (HALF under MODULATE2X, 254/255 default) and the reciprocal is live in effects. The texture bake is dead. Foliage computes the FULL-tint sample but the detail generator overwrites that packed diffuse with its source-height bend byte before emission; the former `FoliageDispatcher.terrain_tint` consumer was therefore removed. The terrain surface remains faithfully untinted. |
| `vertex_tint` (`vertex_rgb`) | `EnvFile` property | UNCONSUMED (*faithful*) | vestigial; retail modulator identity | Parsed for round-trip only. |
| `water_color` | `Water` → `u_water_color` | HONORED | [orig: TimeOfDay_ParseProperty @ 0x57c590] | Lit via `combine_terrain_light` + `lit_water_color`. |
| `water_height` | `Water` mesh height | HONORED | [orig: water_height <<15 parse] | Half-unit convention applied host-side; falls back to terrain data when absent. |
| `water_murk` | `Water` → `u_water_alpha` + underwater fog distance | HONORED | [orig: Environment_SetWaterMurk @ 0x57d4f0] | |
| `iris_percent` / `iris_center` | `EnvFile` → `WeatherRuntime` iris retarget over the marched 3-sample camera-ray average (`set_iris_samples`) → the modulator chain → `ColorSrcGlobalGain` and every colour block | HONORED | [orig: Terrain_SectorComputeLighting @ 0x5c7550 tail; Environment_ComputeAmbientLightAlongDirection @ 0x5c7a00; modulator chain @ 0x57e512 / 0x57ef97] | **Curve recovered (C6, G2)**: the engine's *global auto-exposure*, `gain = iris_center·64·((p/100)/(2·maxSceneLum) + 1 − p/100)`, clamped 0..255, 64 = identity, averaged over 3 view-ray samples and chased by the modulator that scales **every** color block. The modulator chain is live since REN-5 (env #17, 2026-07-06) and the marched samples since D-RLIT-2 (2026-08-22); row flipped 2026-09-24 (env-tod-re.md §Iris auto-exposure). |
| `ceiling_color` / `floor_color` | `WeatherRuntime` smoothing → `EnvironmentState::ceiling_color()` / `floor_color()` → the interior object lerp, the indoor ambient and the iris indoor samples | HONORED | [orig: indoor exposure inputs @ 0x5c7646..0x5c76fe; g_EnvCeilingFloorBlend → effect world @ 0x5f7163; Render_SetupEntityLightingAndShaderConstants @ 0x5d9a6a..0x5d9c71] | **Consumers pinned (C6, G4)** and live: indoors they replace sky/ground as the iris-exposure inputs; every contained drawn entity lerps its hemisphere from floor/ceiling to ground/sky by the building's `light_transfer` (`light_runtime`, render-lighting-re.md); their blend is the effects/particles *indoor ambient*. The getters stay raw under NVG; the NVG rewrite of ceiling/floor lives only in the object block (`EnvironmentState::build_light_values`). Row flipped 2026-09-24. |
| `lightning_color` | `WeatherState::lightning_color` -> the flash additives into sky/fog/skyfog/ground, the `lightning` WAC | HONORED | [orig: Environment_SetLightningFlash @ 0x57d320; thunder @ 0x57ecfb/0x57edc4; SETFLASH1 @ 0x429ec9; WAC flash/farflash @ 0x4ed500/0x4ed510] | Injection math, both sequencer tables, the `flash`/`farflash` handlers and the thunder one-shots are live (2026-08-30, env-tod-re.md #15). |

## Sky dome, clouds, celestial

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `sky_speed` | `WeatherCore` cloud-scroll core (rate ramp + integer accumulators) → `SkyDome` UV offsets + `Water` scroll rate | HONORED | [orig: rate ramp @ 0x57eecc; accumulators Environment_UpdateWeatherTick @ 0x57f1a5..0x57f1d1; consumption Render_Skybox @ 0x5791de..0x579260] | ×{1, 1, 2/3, 4/3} layer factors; accumulator NEGATIVE on U (env #26). |
| `sky_height` | `SkyDome` dome scale → `u_sky_height` (mesh from `engine/formats/env build_sky_dome_mesh`, Y scale in the vertex shader — env #20); celestial dome distance | HONORED | [orig: SkyDome_BuildMesh @ 0x578db0; rebuild gate Environment_ApplyFogAndAmbient @ 0x57e4f4] | Default carries the original raw-200 quirk; retail smooths the height (env #27), reimpl applies it instantly. |
| `sky_map1` / `sky_map2` | `SkyDome` → `u_cloud_tex1/2` | HONORED | [orig: Path_ReplaceOrAppendExtension @ 0x57cc4b (.pcx coercion)] | Coercion handled by the texture resolver, not at parse. |
| `advanced_clouds` | `SkyDome` mode switch | HONORED | [orig: Render_Skybox fixed-function pass @ 0x579b42] | 0 forces the dome to `cloud_tint`; ≠0 takes the keyframed-color path. |
| `cloud_tint` (`cloud_rgb`) | `SkyDome` → `u_flat_color` (flat pass only) | HONORED | [orig: dome material AMBIENT vs D3DRS_AMBIENT=white @ 0x579b42..0x579bb6; xref sweep of g_EnvCloudBlock @ 0x26c64a4] | **Flipped by C7**: `cloud_rgb` now colors the dome *only* in the `advanced_clouds 0` flat pass (textureless, per the C7 pre-port read), exactly the witnessed scope. The fabricated keyframed-path `u_cloud_tint * 2.0` is deleted. |
| `sun_3di` / `moon_3di` | `Celestial` model load + placement; each disc draws its authored SELFLUM material at its own UPL_INTENSITY | HONORED | [orig: EffectWorld_LoadCelestialModels @ 0x5adc50; Render_CelestialBodies @ 0x5acaa0] | 2026-09-24: the body alpha rides the CTRL register UPL_INTENSITY into the model's FF_ST_AD_LUM material, no tint override (env-tod-re.md §Celestial bodies). |
| `star_3di` | `Celestial` model load only | UNCONSUMED (*faithful*) | [orig: EffectWorld_LoadCelestialModels @ 0x5adcd6/0x5add06; Star_RenderField_Unused @ 0x5ad9c0 has no caller] | Re-graded 2026-09-24: retail loads and names the star model but never draws it (env #33); the reimpl's star field is deleted. |
| `glare_3di` | `Celestial` glare model through its authored SELFLUM material in the post-particle overlay stage (`Celestial.get_overlay_bodies()`) and the bloom source + `GlareOcclusion` (env #14 closed 2026-07-06) | HONORED | [orig: Render_SkyboxSunGlow @ 0x5acd00 — window/hysteresis/dot⁴ glow chain; Render_CelestialBodies @ 0x5acaa0] | Terrain ray march = the ENG-3 lo-res DDA port (`TerrainData.raycast_terrain`, `engine/runtime/terrain_query` `[orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80]`); the 32-unit bilinear stand-in retired with #209 (2026-07-08). |

## Time-of-day keyframes (16 slots × 12 colors)

Keyframe `time`, bracketing, stable sort, and the integer channel lerp (with the
original's 63356 snap quirk) are HONORED — [orig: Environment_SortAndSnapshotKeyframes
@ 0x57c240; Environment_FindKeyframeSegment @ 0x57dd80; Color_InterpolateRGB888 @
0x57c2f0] via `env::interpolate_tod`. Per color:

| Color | Chain | Status | Notes |
|---|---|---|---|
| `sun` | day light color + sun body tint (`MissionEnvironment`, `Celestial`) | HONORED | Selected vs moon by day phase [orig: @ 0x57d870]. |
| `moon` | night light color + moon body tint | HONORED | |
| `ground` | fill/ambient light → shader globals | HONORED | |
| `fog` | doubled (`double_saturate`) → fog uniforms | HONORED | [orig: Environment_UpdateWeatherTick @ 0x57f17c]. |
| `sky` | sky ambient → shader globals | HONORED | |
| `skyfog` | `get_frame_clear_color()` (horizon-blended, then doubled with saturation) -> `EnvironmentState::frame_clear_color_for` -> the GameWorld `ClearColor` clear; `get_skyfog_color()` uses the same render-space scale | HONORED (runtime) | [orig: Environment_UpdateWeatherTick blend @ 0x57f037-0x57f0a1, double @ 0x57f1b1; Render_ProcessMainSceneFrame @ 0x5ca771-0x5ca7bf] | **Closed 2026-07-05; corrected 2026-07-07** (env-tod-re.md #21): blend and post-blend doubling ported engine/formats/env-first; GameWorld clears with that render-space value (above/below-water choice witnessed). 2026-09-24: the beauty clear is thermal ? 0x808080 : eye above water ? skyfog : the lit water (`jle` @ 0x5ca790, so eye == water height clears to the lit water); it has no indoor black leg (the black clear is the water mirror's). 2026-09-26: the water mirror clears to the same skyfog, black under the indoors letter, through its own camera environment (`EnvironmentState::water_mirror_clear_color`; `Render_MainScene @ 0x5c1474`, `@ 0x5c1597`). Sentinel-mirror bleed still deliberately not replicated. |
| `skybase` | `SkyDome` → `u_sky_base` | HONORED | Dome combine ported by C7 — see below. |
| `skybright` | `SkyDome` → `u_sky_bright` | HONORED | |
| `skyhighlight` | `SkyDome` → `u_sky_highlight` | HONORED | |
| `cloudbase` | `SkyDome` → `u_cloud_base` | HONORED | |
| `cloudhighlight` | `SkyDome` → `u_cloud_highlight` | HONORED | |
| `cloudedge` | `SkyDome` → `u_cloud_edge` | HONORED | |

**Why the six dome colors are HONORED (flipped by C7).** `sky.gdshader` +
the sky presenter (now `godot/src/env/sky_dome.cpp`) are now a structural port of the recovered combine (C6 grill, G1; spec in
[env-tod-re.md](env-tod-re.md) §Sky dome), folded into one Godot pass: the textureless
gradient (`oD0 = lerp(skybase + (skybright−skybase)·(0.5·dot(N,sunDir)+0.5),
skyhighlight, prox⁸)`) under the dual-layer cloud combine (`rgb = density·cloudRamp +
(1−density)·skyBehind` with `density = tex0·tex1·2`, ramp/skyBehind from the
cloudbase/cloudedge/cloudhighlight lerp chain, blended by `α = (tex0.a·tex1.a·2)²·2`),
with the builder-formula dome normals, Y-only height scale, half-camera-height anchor,
dp3 clip-space proximity, and VS dome fog against the shared scene fog color. Every
invented term of the old shader (`pow 64/4/48` glows, `u_cloud_tint·2`, elevation
smoothsteps, clear-color floor mix) is deleted. The host preserves Godot reverse-Z for
the rendered `POSITION`, but converts the proximity vectors to D3D forward depth
(`z = w - z`) before the witnessed dp3; this closes env #20's residual view-motion
facet. Sun/moon glow no longer comes from the dome shader at all —
celestial bodies are `Celestial`'s job, as in the original.

2026-09-24 (the rendering parity pass; env-tod-re.md §Sky dome): the combine
now runs as two Godot passes, the gradient on its own first rung and the cloud
pass (`sky_clouds.gdshader`) after the celestial bodies, as `Render_Skybox`
draws them. Both write no depth and pin the rendered depth to the far plane
(`depth_draw_never`), so the world always draws over the dome. Under the
first-person NVG view the dome constants take the NVG unpack, and under the
thermal view the sky constants go to 1.0, the cloud constants to 0.9 and the
dome fog to 0x808080 (`runtime/environment/sky_frame.cpp`).

## BMS mission overrides

All six override slots are HONORED, applied non-persistently over a shadowed base
config (the saved `.env` is never contaminated): `water_height` (attrib bit 0x1),
`fog_level` (0x2), `fog_color` (0x4), `water_color`, `water_murk`, `start_time` —
[orig: Game_LoadTerrainDuringConnect @ 0x520710; Game_StartMission @ 0x525383/0x525393;
Environment_SetWaterColor @ 0x57d510; Environment_SetWaterMurk @ 0x57d4f0;
Environment_SetCurrentTime @ 0x57c4b0]. See `EnvFile::apply_mission_overrides`.

## Grill targets — dispositions (C6 session, 2026-06-11)

All six targets **closed** (full session record: env-tod-re.md, C6 appendix). This table
is the fix wave's (C7) work-order list:

| Target | Disposition | Where | C7 work order |
|---|---|---|---|
| G1 sky dome combine | **closed — recovered**, then **ported by C7** (embedded vs_1_1 sources + TSS tables; cloud_rgb = fixed-function-only) | env-tod-re.md §Sky dome + C7 addendum | DONE: `sky.gdshader` + the sky presenter (now `godot/src/env/sky_dome.cpp`) rewritten from the spec; keyframed-path `u_cloud_tint` deleted; dead c25 not replicated; dome rows flipped HONORED |
| G2 iris curve | **closed — recovered** (global auto-exposure, 64 = identity) | env-tod-re.md §Iris auto-exposure | Spec ready; requires the modulator chain — defer implementation, keep UNCONSUMED badge |
| G3 terrain_rgb consumers | **closed — refuted terrain-inert** (bake / water quads / foliage all live) | env-tod-re.md §iris/terrain_rgb | Row stays PARTIAL with the real gap named; per-consumer port decisions ride the terrain/foliage work, not C7 |
| G4 ApplyFogAndAmbient walk | **closed — complete** (8-row walk; exposure → `Render_LightScaleRGB` shader constant) | env-tod-re.md §…walk | Ceiling/floor: **keep** (live exposure inputs + effects indoor ambient); badge as deferred-consumer, do not delete |
| G5 overcast precedence | **closed — corrected** (additive after `.trn` success; never a fallback; missing `.trn` aborts) | env-tod-re.md §Load pipeline | Comment-level in `engine/formats/env`; future overcast cross-fade uses corrected order |
| G6 thunder/oscillators | **closed — feeder** (`SETFLASH1`, trigger ids 0/0x80, sequencer B unreachable; no `.env` rain/wind keywords) | env-tod-re.md §weather tick | Hand to the WAC weather wave (not C7) |

Since landed: G2's modulator chain (REN-5, env #17) and marched samples
(D-RLIT-2, 2026-08-22) and G4's ceiling/floor consumers; the iris and
ceiling/floor rows above read HONORED from 2026-09-24.

## Editor follow-up (shipped by C7)

`EnvFile.get_field_consumption()` (godot/src/env/env_file.cpp) is the engine-side
consumption table — field → honored | partial | unconsumed, with the original anchor
and a plain-language note — and the Environment inspector badges PARTIAL (◐) and
UNCONSUMED (○) rows with tooltips instead of silently accepting edits. C7 also added
the previously-unexposed fields (water murk, lightning, ceiling/floor, vertex tint,
iris) as editable Advanced rows, badged where deferred. The table mirrors this matrix;
rows in both flip only with a citation from the grill record.
