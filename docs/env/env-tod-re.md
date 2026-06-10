# .env environments, time-of-day, and the atmosphere stack

How Joint Operations loads `.env` environment files, drives its time-of-day pipeline, and
renders fog, sky, weather, and celestial bodies, as witnessed in the original engine. This is
the reverse-engineering record behind `libs/env` (format + TOD math), the `EnvFile` /
`NovaEnvKeyframe` wrappers, and the `NovaEnvironment` / `NovaSky` / `NovaWater` / `NovaWeather`
/ `NovaCelestial` runtime.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase `0x400000`,
IDB `Jointops.exe.kong.i64`), grilled 2026-06-09. All addresses are absolute in that image.
This record supersedes the jodemo-era citations (`0x53E030`/`0x53E3F0`/`0x53FA70`/`0x53FCC0`/
`0x53F5D0`) and the never-written `engine_spec_env.md`; those addresses do not exist in the
retail image.

---

## Correspondence map

| Original (Jointops retail) | Reimplementation |
|---|---|
| `Environment_InitDefaults @ 0x57c010` | `env::Config` member defaults (`libs/env/include/env/env.h`) |
| `TimeOfDay_ParseProperty @ 0x57c590` | `env::load_env` keyword handling (`libs/env/src/env.cpp`) |
| `Environment_ParseTimeString @ 0x57c500` | `env::hhmm_to_hours_fp` (`libs/env/src/env.cpp`) |
| `Color_ScaleRGBAndPack @ 0x57f890` | parse-side quantization inside `env::interpolate_tod` |
| `Environment_SortAndSnapshotKeyframes @ 0x57c240` | stable sort in `env::load_env` / `save_env` |
| `Environment_FindKeyframeSegment @ 0x57dd80` | bracketing in `env::interpolate_tod` |
| `Environment_LerpKeyframeSet @ 0x57c3b0` | `env::interpolate_tod` channel loop |
| `Color_InterpolateRGB888 @ 0x57c2f0` | integer channel lerp in `env::interpolate_tod` |
| `Environment_ComputeTimeOfDayColors @ 0x57de40` | `env::interpolate_tod` + `env_render` day phase |
| `Environment_ComputeSunDirection @ 0x57d6d0` | `env::compute_sun_direction` |
| `Terrain_ComputeMoonDirection @ 0x57d760` | `env::compute_moon_direction` |
| `Environment_GetLightDirectionFloat @ 0x57d870` | day/night light selection (`env_render`) |
| `Environment_LoadTimeOfDayConfig @ 0x57db30` | two-pass load model (see §Load pipeline) |
| `Environment_SnapStateToTargets @ 0x57d1e0` | mission-start snap (`env_render` weather state) |
| `Environment_UpdateWeatherTick @ 0x57e9b0` | `env_render` weather tick + `nova_weather.gd` |
| `interpolate_weather_color @ 0x57d9e0` | `env_render` channel smoother |
| `Environment_SetLightningFlash @ 0x57d320` | `env_render` lightning additive injection |
| `Environment_GetFogEndDistance @ 0x57e3e0` | `env_render::compute_fog_params` (end distance) |
| `Environment_ApplyFogAndAmbient @ 0x57e440` | fog application (`nova_environment.gd` + shaders) |
| `Render_SetFogState @ 0x58a950` | `env_render::compute_fog_params` (per-type start) |
| `render_skybox @ 0x579080` | `nova_sky.gd` + `godot/shaders/sky.gdshader` |
| `EffectWorld_LoadCelestialModels @ 0x5adc50` | `nova_celestial.gd` (Phase 3) |
| `Game_LoadTerrainDuringConnect @ 0x520710` | BMS override application (`EnvFile.apply_mission_overrides`) |
| `Game_StartMission @ 0x524360` (0x525371..0x525399) | BMS fog overrides + start TOD |
| `Environment_SetCurrentTime @ 0x57c4b0` / `Environment_SetTodRate @ 0x57c4f0` | runtime TOD state setters |
| `terrain_sector_compute_lighting @ 0x5c7550` | entity lighting + iris consumer (documented, deferred) |
| `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` | terrain_rgb reciprocal consumer (documented, deferred) |

