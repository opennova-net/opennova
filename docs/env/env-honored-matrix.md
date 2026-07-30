# Environment honored-matrix: which `.env` fields the renderer actually consumes

The Environment workspace exposes nearly every `env::Config` field (water height still
arrives via terrain/BMS rather than the inspector), but not every field reaches a
visible render input yet. This matrix classifies each editor-exposed field
so authors (and the editor UI) can tell a live control from a parsed-but-deferred one,
and so renderer changes flip rows only with citations. It is the audit behind the
editor-depth roadmap's environment fidelity work; the reverse-engineering record it
leans on is [env-tod-re.md](env-tod-re.md).

Classifications:

- **HONORED** — reaches a render input through the witnessed original behavior.
- **PARTIAL** — reaches the renderer, but through approximated math, an incomplete
  trigger path, or only in some modes. Each PARTIAL row names what is missing.
- **UNCONSUMED** — parsed, edited, and round-tripped, but no render effect today.
  Rows marked *faithful* are unconsumed in retail too; the rest await a ported
  consumer and must **not** be faked (an untracked divergence would mis-train
  authors).

Chains below run editor inspector → `EnvFile` (godot/engine/env/env_file.h) →
runtime nodes (`NovaEnvironment` / `NovaSky` / `NovaWater` / `NovaWeather` /
`NovaCelestial`, godot/engine/environment/) → shader. The portable math lives in
`libs/env` (`env.h`, `env_weather.h`, `env_celestial.h`, `env_water_render.h`).

## Scalars, fog, water

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `env_name` | inspector → `EnvFile.env_name` | UNCONSUMED (*faithful*) | authoring extension; retail has no keyword | Display/round-trip only. |
| `timeofday` (enum) | `EnvFile` property | UNCONSUMED (*faithful*) | classification string only | Never read by the gradient in retail either. |
| `curtime` | `EnvFile` → `NovaEnvironment.time_of_day` bootstrap | HONORED | [orig: Environment_SetCurrentTime @ 0x57c4b0] | Initial TOD at load. |
| `envscale` | parse-time scale on every `*_rgb` getter | HONORED | [orig: Env_ParseEnvScale @ 0x840950] | Raw storage + getter-side scaling; cross-file parse-order bleed deliberately not replicated (see env-tod-re.md). |
| `fog_level` | `NovaEnvironment.get_fog_*` → terrain/water shader uniforms | HONORED | [orig: Render_SetFogState @ 0x58a950; Environment_GetFogEndDistance @ 0x57e3e0] | Via `env_render::compute_fog_params`. |
| `fog_type` | same | HONORED | [orig: Render_SetFogState @ 0x58a950] | All four policies (0=exp, 1/2/3 linear variants) in `env_render.cpp`. |
| `terrain_tint` (`terrain_rgb`) | `EnvFile.get_terrain_tint` → `NovaEnvironment.get_tile_overlay_tint` (`u_tile_overlay_tint`); reciprocal feeds EffectWorld lighting | HONORED | [orig: PolyTrn_SetTerrainTintColors @ 0x605e20; tile quads @ 0x60df0d; EffectWorld_TickInstancesAndLightScale @ 0x5aa170; overwritten foliage sample @ 0x606030] | **#19 FIXED; corrected 2026-07-13:** the `.til` tile overlay is the observable terrain renderer consumer (HALF under MODULATE2X, 254/255 default) and the reciprocal is live in effects. The texture bake is dead. Foliage computes the FULL-tint sample but the detail generator overwrites that packed diffuse with its source-height bend byte before emission; the former `NovaFoliageDispatcher.terrain_tint` consumer was therefore removed. The terrain surface remains faithfully untinted. |
| `vertex_tint` (`vertex_rgb`) | `EnvFile` property | UNCONSUMED (*faithful*) | vestigial; retail modulator identity | Parsed for round-trip only. |
| `water_color` | `NovaWater` → `u_water_color` | HONORED | [orig: TimeOfDay_ParseProperty @ 0x57c590] | Lit via `combine_terrain_light` + `lit_water_color`. |
| `water_height` | `NovaWater` mesh height | HONORED | [orig: water_height <<15 parse] | Half-unit convention applied host-side; falls back to terrain data when absent. |
| `water_murk` | `NovaWater` → `u_water_alpha` + underwater fog distance | HONORED | [orig: Environment_SetWaterMurk @ 0x57d4f0] | |
| `iris_percent` / `iris_center` | `EnvFile` properties only | UNCONSUMED | [orig: terrain_sector_compute_lighting @ 0x5c7550 tail; compute_ambient_light_along_direction @ 0x5c7a00] | **Curve recovered (C6, G2)** — it is the engine's *global auto-exposure*: `gain = iris_center·64·((p/100)/(2·maxSceneLum) + 1 − p/100)`, clamped 0..255, 64 = identity, averaged over 3 view-ray samples and chased by the modulator that scales **every** color block. Consumer system (modulator chain) unbuilt — still do not fake (env-tod-re.md §Iris auto-exposure, divergence #17). |
| `ceiling_color` / `floor_color` | `NovaWeather` computes `_outdoor_color` / `_indoor_color` | PARTIAL | [orig: indoor exposure inputs @ 0x5c7646..0x5c76fe; Env_CeilingFloorBlend → effect world @ 0x5f7163] | **Consumers pinned (C6, G4)**: indoors they replace sky/ground as the iris-exposure inputs, and their 0.707+0.707 blend is the effects/particles *indoor ambient* (stored beside the outdoor combined light). Keep the computation — it becomes live when the modulator chain / effects ambient land. |
| `lightning_color` | `NovaWeather` additive injection into smoothed sky/fog/fill | PARTIAL | [orig: Environment_SetLightningFlash @ 0x57d320; thunder @ 0x57ecfb/0x57edc4; SETFLASH1 @ 0x429ec9] | Injection math and both sequencer tables ported (`env_weather.h`). **Trigger + thunder fully specced (C6, G6)**: retail fires sequencer A via the `SETFLASH1 [n]` net text command (default 16); thunder = SoundBank trigger 0 (A) / 0x80 (B) on the bank at `dword_24E0914`; sequencer B is unreachable in retail (no SETFLASH2, debug hooks orphaned). Wiring lands with WAC weather. |

