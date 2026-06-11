# Environment honored-matrix: which `.env` fields the renderer actually consumes

The Environment workspace exposes every `env::Config` field, but not every field
reaches a visible render input yet. This matrix classifies each editor-exposed field
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
`libs/env` (`env.h`, `env_render.h`).

## Scalars, fog, water

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `env_name` | inspector → `EnvFile.env_name` | UNCONSUMED (*faithful*) | authoring extension; retail has no keyword | Display/round-trip only. |
| `timeofday` (enum) | `EnvFile` property | UNCONSUMED (*faithful*) | classification string only | Never read by the gradient in retail either. |
| `curtime` | `EnvFile` → `NovaEnvironment.time_of_day` bootstrap | HONORED | [orig: Environment_SetCurrentTime @ 0x57c4b0] | Initial TOD at load. |
| `envscale` | parse-time scale on every `*_rgb` getter | HONORED | [orig: Env_ParseEnvScale @ 0x840950] | Raw storage + getter-side scaling; cross-file parse-order bleed deliberately not replicated (see env-tod-re.md). |
| `fog_level` | `NovaEnvironment.get_fog_*` → terrain/water shader uniforms | HONORED | [orig: Render_SetFogState @ 0x58a950; Environment_GetFogEndDistance @ 0x57e3e0] | Via `env_render::compute_fog_params`. |
| `fog_type` | same | HONORED | [orig: Render_SetFogState @ 0x58a950] | All four policies (0=exp, 1/2/3 linear variants) in `env_render.cpp`. |
| `terrain_tint` (`terrain_rgb`) | `EnvFile.get_terrain_tint` → `NovaEnvironment.get_terrain_lighting_attenuation` | PARTIAL | [orig: reciprocal consumer EffectWorld_TickInstancesAndLightScale @ 0x5aa170] | Attenuation **hard-returns `Vector3.ONE`** (`nova_environment.gd`, `get_terrain_lighting_attenuation`): the recovered reciprocal LUT feeds *effects brightness*, a system we have not built. Kept neutral until the consumer set is pinned (grill target G3). |
| `vertex_tint` (`vertex_rgb`) | `EnvFile` property | UNCONSUMED (*faithful*) | vestigial; retail modulator identity | Parsed for round-trip only. |
| `water_color` | `NovaWater` → `u_water_color` | HONORED | [orig: TimeOfDay_ParseProperty @ 0x57c590] | Lit via `combine_terrain_light` + `lit_water_color`. |
| `water_height` | `NovaWater` mesh height | HONORED | [orig: water_height <<15 parse] | Half-unit convention applied host-side; falls back to terrain data when absent. |
| `water_murk` | `NovaWater` → `u_water_alpha` + underwater fog distance | HONORED | [orig: Environment_SetWaterMurk @ 0x57d4f0] | |
| `iris_percent` / `iris_center` | `EnvFile` properties only | UNCONSUMED | [orig: terrain_sector_compute_lighting @ 0x5c7930..0x5c79f4] | Consumer is the entity/sector lighting view-distance curve — unbuilt system; exact curve unknown (grill target G2). Do not fake. |
| `ceiling_color` / `floor_color` | `NovaWeather` computes `_outdoor_color` / `_indoor_color` | PARTIAL | [orig: ceiling/floor blend inside the weather state walk] | Computed (`nova_weather.gd:194-204`) then **never read** — verified no consumer in the tree. They look like state for the indoor/under-cover modulator we have not wired. Do not delete without the full `Environment_ApplyFogAndAmbient @ 0x57e440` walk (grill target G4). |
| `lightning_color` | `NovaWeather` additive injection into smoothed sky/fog/fill | PARTIAL | [orig: Environment_SetLightningFlash @ 0x57d320] | Injection math and both sequencer tables ported (`env_render.h`); **no trigger source** fires it yet (weather/WAC system pending) and thunder audio is unwired. |

## Sky dome, clouds, celestial