Misnames fixed during the grill: `Render_SetFogParams @ 0x54b4b0` was an entity-pool sweeper
(now `Entity_DestroyUnreferencedPool4Entries`); `sub_57C4B0`/`sub_57C4F0` were "water
reflection"/"ambient G" in kong comments but are the current-time and TOD-rate setters.

## Format truths (parser, `TimeOfDay_ParseProperty @ 0x57c590`)

- **Time is 16.16 fixed-point HOURS, not HHMM.** `Environment_ParseTimeString @ 0x57c500`
  parses positionally from the string tail: `minutes = last two digits` (clamped 59),
  `hours = preceding digits` (clamped 23), returns `(h << 16) + (m << 16) / 60` (integer
  division). Strings shorter than 3 chars yield 0. All keyframe bracketing and interpolation
  happen in this hours space; a day is `0x180000` (24.0).
- **TOD keyframes live in 16 fixed slots of 52 bytes** (`Env_TodParseSlots @ 0x26c6070`):
  `{time, sun, moon, sky, ground, fog, skyfog, skybase, skybright, skyhighlight, cloudbase,
  cloudhighlight, cloudedge}`, colors packed `0x00RRGGBB`. The 17th and later `tod_begin`
  blocks do not allocate or store a time; their color lines keep writing into slot 16
  (@ 0x57c65b). Slots are never cleared between loads — colors a block does not set carry
  leftovers (engine quirk; benign because stock files set all 12).
- **There is no block-nesting state: `tod_end` is optional.** `tod_begin` simply advances
  the slot pointer (@ 0x57c647) and a stray `tod_end` resets it to scratch (@ 0x57c696) —
  shipped `FULL_03.ENV` / `FULL_05.ENV` contain a `tod_begin` inside an unterminated block
  and the engine accepts them. A reimpl that errors on "nested" blocks rejects retail data.
- **Colors quantize at parse.** `Color_ScaleRGBAndPack @ 0x57f890`: each component
  `int(value * envscale)`, truncated, clamped to <= 255 (no lower clamp), packed. `envscale`
  (`Env_ParseEnvScale @ 0x840950`) is a parse-state float applied to every subsequent `*_rgb`
  — including `water_rgb`, `cloud_rgb`, `lightning_rgb`, `ceiling_rgb`, `floor_rgb` — but not
  `terrain_rgb`. It resets to 1.0 once per load (`@ 0x57db49`), not per file, so a `.trn`
  envscale leaks into the `.env` pass until overridden.
- **fog_rgb mirrors into skyfog_rgb** only while the slot's skyfog still equals the sentinel
  `0xC0C0FF` (@ 0x57c9b8). The sentinel is seeded only on the scratch keyframe by
  `Environment_LoadTimeOfDayConfig` (@ 0x57db54); parse slots rely on leftovers.
- **Fixed-point scales per keyword:** `water_height << 15` (half world units in 16.16),
  `sky_height << 16`, `fog_level << 16`, `sky_speed << 10`, `curtime = ParseTimeString << 8`
  (8.24 hours). All of these parse with `atol` (integer truncation), not `atof`.
  `water_murk` is float, clamped only at the top to 0.99 (@ 0x57cba9). `iris_percent` /
  `iris_center` are floats, unscaled.
- **`sky_map1`/`sky_map2` are coerced to `.pcx`** via `Path_ReplaceOrAppendExtension`
  (@ 0x57cc4b); the four `*_3di` names are stored verbatim.
- **`tod_rate` is the day length in real minutes** (min 60): the per-tick curtime advance is
  `0x18000000 / (3720 * rate)` (@ 0x57d108) — 3720 = 60 s x 62 ticks. Default advance 75
  (≈ 1440-minute day).
- **`timeofday` maps to an enum** (`Env_TimeOfDayEnum @ 0x24d2364`: dawn/day/dusk/night),
  used elsewhere for classification; the gradient never reads it.