## Sky dome, clouds, celestial

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `sky_speed` | `NovaWeatherCore` cloud-scroll core (rate ramp + integer accumulators) → `NovaSky` UV offsets + `NovaWater` scroll rate | HONORED | [orig: rate ramp @ 0x57eecc; accumulators Environment_UpdateWeatherTick @ 0x57f1a5..0x57f1d1; consumption render_skybox @ 0x5791de..0x579260] | ×{1, 1, 2/3, 4/3} layer factors; accumulator NEGATIVE on U (env #26). |
| `sky_height` | `NovaSky` dome scale → `u_sky_height` (mesh from `libs/env build_sky_dome_mesh`, Y scale in the vertex shader — env #20); celestial dome distance | HONORED | [orig: build_sky_dome_mesh @ 0x578db0; rebuild gate Environment_ApplyFogAndAmbient @ 0x57e4f4] | Default carries the original raw-200 quirk; retail smooths the height (env #27), reimpl applies it instantly. |
| `sky_map1` / `sky_map2` | `NovaSky` → `u_cloud_tex1/2` | HONORED | [orig: Path_ReplaceOrAppendExtension @ 0x57cc4b (.pcx coercion)] | Coercion handled by the texture resolver, not at parse. |
| `advanced_clouds` | `NovaSky` mode switch | HONORED | [orig: render_skybox fixed-function pass @ 0x579b42] | 0 forces the dome to `cloud_tint`; ≠0 takes the keyframed-color path. |
| `cloud_tint` (`cloud_rgb`) | `NovaSky` → `u_flat_color` (flat pass only) | HONORED | [orig: dome material AMBIENT vs D3DRS_AMBIENT=white @ 0x579b42..0x579bb6; xref sweep of Env_CloudBlock @ 0x26c64a4] | **Flipped by C7**: `cloud_rgb` now colors the dome *only* in the `advanced_clouds 0` flat pass (textureless, per the C7 pre-port read), exactly the witnessed scope. The fabricated keyframed-path `u_cloud_tint * 2.0` is deleted. |
| `sun_3di` / `moon_3di` / `star_3di` | `NovaCelestial` model load + placement + keyframe tint | HONORED | [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] | |
| `glare_3di` | `NovaCelestial` additive overlay + `NovaGlareOcclusion` (env #14 closed 2026-07-06) | HONORED | [orig: render_skybox_sun_glow @ 0x5acd00 — window/hysteresis/dot⁴ glow chain; render_celestial_bodies @ 0x5acaa0] | Terrain ray march = the ENG-3 lo-res DDA port (`NovaTerrainData.raycast_terrain`, `libs/terrain_query` `[orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80]`); the 32-unit bilinear stand-in retired with #209 (2026-07-08). |

## Time-of-day keyframes (16 slots × 12 colors)

Keyframe `time`, bracketing, stable sort, and the integer channel lerp (with the
original's 63356 snap quirk) are HONORED — [orig: Environment_SortAndSnapshotKeyframes
@ 0x57c240; Environment_FindKeyframeSegment @ 0x57dd80; Color_InterpolateRGB888 @
0x57c2f0] via `env::interpolate_tod`. Per color:

| Color | Chain | Status | Notes |
|---|---|---|---|
| `sun` | day light color + sun body tint (`NovaEnvironment`, `NovaCelestial`) | HONORED | Selected vs moon by day phase [orig: @ 0x57d870]. |
| `moon` | night light color + moon body tint | HONORED | |
| `ground` | fill/ambient light → shader globals | HONORED | |
| `fog` | doubled (`double_saturate`) → fog uniforms | HONORED | [orig: Environment_ApplyFogAndAmbient @ 0x57f17c]. |
| `sky` | sky ambient → shader globals | HONORED | |
| `skyfog` | `get_frame_clear_color()` (horizon-blended, undoubled) -> the GameWorld `ClearColor` clear; `get_skyfog_color()` stays the doubled render color | HONORED (runtime) | [orig: Environment_UpdateWeatherTick blend @ 0x57f037-0x57f0a1; Render_ProcessMainSceneFrame @ 0x5ca776-0x5ca7bf; Clear halving @ 0x67715d] | **Closed 2026-07-05** (env-tod-re.md #21): blend ported libs/env-first, byte-exact; GameWorld clears with it (above/below-water choice witnessed). Editor preview adoption rides ENV-1; sentinel-mirror bleed still deliberately not replicated. |
| `skybase` | `NovaSky` → `u_sky_base` | HONORED | Dome combine ported by C7 — see below. |
| `skybright` | `NovaSky` → `u_sky_bright` | HONORED | |
| `skyhighlight` | `NovaSky` → `u_sky_highlight` | HONORED | |
| `cloudbase` | `NovaSky` → `u_cloud_base` | HONORED | |
| `cloudhighlight` | `NovaSky` → `u_cloud_highlight` | HONORED | |
| `cloudedge` | `NovaSky` → `u_cloud_edge` | HONORED | |

**Why the six dome colors are HONORED (flipped by C7).** `sky.gdshader` +
`nova_sky.gd` are now a structural port of the recovered combine (C6 grill, G1; spec in
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
celestial bodies are `NovaCelestial`'s job, as in the original.

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
| G1 sky dome combine | **closed — recovered**, then **ported by C7** (embedded vs_1_1 sources + TSS tables; cloud_rgb = fixed-function-only) | env-tod-re.md §Sky dome + C7 addendum | DONE: `sky.gdshader` + `nova_sky.gd` rewritten from the spec; keyframed-path `u_cloud_tint` deleted; dead c25 not replicated; dome rows flipped HONORED |
| G2 iris curve | **closed — recovered** (global auto-exposure, 64 = identity) | env-tod-re.md §Iris auto-exposure | Spec ready; requires the modulator chain — defer implementation, keep UNCONSUMED badge |
| G3 terrain_rgb consumers | **closed — refuted terrain-inert** (bake / water quads / foliage all live) | env-tod-re.md §iris/terrain_rgb | Row stays PARTIAL with the real gap named; per-consumer port decisions ride the terrain/foliage work, not C7 |
| G4 ApplyFogAndAmbient walk | **closed — complete** (8-row walk; exposure → `Render_LightScaleRGB` shader constant) | env-tod-re.md §…walk | Ceiling/floor: **keep** (live exposure inputs + effects indoor ambient); badge as deferred-consumer, do not delete |
| G5 overcast precedence | **closed — corrected** (additive after `.trn` success; never a fallback; missing `.trn` aborts) | env-tod-re.md §Load pipeline | Comment-level in `libs/env`; future overcast cross-fade uses corrected order |
| G6 thunder/oscillators | **closed — feeder** (`SETFLASH1`, trigger ids 0/0x80, sequencer B unreachable; no `.env` rain/wind keywords) | env-tod-re.md §weather tick | Hand to the WAC weather wave (not C7) |

## Editor follow-up (shipped by C7)

`EnvFile.get_field_consumption()` (godot/engine/env/env_file.cpp) is the engine-side
consumption table — field → honored | partial | unconsumed, with the original anchor
and a plain-language note — and the Environment inspector badges PARTIAL (◐) and
UNCONSUMED (○) rows with tooltips instead of silently accepting edits. C7 also added
the previously-unexposed fields (water murk, lightning, ceiling/floor, vertex tint,
iris) as editable Advanced rows, badged where deferred. The table mirrors this matrix;
rows in both flip only with a citation from the grill record.