| Field | Chain | Status | Original anchor | Notes |
|---|---|---|---|---|
| `sky_speed` | `NovaSky` scroll accumulators (+ water) | HONORED | [orig: render_skybox @ 0x5791de; Environment_UpdateWeatherTick @ 0x57f1a5] | ×{1, 1, 2/3, 4/3} layer factors. |
| `sky_height` | `NovaSky` dome scale → `u_sky_height`; celestial dome distance | HONORED | [orig: build_sky_dome_mesh @ 0x578db0; Environment_ApplyFogAndAmbient @ 0x57e4f7] | Default carries the original raw-200 quirk. |
| `sky_map1` / `sky_map2` | `NovaSky` → `u_cloud_tex1/2` | HONORED | [orig: Path_ReplaceOrAppendExtension @ 0x57cc4b (.pcx coercion)] | Coercion handled by the texture resolver, not at parse. |
| `advanced_clouds` | `NovaSky` mode switch | HONORED | [orig: render_skybox fixed-function pass @ 0x579b42] | 0 forces the dome to `cloud_tint`; ≠0 takes the keyframed-color path. |
| `cloud_tint` (`cloud_rgb`) | `NovaSky` → `u_cloud_tint` | PARTIAL | [orig: dome material color in the @ 0x579b42 pass] | Honored as the advanced_clouds=0 dome color, but the shader **also** multiplies it into the cloud layer in the keyframed path (`sky.gdshader`, `cloud_color *= u_cloud_tint * 2.0`) — scope unwitnessed (grill target G1). |
| `sun_3di` / `moon_3di` / `star_3di` | `NovaCelestial` model load + placement + keyframe tint | HONORED | [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] | |
| `glare_3di` | `NovaCelestial` additive overlay, intensity from `compute_sun_glare` | PARTIAL | [orig: render_skybox_sun_glow @ 0x5acd00; compute_sun_glare_and_fog_blend @ 0x5ad610] | Intensity formula (dot³², ×192, clamp 255) matches; the 8-jittered-ray terrain **occlusion is held at full brightness** (tracked divergence). |

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
| `skyfog` | doubled → horizon/clear color | HONORED | Sentinel-mirror (0xC0C0FF leftover-slot bleed) deliberately not replicated. |
| `skybase` | `NovaSky` → `u_sky_base` | PARTIAL | Consumed in the dome combine — see below. |
| `skybright` | `NovaSky` → `u_sky_bright` | PARTIAL | |
| `skyhighlight` | `NovaSky` → `u_sky_highlight` | PARTIAL | |
| `cloudbase` | `NovaSky` → `u_horizon_color` | PARTIAL | |
| `cloudhighlight` | `NovaSky` → `u_ground_fog_color` | PARTIAL | |
| `cloudedge` | `NovaSky` → `u_secondary_ambient` | PARTIAL | |

**Why the six dome colors are PARTIAL.** All six are interpolated and pushed as
uniforms, but `sky.gdshader`'s fragment combine is an **approximation, not a port**:

- The zenith gradient `u_sky_base + (u_sky_bright - u_sky_base) * t` matches the
  original's `c11 + c12·t` constant form (c12 is uploaded as skybright−skybase), but
  the blend driver `t` (`sun_facing * 0.5 + 0.5`, elevation smoothsteps `0.1`/`0.4`)
  is invented.
- `cloudbase`/`cloudhighlight` are repurposed as a horizon/ground-fog gradient and a
  cloud-density color ramp; `cloudedge` is added as a flat ambient (`* 0.5`). In the
  original these are the c24..c27 cloud-texture color ramp of the pixel stage under
  `render_skybox @ 0x579080`.
- Every exponent and scale (`pow 64`, `pow 4`, `pow 48`, `* 2.0`, `* 0.15`,
  `* 0.8`, cloud mix `0.7/0.3`) is uncited.

This is why environment color edits can look "not honored": the sliders feed real
uniforms, but the combine does not respond the way the original dome would.
Recovering the true combine is grill target **G1**, and the fix wave only lands what
that grill proves.

## BMS mission overrides

All six override slots are HONORED, applied non-persistently over a shadowed base
config (the saved `.env` is never contaminated): `water_height` (attrib bit 0x1),
`fog_level` (0x2), `fog_color` (0x4), `water_color`, `water_murk`, `start_time` —
[orig: Game_LoadTerrainDuringConnect @ 0x520710; Game_StartMission @ 0x525383/0x525393;
Environment_SetWaterColor @ 0x57d510; Environment_SetWaterMurk @ 0x57d4f0;
Environment_SetCurrentTime @ 0x57c4b0]. See `EnvFile::apply_mission_overrides`.

## Grill targets (the unknowns a grill-ida session must close)

- **G1 — sky dome pixel-stage combine** under `render_skybox @ 0x579080`: how c11
  (skybase), c12 (skybright−skybase), c15 (skyhighlight), c24 (cloudbase), c25
  (cloudhighlight−cloudbase), c26 (cloudedge), c27 (cloudhighlight) actually combine
  per fragment, and `cloud_rgb`'s scope in the keyframed path. Replaces the invented
  `sky.gdshader` mapping.
- **G2 — iris curve** x87 tail @ `0x5c7930..0x5c79f4` inside
  `terrain_sector_compute_lighting @ 0x5c7550`: exact inputs and shaping of the
  `iris_percent`/`iris_center` response.
- **G3 — `Env_TerrainColorPacked` consumers** (xrefs of the packed global): confirm
  or refute any terrain-tint consumer beyond the `@ 0x5aa170` effects reciprocal.
  Decides whether "terrain_rgb does nothing to terrain" is *faithful*.
- **G4 — full `Environment_ApplyFogAndAmbient @ 0x57e440` walk**: every state block
  pushed per frame, the ceiling/floor application points, and
  `compute_ambient_light_along_direction @ 0x5c7a00` wiring.
- **G5 — overcast/.trn precedence** at the `@ 0x57dbeb` branch of
  `Environment_LoadTimeOfDayConfig @ 0x57db30`: does `overcast.def` load only when
  the `.trn` has no TOD block, or always after it?
- **G6 — weather oscillators + thunder**: which lightning sequencer epochs trigger
  thunder sounds, and whether the rain/wind rings carry `.env`-tunable parameters
  (feeds the WAC weather work, not this matrix).

## Editor follow-up

The fix wave (roadmap slice C7) adds an engine-side consumption table on `EnvFile`
(field → honored | deferred, with the original anchor) so the Environment inspector
can badge UNCONSUMED/PARTIAL fields as "not yet consumed by the renderer" instead of
silently accepting edits. Rows in this matrix flip only with a citation from the
grill record.