- **`vertex_rgb` has no parse case in retail JO** — vestigial from earlier titles. The global
  modulation block it once fed defaults to `0x404040` (1.0 in 6.6 fixed = identity).
- Unknown keywords are ignored. An optional parse hook (`Env_ParseHook @ 0x26c6058`) can
  intercept lines before the standard handling.

### Engine defaults (`Environment_InitDefaults @ 0x57c010`)

`cld_day1.pcx` / `cld_day1b.pcx`, `msun.3di` / `fmoon4.3di` / `mglare.3di` / `mstar.3di`,
iris 50.0 / 1.25, water murk 0.8, water color `0x685039` (104,80,57), terrain `0xFFFFFF`,
fog type 1, fog level 1024.0, sky speed 0, advanced_clouds 0, curtime 15:00, TOD advance 75.
Quirk: default sky_height is written as raw `200` where consumers read 16.16 (≈ 0.003 units —
flat sky); every shipped file sets `sky_height` so it never matters. Scratch TOD defaults:
sun `0x646440`, sky `0x404064`, ground `0x202020`, fog/skyfog `0xC0C0FF`.

## Load pipeline (`Environment_LoadTimeOfDayConfig @ 0x57db30`)

Two parse passes share the parser and the 16 slots:

1. `<map>.trn` (fallback `overcast.def` when absent/failing) → snapshot into
   `Env_TrnSnapshotTable @ 0x26c7414` (stable bubble sort by time, count at +832;
   `Environment_SortAndSnapshotKeyframes @ 0x57c240`).
2. keyframe count resets, then `<env>.env` → snapshot into
   `Env_EnvSnapshotTable @ 0x26c70d0`.

The runtime blend (`Env_OvercastBlend @ 0x26c6894`, spring-damped toward
`Env_OvercastBlendTarget`) interpolates the final TOD colors **between the .env snapshot and
the .trn/overcast snapshot** — overcast weather literally cross-fades to the second table.

## Time-of-day compute (`Environment_ComputeTimeOfDayColors @ 0x57de40`)

Runs per tick with `curtime + advance` (8.24 hours, wraps at `0x18000000`); no-op when the
.env snapshot is empty. Hardcoded day-phase windows (16.16 hours): sunrise ramp 05:40→06:20
with the sun/moon switch at 06:00, sunset ramp 18:25→19:05 with the switch at 18:45, ramp
width 20 minutes (`21840`); sets `Env_IsNightPhase @ 0x26c645c` and
`Env_DayPhaseBlend @ 0x26c6460`. Segment lookup (`Environment_FindKeyframeSegment @ 0x57dd80`)
wraps below the first keyframe to (last → first); fraction is a truncating integer
`(elapsed << 16) / duration`; duration 0 → fraction 0. Channel lerp
(`Color_InterpolateRGB888 @ 0x57c2f0`) is per-byte `(t*(b-a) + (a<<16) + 0x8000) >> 16`
(round half up). The set lerp (`Environment_LerpKeyframeSet @ 0x57c3b0`) clamps t to
`[0, 0x10000]` with a transposed-digits quirk: anything **above 63356** (not 65536) snaps to
full — a ~3.3% snap zone near 1.0 from an original source typo.

The selected light color is **sun by day, moon by night** (same flag picks the light
direction in `Environment_GetLightDirectionFloat @ 0x57d870`). Results write into per-color
state blocks (below).

## Color state blocks and the weather tick

Each color channel owns a 52-byte state block (fog `0x26c650c`, skyfog `0x26c6540`, light
`0x26c6574`, moon `0x26c65a8`, sky `0x26c65dc`, ground `0x26c6610`, modulator `0x26c6644`,
modulator2 `0x26c6678`, skybase `0x26c66b0`, skybright `0x26c66e4`, skyhighlight `0x26c6718`,
cloudbase `0x26c674c`, cloudhighlight `0x26c6780`, cloudedge `0x26c67b4`, plus statics ceiling
`0x26c6470`, cloud `0x26c64a4`, floor `0x26c64d8`):

| Offset (dwords) | Meaning |
|---|---|
| [0] | current packed render color (post modulation) |
| [1] | `[0] + [12]` saturating (lightning-bright variant; consumed by entity lighting) |
| [2..5] | B, G, R, A channels in 12.20 fixed |
| [6..9] | per-channel max step (B, G, R, A) |
| [10] | parsed target (written by parser / LABEL_13 seeding) |
| [11] | active target (smoother chases this) |
| [12] | additive overlay (lightning flash) |

`interpolate_weather_color @ 0x57d9e0` (16 calls per tick): per channel
`step = ((target<<20) - current) >> 3` clamped to ±max-step, accumulate in 12.20, repack with
`+0x80000` rounding; then `[0] = ([0] + additive) × ModulatorBlock × rainFactor` where
`rainFactor = max(0, 0x8000 - Env_RainIntensity)`. Modulator chain: every block multiplies by
the modulator block; the modulator multiplies by modulator2; modulator2 by constant `0x404040`
(identity in 6.6). The modulator's target is the player's terrain-light sample
(`compute_ambient_light_along_direction @ 0x5c7a00`, replicated to gray via ×0x10101 in
`Environment_ApplyFogAndAmbient @ 0x57e533`) — that is the indoor/under-cover dimming.

`Environment_UpdateWeatherTick @ 0x57e9b0`, per 62 Hz tick:

- `ComputeTimeOfDayColors(curtime + advance)` (TOD-keyframed blocks snap; only the static
  blocks and scalars actually smooth).
- 310-tick "TOD minute" cadence counter.
- Wind PRNG: `r = rol32(r, 9); if (r < 0) r += 0x1ABB09`; `rand = r & 0xFFF`. Wave amplitude
  `(windScale * (15*prev + rand²>>8)) >> 12` into a 256-entry ring (`0xFFFF - 2*amp`,
  clamped ≥ 0) plus a sprung oscillator ring (1/32 + 63/64 damping toward 0x8000).
- Earthquake jitter (entity/projectile position noise + camera shake), rain fade
  (`intensity -= rate`, clamp 0).
- **Two lightning sequencers** (`Environment_SetLightningFlash @ 0x57d320` scales
  `lightning_rgb` into the additive slots — sky >>8, fog/skyfog >>9, ground >>10):
  timer A at ticks 10/6/4/2 → flash 200/255/200/255, at 0 → flash 0 + thunder sound;
  timer B at 31/28/26/24/23/22/20 → 200/150/200/150/100/50/0, at 0 → second thunder.
- Scalar smoothers: spring-dampers `(d + 31) >> 5` with accel and absolute clamps (fog
  distance `Env_FogDistCurrent ← Env_FogDistTarget`, overcast blend, two more) and
  eighth-snaps `(d + 7) >> 3` with overshoot snap (sky height, cloud scroll rate, one more).
- Derived render colors: `Env_TerrainLightCombined = light*0xB5/256 + sky` (0xB5 ≈ 0.707!),
  a `0x5A` (0.35) variant, `Env_CeilingFloorBlend = ceiling*0.707 + floor*0.707`,
  `Env_WaterColorLit = water * combined >> 7` (×2 gain), and **fog + skyfog render colors are
  doubled with saturation** — `.env` fog colors are authored at half intensity.
- Horizon blend: when fog distance < half of `Env_FogDistReference` (default 1024), the clear
  color cross-fades from skyfog toward fog color.
- Cloud scroll: four accumulators advance by the smoothed rate × (1, 1, 2/3, 4/3).

`Environment_SnapStateToTargets @ 0x57d1e0` (mission start) copies every parsed target [10]
into the active target [11], snaps fog distance/sky height/scroll rate/overcast targets from
the parsed values, zeroes quake/lightning/rain, and seeds the weather PRNG with a constant —
weather is deterministic per mission start.

## Fog policy (`Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960`)

Driven from `Environment_ApplyFogAndAmbient @ 0x57e440`. `Render_SetFogState` passes
`useLinear = (fog_type != 0)`; the device layer then selects the model:

| fog_type | behavior |
|---|---|
| 0 | exponential fog, density = ln(64) / end (vertex-fog variant ln(64)/(3·end) by device caps) |
| 1 | linear, start = 0.5 world units (caller constant — effectively zero) |
| 2 | linear, start = (1 − density) × end × 0.5 |
| 3 | linear, start = (1 − density) × end × 0.25 |

`start == end` disables fog entirely. The jodemo-era claims in `libs/terrain` (exp ln64 for
type 0, 0.5/0.25 linear starts) are behaviorally correct; only their address citations were
wrong for the retail image.

`density = Env_OvercastBlend / 65536`. End distance (`Environment_GetFogEndDistance
@ 0x57e3e0`): above water `smoothedFogDist × (1 − overcast/2)`; **underwater the end distance
comes from water murk**: `(1 − 0.992·(1 − (1−m)(2−m)/2)) × 200` units (m = 0.8 → ≈ 25 u), and
the fog color switches to the lit water color. A "white fog" pass uses constant `0x808080`.
The applied fog distance also feeds the far clip/culling and the sky dome fog factor.

## Sky dome (`render_skybox @ 0x579080`)

Dome = 441 vertices / 800 triangles (21×21), drawn twice (cloud layer 1, then layer 2) in the
shader path. Cloud UVs: layer 1 `(camera + scrollAcc1x) / 2^28`; layer 2
`(camera + scrollAcc[4/3 for U, 2/3 for V]) / 2^29` (half UV scale = clouds 2× larger, with
anisotropic shear). VS constants: c0-3 UV scroll matrix, c4-7 world, c8 sky highlight float
mirror, c9 `[fogDist×0.9/65536, 0, 1, 0]`, c10 `[0, 0.5, 1, 0.25]`, c11 skybase, c12
(skybright − skybase), c13/c14 sun (pass 1) or moon (pass 2) world position = camera +
direction × 2000 (normalized variants), c15 skyhighlight, c16-23 the two UV matrices, c24
cloudbase, c25 (cloudhighlight − cloudbase), c26 cloudedge, c27 cloudhighlight.
**`advanced_clouds 0` renders a single fixed-function pass with the dome material set to the
cloud block color** (not fog). A white-sky variant forces colors to 1.0/0.9.
`Environment_ApplyFogAndAmbient` pushes the smoothed sky height to the dome
(`@ 0x57e4f7`) only when it changes.

## Celestial bodies (`EffectWorld_LoadCelestialModels @ 0x5adc50`)

Lazy-loaded by name into handles: sun (`Celestial_SunModel @ 0x27e5648`), moon (`0x27e5644`),
glare (`0x27e5640`), star (`0x27e563c`), plus hardcoded `upl.3di` (`0x27e5638`). Glare, star,
and upl load under render mode `0x300000` (additive); sun/moon load plain.
`render_skybox_layers @ 0x5ac230` submits the glare/sun models at camera + offsets (+64/+16
units, alpha `0x2000`, submit flag `0x110`); `render_skybox_sun_glow @ 0x5acd00` drives the
glare with 8 jittered terrain raycasts feeding a ±16/frame brightness hysteresis
(target = 32 × visible rays) and `compute_sun_glare_and_fog_blend @ 0x5ad610` computes
`dot(view,sun)^32 → glare (×192, clamp 255)` and `dot^128 → fog whitening (×40, clamp 40)`.

## BMS per-mission overrides

Applied at mission load, from the in-memory BMS header:

| Field (memory) | Gate | Effect |
|---|---|---|
| water height s16 `Bms_WaterHeightOverride @ 0xa76268` | attrib bit 0x1 | `(v << 15)` → `Terrain_Init` water height (else 0) |
| fog level dword `Bms_FogLevelOverride @ 0xa7626c` | attrib bit 0x2 | `Env_FogLevelFixed` (@ 0x525383) |
| fog color packed `Bms_FogColorOverride @ 0xa76270` | attrib bit 0x4 | fog block parsed target (@ 0x525393) |
| water color bytes `Bms_WaterColorOverride @ 0xa762c6..c8` | any byte nonzero | `Environment_SetWaterColor @ 0x57d510` |
| murk byte `Bms_WaterMurkOverride @ 0xa762c9` | nonzero | × 0.01 → `Environment_SetWaterMurk @ 0x57d4f0` |
| start TOD s16 (8.8 h) `Bms_StartTimeOfDay @ 0xa76418` | local play | `<< 16` → `Environment_SetCurrentTime @ 0x57c4b0` |
| wave amp `Bms_WaveAmplitude @ 0xa7641a` | local play | `Terrain_GenerateWaterNoiseTextures @ 0x57d170` |
| attrib bit 0x100000 | — | water-related `Terrain_Init` flag (unidentified) |

In a network session the server-synced time + TOD rate replace the local start TOD
(`Environment_SetTodRate @ 0x57c4f0`); fog/water/env state is also carried by
`NetPacket_WriteWorldSyncState @ 0x42d6e0` and validated by the server's env CRC check
(`server_handle_client_crc_validation @ 0x519110`). The terrain and environment names feed
`Terrain_LoadEnvironmentConfig @ 0x610940` → `Environment_LoadTimeOfDayConfig`.

## iris_percent / iris_center and terrain_rgb (documented, deferred)

- The only iris consumer is **entity/sector lighting** (`terrain_sector_compute_lighting`,
  reads @ 0x5c7954 / 0x5c79a6): a view-distance response curve — clamp against
  `iris_center × k`, divide, scale by `iris_percent`, `(100 − x) × 0.01` shaping (constants
  100.0 @ 0x7c4654, 0.01 @ 0x7c56a8, 0.707 @ 0x7d4b2c). It is **not** a sun-glare/exposure
  effect. Implementation deferred; exact curve left as an open question.
- `terrain_rgb` packs raw into `Env_TerrainColorPacked @ 0x26c67f4` and a reciprocal
  `32640/component` (clamp 255, 128-if-zero) into `Env_TerrainColorRecip @ 0x26c67f8`; the
  witnessed reciprocal consumer is the **effects system** (`EffectWorld_TickInstancesAndLightScale
  @ 0x5aa170`: float scale = recip × 1/128 per channel) — effect brightness compensation
  against the terrain tint. Entity lighting outdoors uses the light/sky/ground blocks with a
  3-raycast sun occlusion (ambient level 8→5); indoors directional = 0, ambient = ceiling
  block, ground = floor block.

## Divergences (reimpl vs original)

| # | Divergence | Disposition |
|---|---|---|
| 1 | Interpolation parameter space was HHMM in reimpl; original uses 16.16 hours | **Fixed** in `env::interpolate_tod` (exact integer conversion + truncating fraction) |
| 2 | Reimpl float lerp vs original byte quantize + integer lerp (+0x8000 round, 63356 snap quirk) | **Fixed**: integer-faithful lerp incl. quirk |
| 3 | Reimpl unlimited keyframes vs 16-slot cap with 17th-block color bleed into slot 16 | **Fixed**: cap + overflow-bleed replicated on parse |
| 4 | `std::sort` vs stable bubble sort (duplicate-time order) | **Fixed**: `std::stable_sort` |
| 5 | Defaults diverged (sky maps, star, iris, fog type/level, sky speed, advanced_clouds, water color, curtime) | **Fixed**: `Config` defaults = retail; authoring template sets its values explicitly |
| 6 | `water_murk` lower clamp 0.0 (original has none, only ≤ 0.99) | **Fixed**: upper clamp only |
| 7 | Numeric keywords parsed with `atof` vs original `atol` truncation; curtime/tod time digit clamping (h≤23, m≤59 positional) | **Fixed**: integer parse + positional sanitize |
| 8 | envscale applied at interpolation to keyframes only; original bakes at parse into every `*_rgb` (positional, leaks across files in a load) | **Tracked decision**: Config stores raw file values for authoring round-trip; engine view applies envscale at quantization (equivalent when envscale precedes all colors, which the corpus sweep validates). Global-color scaling wired in `env_render`/EnvFile getters |
| 9 | fog→skyfog mirror via per-keyframe flag vs `0xC0C0FF` value sentinel over leftover slots | **Tracked decision**: flag semantics (equivalent for well-formed files; leftover bleed not replicated) |
| 10 | `sky_map*` `.pcx` coercion not applied in reimpl parse | **Fixed** at texture-resolution layer (names stored verbatim for round-trip) |
| 11 | Negative color components: original packs garbage (no lower clamp) | **Tracked decision**: clamp to 0 (no UB replication) |
| 12 | Default sky_height raw-200 quirk (≈0.003 units) | Documented; reimpl default mirrors the quirk via comment, authoring template sets 175 |
| 13 | `vertex_rgb` parsed by reimpl, ignored by retail JO | Keep parsing for round-trip; engine view ignores (modulator identity) |
| 14 | Sun glare occlusion 8 jittered rays | Ship 1 ray + identical hysteresis (Phase 3, tracked) |
| 15 | Thunder sounds on lightning timer epochs | Deferred (signal stub) |
| 16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade | Documented; runtime port carries .env table only until weather/WAC work lands (overcast blend defaults 0 = pure .env, matching clear weather) |
| 17 | Iris view-distance lighting curve | Deferred, consumer documented |
| 18 | Earthquake / rain / wind oscillator rings | Weather-system scope; ported constants documented, wiring deferred with WAC weather |

## Corpus sweep (retail JO:CA install, 2026-06-09)

`tests/env/jo_env_sweep_test.cpp` (gated on `OPENNOVA_JO_DIR`): 10 `.env` files, all parse
and survive save→reparse semantically. Distribution facts backing the divergence
dispositions: 0 files set `envscale` after a color line (#8 holds), 0 tod blocks author
`fog_rgb` without `skyfog_rgb` (#9 moot in practice), 0 files exceed the 16-keyframe cap,
0 author `water_height` (it comes from `.trn`/BMS), fog types used are only 2 (4 files) and
3 (6 files), and all 10 set `advanced_clouds 1`.

## Open questions

- Exact iris response curve (x87 tail of `terrain_sector_compute_lighting @ 0x5c7930..0x5c79f4`).
- `Terrain_Init` attrib-bit-0x100000 water flag semantics.
- Day/night `timeofday` enum mapping order (dawn/day/dusk/night → 1/2/3/4-or-0) — classification
  only, no gradient impact.
- Whether `overcast.def` also loads after a successful `.trn` TOD parse or only as fallback
  (decompiler control-flow ambiguity at @ 0x57dbeb); stock JO terrains appear to carry no TOD
  blocks in `.trn`, making overcast.def the de facto first table.
- `dword_26C6450` "TOD minutes elapsed" consumer.

## Verdicts

| System | Verdict |
|---|---|
| `.env` parse + defaults | **matching** (after fixes; divergences 8/9/11/13 tracked by design) |
| TOD keyframe interpolation | **matching** (integer-faithful, hours space) |
| Sun/moon direction math | **matching** (float-vs-fixed quantization noted, sub-1e-4) |
| Fog policy | **matching** (env_render port; overcast coupling included) |
| Weather tick / smoothing / lightning | **divergent → ported constants** (16-channel model + integer smoothing in env_render; thunder + quake + rain wiring deferred, each tracked) |
| Sky dome render | **divergent → aligned** (scroll/VS map/advanced_clouds=0 fixes in nova_sky; dome mesh constants verified against `build_sky_dome_mesh @ 0x578db0` recon) |
| Celestial + glare | **new implementation** from witnessed model (Phase 3; 1-ray occlusion tracked) |
| BMS overrides | **matching** application semantics (Phase 3 wiring) |
| iris / terrain_rgb | **unknown → documented**, consumers identified, implementation deferred |
