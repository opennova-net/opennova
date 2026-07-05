# .env environments, time-of-day, and the atmosphere stack

How Joint Operations loads `.env` environment files, drives its time-of-day pipeline, and
renders fog, sky, weather, and celestial bodies, as witnessed in the original engine. This is
the reverse-engineering record behind `libs/env` (format + TOD math), the `EnvFile` /
`NovaEnvKeyframe` wrappers, and the `NovaEnvironment` / `NovaSky` / `NovaWater` / `NovaWeather`
/ `NovaCelestial` runtime.

Reverse-engineered from `Jointops.exe` (Joint Operations: Combined Arms, imagebase `0x400000`,
IDB `Jointops.exe.kong.i64`), grilled 2026-06-09; consumer/combine grill (C6) 2026-06-11
closed targets G1–G6 of [env-honored-matrix.md](env-honored-matrix.md) — see the C6 appendix
for the per-target session record. All addresses are absolute in that image.
This record supersedes the jodemo-era citations (`0x53E030`/`0x53E3F0`/`0x53FA70`/`0x53FCC0`/
`0x53F5D0`) and the never-written `engine_spec_env.md`; those addresses do not exist in the
retail image.

Which of these fields the reimplementation's renderer actually consumes today — and which
remain parsed-but-deferred — is tracked per field in
[env-honored-matrix.md](env-honored-matrix.md).

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

Two parse passes share the parser and the 16 slots. **`overcast.def` is not a fallback**
(C6 grill, hand-read at @ 0x57dbca/0x57dbf5; `File_ParseASCIIFile @ 0x53d810` returns 0 on
success, which is what made the decompiler's nesting look ambiguous):

1. `<mapName>` + extension `trn` (inline ext string @ 0x7d7950) **must exist and parse, or
   the function returns early** — no overcast.def, no `.env` pass, no snapshot updates at
   all (@ 0x57dbcc..0x57dbde). On `.trn` success, `overcast.def` **always** parses next
   when present (inline name @ 0x7d7940), **appending into the same 16 slots** —
   `Env_TodParseCount` is *not* reset between the two parses — so its `tod_begin` blocks
   continue after the `.trn`'s. Stock JO `.trn` files carry no TOD blocks, which is why
   overcast.def is the de facto first table. A NULL mapName jumps straight to the
   overcast.def parse (@ 0x57db98 → 0x57dbf7). The combined set snapshots into
   `Env_TrnSnapshotTable @ 0x26c7414` (stable bubble sort by time, count at +832;
   `Environment_SortAndSnapshotKeyframes @ 0x57c240`).
2. slot pointer + keyframe count reset, then `<env>.env` → on parse success (or
   missing/empty env name) the scratch keyframe's colors seed the 12 color blocks'
   parsed targets (@ 0x57dce0 — this is how "naked" color lines outside `tod_begin`
   become static targets) and the table snapshots into `Env_EnvSnapshotTable @ 0x26c70d0`.
   An `.env` parse *failure* skips the seeding and snapshot — the previous env table
   persists stale.

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
(identity in 6.6). The modulator's target is the player's **iris auto-exposure sample**
(`compute_ambient_light_along_direction @ 0x5c7a00`, replicated to gray via ×0x10101 in
`Environment_ApplyFogAndAmbient @ 0x57e533`, chased over 62 ticks via
`ColorBlock_SetStepDeltas @ 0x57d940`) — indoor/under-cover dimming and night-brightening
are the same mechanism; the full curve is in §Iris auto-exposure below.

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
  timer A at ticks 10/6/4/2 → flash 200/255/200/255, at 0 → flash 0 + thunder; timer B at
  31/28/26/24/23/22/20 → 200/150/200/150/100/50/0, at 0 → second thunder. **Thunder wiring
  (C6)**: epoch 0 calls `sub_527B90 → SoundBank_PlayTriggerEntries @ 0x75ccd0` on the bank
  at `dword_24E0914` — sequencer A fires trigger id **0** (param 0x10000, @ 0x57ecfb),
  sequencer B trigger id **0x80** (param 0xA0000, @ 0x57edc4), gated on
  `g_napi_np_ctx.is_mp_session_peer`. **Starters**: the net text command **`SETFLASH1 [n]`**
  (`NapiNPClientMsg_HandleTextCommand @ 0x429ec9`: timer A = atol(arg), default 16; the
  host applies locally and sprintf-broadcasts `"SETFLASH1 16"` @ 0x4d2b6a) — this is the
  retail trigger path. Two orphaned debug hooks exist (`Env_TriggerLightningFlashA
  @ 0x4ed500` = 16, `Env_TriggerLightningFlashB @ 0x4ed510` = 32, zero callers); **no
  `SETFLASH2` exists, so sequencer B is unreachable in retail play**. Rain/wind carry no
  `.env` keywords (parser keyword set enumerated in the 2026-06-09 grill) — weather
  intensity is command/WAC-driven, not `.env`-tunable.
- Scalar smoothers: spring-dampers `(d + 31) >> 5` with accel and absolute clamps (fog
  distance `Env_FogDistCurrent ← Env_FogDistTarget`, overcast blend, two more) and
  eighth-snaps `(d + 7) >> 3` with overshoot snap (sky height, cloud scroll rate, one more).
- Derived render colors: `Env_TerrainLightCombined = light*0xB5/256 + sky` (0xB5 ≈ 0.707!),
  a `0x5A` (0.35) variant, `Env_CeilingFloorBlend = ceiling*0.707 + floor*0.707`,
  `Env_WaterColorLit = water * combined >> 7` (×2 gain), and **fog + skyfog render colors are
  doubled with saturation** — `.env` fog colors are authored at half intensity.
- **Horizon blend (witnessed 2026-07-05, closes the previously uncited one-liner):** after
  the 16 channel smoothers and **before** the fog/skyfog doubling, when the smoothed
  `Env_FogDistCurrent @ 0x26c681c` is (unsigned) strictly below `Env_FogDistReference/2`
  the skyfog render color is overwritten **in place** with `fog*(1-t) + skyfog*t`,
  `t = ((dist - ref/4) << 16)/(ref/4)` clamped to 0 below `ref/4` — pure fog at <= ref/4,
  a linear fade across [ref/4, ref/2], untouched skyfog above (MMX: bytes x0x101 >> 1,
  pmulhw against t/~t >> 1, paddsw, >> 6, packuswb)
  `[orig: Environment_UpdateWeatherTick @ 0x57e9b0, blend @ 0x57f037-0x57f0a1]`; the
  doubling then applies to the post-blend value `[@ 0x57f1b1]`. `Env_FogDistReference
  @ 0x26c68a8` (16.16): default 1024.0 `[orig: Environment_InitDefaults @ 0x57c0b0]`,
  re-set at terrain init to 768.0/1024.0 by adapter-caps bit 0x40 and forced to 1024.0
  on the session authority `[orig: Terrain_Init @ 0x60fc9a/0x60fca3]`. The witnessed
  consumer is the **frame clear**: every scene entry clears with `is_alternate_fog ?
  0x808080 : camera above water ? skyfog[0] : Env_WaterColorLit` via CD3DDevice
  SetClearColor/SetClearDepth(1-2^-15) -> `IDirect3DDevice9::Clear(TARGET|ZBUFFER)`
  `[orig: Render_ProcessMainSceneFrame @ 0x5ca776-0x5ca7bf; terrain_scene_render
  @ 0x5d065a-0x5d0699; device Clear @ 0x677100 (IDB-misnamed CGfxTextOverlay_Draw)]`,
  the Clear halving the color `(c>>1)&0x7F7F7F7F` on non-modulate2x devices
  `[@ 0x67715d; cap dword_32656AC, cf. decode_blend_state_extended @ 0x681080]`; the
  sky-dome pass additionally sets the device FOG color to skyfog while drawing the dome
  and restores fog for the world `[orig: sub_579CB0 @ 0x579cb0]`. Reimpl: blend in
  undoubled space, feed the smoothed (pre-overcast) fog distance, expose via
  `NovaEnvironment.get_frame_clear_color()` (undoubled = the non-modulate2x path a 1x
  host is), consume in the GameWorld clear (`ClearColor` WorldEnvironment); the dome-fog
  consumer rides the sky-dome fog work.
- Cloud scroll: four accumulators advance by the smoothed rate × (1, 1, 2/3, 4/3).

`Environment_SnapStateToTargets @ 0x57d1e0` (mission start) copies every parsed target [10]
into the active target [11], snaps fog distance/sky height/scroll rate/overcast targets from
the parsed values, zeroes quake/lightning/rain, and seeds the weather PRNG with a constant —
weather is deterministic per mission start.

## Environment_ApplyFogAndAmbient walk (C6)

`Environment_ApplyFogAndAmbient @ 0x57e440` is the per-**pass** environment push, called from
every scene renderer (`render_main_scene @ 0x5c164c`, `terrain_scene_render @ 0x5d07f6`,
`Render_ProcessMainSceneFrame @ 0x5ca3ce/0x5ca841`, `Terrain_RenderSceneWithReflection
@ 0x5c949f`, `Render_RadarCompassOverlay @ 0x5c98b6`, `render_cinematic_multiview @ 0x570adb`)
with `(is_underwater, is_alternate_fog)`; `is_underwater = g_view_pos_z <
Env_WaterHeightFixed` at most sites. Complete ordered walk:

| # | Site | Action |
|---|---|---|
| 1 | @ 0x57e44c | `end = Environment_GetFogEndDistance(is_underwater)` (murk-derived when underwater) |
| 2 | @ 0x57e458 | `Render_UnpackModulatorToLightScale(modulator[0])` → `Render_LightScaleRGB @ 0x8409f4..fc` = modulator color ÷ 64 — **the application point of the iris auto-exposure**: these floats are a shader constant (consumed in `apply_shader_parameters @ 0x58e05d`); 64 = 1.0 gain |
| 3 | @ 0x57e464 | `EffectWorld_UnpackModulatorToAmbientScale(modulator[0])` → `EffectWorld_AmbientScaleRGB @ 0x840b24..2c` (foliage + effects renderers) |
| 4 | @ 0x57e471–0x57e4ad | device fog color ← underwater ? `Env_WaterColorLit` : `is_alternate_fog` ? `0x808080` : fog block `[0]`; fog type ← underwater ? 1 : `Env_FogType` |
| 5 | @ 0x57e4c3–0x57e4db | `Render_SetFogState(0.5, end/65536, type, Env_OvercastBlend/65536)` |
| 6 | @ 0x57e4ee | `Env_FogEndApplied = end` |
| 7 | @ 0x57e4f4–0x57e505 | smoothed sky height pushed on change → `Terrain_PushSkyDomeHeightFloat @ 0x610920` → `sub_579070` (dome) |
| 8 | @ 0x57e512–0x57e538 | if local player && `dword_C6EAFC`: `modulator.target[11] = 0x10101 × compute_ambient_light_along_direction(player)`, then `ColorBlock_SetStepDeltas(modulator, 62)` — exposure reaches the new target in 62 ticks (1 s) |

**Ceiling/floor application points** (the G4 question): (a) the **indoor branch of the
exposure sample** — `terrain_sector_compute_lighting @ 0x5c7646..0x5c76fe` substitutes
ceiling block `[1]` (@ 0x26c6474) for sky and floor block `[1]` (@ 0x26c64dc) for ground,
with directional zeroed, when the position is under cover; and (b) the derived
`Env_CeilingFloorBlend @ 0x26c67f0` (= 0.707·ceiling + 0.707·floor, weather tick
@ 0x57f110) is stored by `render_emitter_effect @ 0x5f7163` into the effect world at
+0x3EC **beside** the outdoor `Env_TerrainLightCombined` at +0x3E8 — the indoor vs outdoor
ambient pair for particles/effects.

## Iris auto-exposure (C6 — the iris_percent / iris_center consumer)

`terrain_sector_compute_lighting @ 0x5c7550` *returns* the iris gain (int 0..255; the
x87 tail was invisible until the IDB's void return type was fixed). Inputs: directional =
light block `[1]` × visibleRays/8 (3 sun-occlusion raycasts, level 8..5; zero indoors),
sky = sky block `[1]` (ceiling indoors), ground = ground block `[1]` (floor indoors), each
÷255; light direction from `Environment_GetLightDirectionFloat`. Exact curve (constants
@ 0x7c333c = 0.25, @ 0x7c3b94 = 0.5, @ 0x7c3dd0 = 64.0, @ 0x7c4654 = 100.0,
@ 0x7c56a8 = 0.01, @ 0x7d4b2c = 0.707, @ 0x7ca29c = 255.0):

```
lum(C)   = 0.25·(C.r + C.b) + 0.5·C.g
dirLum   = lum(directional);  skyLum = lum(sky);  gndLum = lum(ground)
vertLum  = dirY·dirLum + skyLum
horizLum = sqrt(dirX² + dirZ²)·dirLum + 0.707·(skyLum + gndLum)
m        = max(dirLum, skyLum, gndLum, vertLum, horizLum)
base     = iris_center × 64.0
gain     = 0.01 · ( iris_percent · base/(2m) + (100 − iris_percent) · base )
return clamp((int)gain, 0, 255)        // 64 = identity (6.6 fixed, modulator units)
```

At defaults (50 / 1.25): `gain = 40 + 20/m` — ≈60 in bright sun (m≈1), rising toward the
255 clamp in darkness. `compute_ambient_light_along_direction @ 0x5c7a00` averages the
gain at **3 points marched from the camera-ray hit back toward the camera**
(`(s0+s1+s2)/3`), and that average becomes the modulator target (walk row 8 above) — so
iris is the engine's **global auto-exposure**, scaling *every* color block (sky, fog,
light, clouds…) through the modulator chain, not a niche terrain curve.

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

## Sky dome (`render_skybox @ 0x579080`) — full combine recovered (C6)

Dome = 441 vertices / 800 triangles (21×21 grid, FVF `0x212` = XYZ|NORMAL|TEX2, stride 40,
`build_sky_dome_mesh @ 0x578db0`). C7 pre-port reads pinned the builder's remaining
unknowns: the height scale `v14 = skyHeight/175.69` stretches **Y only** (x/z stay at the
1024-unit rim radius, @ 0x578ed4), and the **dome normal is the builder's anisotropic
normal `normalize(x, y_scaled/v14², z)`** — equivalently `normalize(x, y_unscaled/v14, z)`
— *not* the normalized vertex direction (@ 0x578fbb..0x579023). The dome's world
translation is the camera position with **height halved** (`camHeight >> 1`,
@ 0x5790b2..0x5790d7) — the dome rides up at half the camera's vertical rate. The shader
path draws the dome **twice — but the two passes are NOT one-per-cloud-layer** (pre-C6
misreading): pass 1 is a **textureless sky gradient**, pass 2 draws **both cloud layers in
one multi-stage pass**, alpha-blended over it. Both vertex shaders are embedded as `vs_1_1`
*source text* (`SkyVS_GradientPassSource @ 0x7d7338`, `SkyVS_CloudPassSource @ 0x7d6fc0`)
and assembled at runtime with the statically-linked `D3DXAssembleShader` in
`terrain_init_rendering_resources @ 0x5789e0` (handles → sky+116 / sky+120).

**VS constants** (uploads @ 0x579709..0x579868, both passes): c0-3 WVP, c4-7 world,
**c8 = eye world position** (fog reference — the pre-C6 "sky highlight float mirror" label
was wrong; @ 0x27219f0), c9 `[fogDist×0.9/65536, 0, 1, 0]`, c10 `[0, 0.5, 1, 0.25]`,
c11 skybase, c12 (skybright − skybase), c13 = normalized sun direction (pass 1) / active
light direction, moon at night (pass 2), c14 = the same point at camera + dir×2000 pushed
through the view-proj transform and normalized (clip-space proximity reference),
c15 skyhighlight, c16-19 / c20-23 the two UV scroll matrices (layer 1 `(camera +
scrollAcc1x)/2^28`; layer 2 `(camera + scrollAcc[4/3 U, 2/3 V])/2^29`), c24 cloudbase,
c25 (cloudhighlight − cloudbase) — **uploaded but read by neither shader (dead)**,
c26 cloudedge, c27 cloudhighlight.

**Pass 1 — sky gradient** (VS `SkyVS_GradientPassSource`, raw UVs, no texture — its effect
is created with `CEffect_SetTextureParam(NULL)`):

```
t      = 0.5·dot(domeNormal_world, sunDir[c13]) + 0.5      // THE gradient driver
sky    = c11 + c12·t                                       // skybase → skybright toward the sun side
prox   = max(dp3(normalize(clipPos.xyz), c14.xyz), 0)      // 3-component; w excluded on both sides
oD0    = lerp(sky, skyhighlight[c15], prox⁸)               // sun-proximity highlight
oFog   = 1 − dist(worldPos, eye[c8]) / (0.9·fogDist)
```

(C7 precision pin: the source text normalizes `clipPos.xyz` with `dp3/rsq/mul` and the
proximity dot is itself a `dp3` — both strictly 3-component, w never participates.)

**Pass 2 — clouds** (VS `SkyVS_CloudPassSource`, both scrolled UV sets; per-vertex):

```
sky    = c11 + c12·t                                       // same gradient
oD1    = lerp(sky, cloudedge[c26], prox⁴)                  // "sky behind the clouds"
ramp   = lerp(cloudbase[c24], cloudedge[c26], prox⁴)
oD0    = lerp(ramp, cloudhighlight[c27], prox⁸)            // the cloud color ramp
```

Pixel stage = fixed-function TSS (no pixel shader is ever bound). Stage table decoded
against `RenderState_ApplyToDevice @ 0x681920` (D3DTOP/D3DTA literals); blend =
srcAlpha/invSrcAlpha, capability-gated at effect build (@ 0x5789e0):

- *Capable hw* (ps ≥ 3 or DOT3+MULTIPLYADD caps), 3 stages:
  `s0 color = tex0, alpha = tex0.a` → `s1 color = tex1·current·2, alpha = tex1.a·current.a·2`
  → `s2 color = LERP(current, oD0_cloudRamp, oD1_skyBehind), alpha = current.a²·2`.
  Per fragment: `rgb = density·cloudRamp + (1−density)·skyBehind` with
  `density = tex0·tex1·2`, blended onto pass 1 by `α = (tex0.a·tex1.a·2)²·2`.
- *Basic hw*, 2 stages: `s0 color = oD0 (flat cloud ramp), alpha = tex0.a` →
  `s1 color = current, alpha = tex1.a·current.a` — texture *colors* unused, alphas drive
  the blend.

**`advanced_clouds 0`** renders a single fixed-function pass with the cloud block color in
the dome **material's ambient** slot against `SetRenderState(D3DRS_AMBIENT, 0xFFFFFF)`
(@ 0x579b42..0x579bb6) — and that is the **only** render path that reads `cloud_rgb`
(`Env_CloudBlock @ 0x26c64a4` xref sweep: render path reads exist solely in that branch;
the keyframed path's cloud colors come exclusively from the cloudbase/cloudedge/
cloudhighlight blocks). C7 correction: the flat path applies the **pass-1 effect** —
created with `CEffect_SetTextureParam(NULL)` — so it is **textureless** (the earlier
"textures still bound" reimpl comment was wrong). A white-sky variant forces sky colors
to 1.0 and cloud colors to 0.9. `Environment_ApplyFogAndAmbient` pushes the smoothed sky
height to the dome (`@ 0x57e4f7`) only when it changes.

**Dome fog color (C7 close-out).** There is no dome-specific fog color derivation: both
dome draws run with VS-fog modes (`CD3DDevice_SetFogAndBlendMode(.., 8)` zeroes
FOGTABLEMODE/FOGVERTEXMODE so the VS `oFog` output drives the fixed fog blend) against the
shared scene `D3DRS_FOGCOLOR`, whose value is the **active fog block dword verbatim** —
`CD3DDevice_SetActiveFogColor @ 0x677040` stores `Env_FogBlock` (above water) /
`Env_WaterColorLit` (underwater) / literal `0x808080` (white-fog pass), and
`CD3DDevice_SetFogAndBlendMode @ 0x677740` pushes it untransformed. The dome therefore
fogs with exactly the same color as terrain.

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

## iris_percent / iris_center and terrain_rgb — consumers pinned (C6)

- **Iris**: the pre-C6 reading ("a view-distance response curve, not an exposure effect")
  was **wrong on both counts** — the recovered curve (§Iris auto-exposure above) is the
  engine's global auto-exposure, fed by scene luminance, applied through the modulator
  chain to every color block and to the `Render_LightScaleRGB` shader constant.
  Implementation still deferred (reimpl has no modulator chain yet); the curve is now
  spec-complete with unit-testable constants.
- **`terrain_rgb` is NOT terrain-inert** (pre-C6 hypothesis refuted by the @ 0x26c67f4
  xref sweep). The packed color is pushed at terrain init — *after* the env parse, so the
  file's value is live (`Game_LoadTerrainDuringConnect` orders `Terrain_LoadEnvironmentConfig
  @ 0x52073b` before `Terrain_Init @ 0x52076f`) — via `PolyTrn_SetTerrainTintColors
  @ 0x605e20` into two renderer globals: full tint `0x31a1824` and half tint `0x31a1828`
  (`(c>>1)&0x7f7f7f`). Witnessed consumers (dispositions re-graded by the 2026-07-05
  #19 grill):
  - `PolyTrn_InitTextures @ 0x60b8cb` (from `Game_StartMission`): writes the 256×256
    quarter-res bake buffer `dword_319F798` per texel (`channel × value >> 12`, 16-tap
    4×4 sum; underwater +32/+32/+48 blue-shift behind a scale-mismatched height test) —
    **DEAD CODE**: the buffer is written and freed but its three readers (`0x606ce0`,
    `0x606c30`, `Terrain_GetColorMapBilinear @ 0x606d80`) have **zero xrefs** (full
    `.text` E8/E9 scan). The GPU terrain textures ship **untinted**; an untinted
    terrain surface is the faithful behavior. (The pre-grill claim that the bake
    reaches the textures was wrong.)
  - `PolyTrn_RenderTile @ 0x60df0d` (every frame via `PolyTrn_RenderFrame @ 0x60eac0` ←
    `render_main_scene` → `render_water_quad @ 0x604700`): **LIVE** — half tint is the
    DIFFUSE on all four vertices of the `.til` tile-overlay quad (tri-strip FVF
    `0x2C4`, SPECULAR `0xFF000000`, composited into the 128-slot tile texture RT with
    `COLORWRITEENABLE=RGB`); combine pass `0x631` runs stage0 TEXTURE × DIFFUSE with
    MODULATE2X when caps `dword_32656AC` (default true) else MODULATE. The default
    tint renders 254/255 per channel — near-identity, one LSB dark, NOT exact.
  - `sample_terrain_lightmap @ 0x606030` (from `generate_foliage_instances_0`
    @ 0x600197..0x6001eb, 4 samples at instance ±0x8000 both axes): **LIVE** — lightmap
    texel × `full/128` saturating (`min((texel×FULL)>>7, 255)`, alpha passthrough;
    128 = identity; the default `0xFFFFFF` ≈ ×2 saturating gain — lightmaps are
    authored ≤128 nominal) — **foliage instance tinting**. The colormap source is
    alpha-premultiplied (`A<<24 | (A*c>>8)`, untinted) and the emitter's main-path
    vertex color is `0xFF000000 | (0x404040 + (avg>>1))` under a 2X draw — those two
    facets ride the foliage render-emitter parity (PAR-R2), not the tint.
  - Vestigial: a runtime getter/setter pair (`Env_GetTerrainColorPacked @ 0x57d4b0`,
    setter @ 0x57d4c0) with **zero callers**, and a `(color & 0xC0C0C0) != 0xC0C0C0` mode
    flag write in `Terrain_Init @ 0x60fc66` whose target (`0x31beae8`) is never read.
- `terrain_rgb`'s reciprocal `32640/component` (clamp 255, 128-if-zero) in
  `Env_TerrainColorRecip @ 0x26c67f8` has exactly one consumer — **confirmed sole** by the
  C6 xref sweep: `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` (float scale =
  recip × 1/128 per channel), effect brightness compensation against the terrain tint.
- Entity lighting outdoors uses the light/sky/ground blocks with a 3-raycast sun occlusion
  (ambient level 8→5); indoors directional = 0, ambient = ceiling block, ground = floor
  block (these are the iris exposure inputs — §Iris auto-exposure).

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
| 14 | Sun glare occlusion 8 jittered rays + ±16/frame hysteresis | **Partial**: `nova_celestial.gd` renders glare_3di additively at the sun with the `dot^32` intensity from `env_render::compute_sun_glare`; terrain-raycast occlusion held at full brightness (tracked) |
| 15 | Thunder sounds on lightning timer epochs | Deferred; **fully specced by C6** (SoundBank trigger 0 / 0x80 on bank `dword_24E0914`, `SETFLASH1` net-command start; sequencer B unreachable in retail) — wiring lands with WAC weather |
| 16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade | Documented; **C6 corrected the precedence**: overcast.def is additive-after-success (and the sole table for NULL map), never a fallback; a missing/failed `.trn` aborts the whole TOD load. Runtime port carries .env table only until weather/WAC work lands (overcast blend defaults 0 = pure .env, matching clear weather) |
| 17 | Iris auto-exposure (modulator gain) | Deferred; curve + consumer chain fully recovered (§Iris auto-exposure). **The curve is now PORTED to `libs/env`** (`iris_gain`/`iris_luminance`, `[orig: terrain_sector_compute_lighting @ 0x5c7550]`, unit-tested in `env_render_unit_test`: m=1→60, darkness→255-clamp, iris_percent-0→base). Row stays WITNESSED-READY-DEFERRED: the reimpl still has no modulator CHAIN to apply the gain to the color blocks — that runtime consumer is the residual |
| 18 | Earthquake / rain / wind oscillator rings | Weather-system scope; ported constants documented, wiring deferred with WAC weather |
| 19 | `terrain_rgb` terrain-stack consumers | **FIXED 2026-07-05** (the tint grill re-shaped it): the FULL/HALF split is ported libs/env-first (`terrain_tint_from_packed`/`_from_rgb` `[orig: PolyTrn_SetTerrainTintColors @ 0x605e20]`, ctest-pinned) and both LIVE consumers render — the `.til` tile overlay (`EnvFile.tile_overlay_tint_factor` → `u_tile_overlay_tint`, the MODULATE2X-over-HALF combine folded to one multiply, 254/255 at default `[orig: PolyTrn_RenderTile @ 0x60df0d]`) and the foliage lightmap sample (`env::foliage_lightmap_tint` per sample in `NovaFoliageDispatcher`, `min((texel×FULL)>>7,255)` `[orig: sample_terrain_lightmap @ 0x606030]`), fed from the loaded env by GameWorld at terrain init like retail. The texture-bake consumer is **DEAD CODE** (readers zero-xref; untinted terrain surface IS faithful — `get_terrain_lighting_attenuation` identity ratified, §C6). Residual facets ride PAR-R2 (colormap alpha-premultiply, emitter half-plus-bias vertex color) and the editor foliage preview keeps the default (=retail default) tint |
| 20 | Sky dome combine | **Fixed by C7**: `sky.gdshader` + `nova_sky.gd` rewritten as a structural port of the recovered two-pass spec (§Sky dome) — gradient/cloud lerp chains, dp3 clip-space proximity, builder-formula dome normals computed in the vertex stage, Y-only height scale, half-camera-height anchor, textureless `advanced_clouds 0` flat pass, VS dome fog against the shared scene fog color. The fabricated keyframed-path `u_cloud_tint` is deleted; the dead c25 upload is not replicated. Residual cosmetic caveat: the clip-space prox dot is computed in Godot's clip conventions (reverse-Z), not D3D's — same construction, slightly different z scale; tracked for visual A/B |
| 21 | skyfog frame clear color | **FIXED 2026-07-05**: the horizon blend is ported libs/env-first (`horizon_blend_skyfog`, byte-exact vs the MMX sequence, ctest-pinned + parity-vector cell) and consumed - `NovaEnvironment.get_frame_clear_color()` drives the GameWorld `ClearColor` WorldEnvironment (above-water skyfog blend / underwater lit-water, the witnessed choice); the vehicle alternate-fog view and the dome-fog application ride their subsystems. Editor preview adoption rides ENV-1. |

## Corpus sweep (retail JO:CA install, 2026-06-09)

`tests/env/jo_env_sweep_test.cpp` (gated on `OPENNOVA_JO_DIR`): 10 `.env` files, all parse
and survive save→reparse semantically. Distribution facts backing the divergence
dispositions: 0 files set `envscale` after a color line (#8 holds), 0 tod blocks author
`fog_rgb` without `skyfog_rgb` (#9 moot in practice), 0 files exceed the 16-keyframe cap,
0 author `water_height` (it comes from `.trn`/BMS), fog types used are only 2 (4 files) and
3 (6 files), and all 10 set `advanced_clouds 1`.

## Open questions

- `Terrain_Init` attrib-bit-0x100000 water flag semantics.
- Day/night `timeofday` enum mapping order (dawn/day/dusk/night → 1/2/3/4-or-0) — classification
  only, no gradient impact.
- `dword_26C6450` "TOD minutes elapsed" consumer.
- Pass-1 sky-gradient effect's exact stage table (`sub_679030` looks the effect up by name;
  with no texture bound the dome flat-shades oD0 — the sensible diffuse passthrough — but
  the named effect's TSS rows were not dumped). Cosmetic-only for C7.
- Closed by C6: iris curve (§Iris auto-exposure), overcast precedence (§Load pipeline),
  terrain_rgb consumers (§iris/terrain_rgb), sky combine (§Sky dome), thunder wiring
  (§weather tick).
- Closed by the C7 pre-port reads: dome mesh normals + Y-only scale (§Sky dome),
  half-camera-height dome anchor, dp3 form of the clip-space proximity, and
  `SetFogAndBlendMode` mode 8 = VS-fog (table/vertex fog modes zeroed so the shader's
  `oFog` drives the blend) with FOGCOLOR = active fog block value (§Sky dome, dome fog
  close-out).

## Verdicts

| System | Verdict |
|---|---|
| `.env` parse + defaults | **matching** (after fixes; divergences 8/9/11/13 tracked by design) |
| TOD keyframe interpolation | **matching** (integer-faithful, hours space) |
| Sun/moon direction math | **matching** (float-vs-fixed quantization noted, sub-1e-4) |
| Fog policy | **matching** (env_render port; overcast coupling included) |
| Weather tick / smoothing / lightning | **divergent → ported constants** (16-channel model + integer smoothing in env_render; thunder + quake + rain wiring deferred, each tracked; thunder spec complete per C6) |
| Sky dome render: scroll / VS constants / mesh / advanced_clouds=0 | **matching** (verified against `build_sky_dome_mesh @ 0x578db0` + the C6 constant map) |
| Sky dome per-fragment combine | **matching** (C7): structural port of the recovered two-pass spec (§Sky dome) folded into one Godot pass; clip-convention prox caveat tracked under divergence #20 |
| Celestial + glare | **new implementation** from witnessed model (`nova_celestial.gd`: sun/moon/star/glare 3DI at the sky, glare additive; occlusion held at full brightness, tracked) |
| BMS overrides | **matching** application semantics via EnvFile's non-persistent override layer (runtime apply on load / clear on unload; base file never mutated) |
| iris / terrain_rgb | iris **divergent** — global auto-exposure (curve spec-complete), no modulator chain yet, divergence #17. terrain_rgb **matching** after the 2026-07-05 tint port (#19 FIXED): both live consumers render (tile overlay HALF×2X, foliage FULL×/128); the bake consumer is dead code, untinted terrain surface ratified faithful |
| Load pipeline overcast precedence | **matching** after C6 correction (additive-after-success; reimpl two-table model documented, overcast blend at 0 pending weather work) |

## What this effort shipped (2026-06-09, branch `environment-workspace`)

- `libs/env`: engine-faithful parse + 16.16-hours TOD interpolation; new
  `env_render` module (fog policy, day phase, integer smoothing, lightning,
  glare, derived colors, BMS overrides) with `env_render_unit_test` and the
  `OPENNOVA_JO_DIR`-gated install sweep.
- `godot/engine`: EnvFile fog/day-phase/glare/override/`to_bytes` surface +
  `NovaColorSmoother`; engine-faithful `nova_environment`/`nova_sky`/
  `nova_water`/`nova_weather`; new `nova_celestial` + two celestial shaders;
  BMS override fields through `NovaMissionData`; runtime apply/clear in
  `game_world`.
- `godot/modtools`: terrain preview unified onto `NovaWater` + `EditorWeather`
  (PR #24 parity); Environment workspace undo/redo, save-path gate, and sky-model
  fields.
- Citations: all jodemo-era addresses and the dangling `engine_spec_env.md`
  reference re-anchored to retail; IDA renamed + commented across the cluster.

---

## Appendix: atmosphere parity audit (2026-06)

Consolidated 2026-06-10 from `notes/audit_atmosphere_citations.md`,
`notes/audit_atmosphere_gaps.md`, and `notes/audit_atmosphere_visual.md`. The audit preceded
the 2026-06-09 grill above and fed it; this appendix preserves the jodemo→Jointops citation
corrections, the addresses still pending verification, and the visual-parity check
dispositions. Addresses marked jodemo are in `jodemo.exe`; everything else is Jointops retail.

### Citation corrections (jodemo addresses mislabeled as Jointops)

Central finding: the jodemo-era `engine_spec_env.md` (a dangling reference, never tracked)
declared "Target binary: Jointops.exe" but cited jodemo addresses (`0x53xxxx` / `0x540B90` /
`0x501FB0` / `0x5D0A90`). Function bodies are identical between the two images, so the C++
port was behaviorally correct throughout — only the citations were wrong. Recovered retail
addresses (verified by body comparison; 18 recites applied across `libs/env`, `libs/terrain`,
and `godot/engine/environment`):

| Old cite (jodemo, mislabeled) | Jointops retail | Note |
|---|---|---|
| `Terrain_SetDefaultEnvironmentValues @ 0x53E030` | `Environment_InitDefaults @ 0x57c010` | |
| `sub_53E3F0` (keyword callback) | `TimeOfDay_ParseProperty @ 0x57c590` | called from the loader body |
| `sub_53FA70` (loader) | `Environment_LoadTimeOfDayConfig @ 0x57db30` | body sequence verified: defaults reset (incl. `0xC0C0FF` seed), `.trn` pass, `.env` pass, snapshot copy |
| `sub_53FCC0` (TOD interp) | `Environment_ComputeTimeOfDayColors @ 0x57de40` | |
| `sub_540B90` (per-frame tick) | `Environment_UpdateWeatherTick @ 0x57e9b0` | |
| `Terrain_CalcSunDirection @ 0x53F5D0` | `Environment_ComputeSunDirection @ 0x57d6d0` | fixed-point core; the float wrapper `Terrain_GetSunDirectionAsFloat @ 0x57d7f0` matches the float reimpl's shape (same core/wrapper pairing for the moon variant) |
| `sub_501FB0` (line parser) | `File_ParseASCIIFile @ 0x53d810` | |
| `Path_ReplaceExtension` (wrong helper name) | `Path_ReplaceOrAppendExtension @ 0x53c780` | |
| `Terrain_SetEnvironmentData @ 0x53F840` | `Terrain_SetEnvironmentData @ 0x53f830` | off-by-16 in the old cite |
| jodemo env globals `0xFF2xxx` (e.g. `dword_FF375C` snapshot, `flt_FF2B2C` iris) | `0x26c6xxx` state-block region | re-anchored per-global during the grill (see §Color state blocks) |

Superseded audit verdict: the audit kept `Render_SetFogParams @ 0x54b4b0` as an exact retail
match (kong confidence was already low — wrong signature). The grill then proved that function
is an entity-pool sweeper (`Entity_DestroyUnreferencedPool4Entries`); the real fog setter is
`Render_SetFogState @ 0x58a950` (§Fog policy).

### Addresses still pending verification

Terrain-side cites (`libs/terrain/lighting.h`) the audit could not confirm in the retail
image and the env grill did not close (terrain-lighting / foliage scope):

| Claimed cite | Status | Nearest known retail anchor |
|---|---|---|
| `Terrain_SetLightingColors @ 0x5C4B10` | VERIFY-pending | no fn at that address; `render_visibility_portal_traversal @ 0x5c4ae0` at −48. Entity/sector lighting was since anchored at `terrain_sector_compute_lighting @ 0x5c7550` (§iris) |
| `Render_ConfigureFog @ 0x5F9890` | VERIFY-pending | unnamed `sub_5F98A0` at +16, body unconfirmed; the device fog path was since anchored at `CD3DDevice_SetFogParameters @ 0x677960` (§Fog policy) |
| `Terrain_GetModulatedColorAtPos @ 0x5C5FE0` | VERIFY-pending | nearest `Entity_BuildProjectileTrailRay @ 0x5c6090` at +176 — likely a different function |
| `Foliage_BuildGeometry @ 0x5BF5F0` | stale / do not cite | address resolves inside `build_shader_pass_name @ 0x5bf5d0`; foliage placement anchors are `terrain_update_foliage_tiles @ 0x601F50`, `generate_foliage_instances @ 0x600980`, and `sub_606620 @ 0x606620` |
| jodemo `sub_5D0A90` (terrain-init caller of the loader) | closed by the grill | `Terrain_LoadEnvironmentConfig @ 0x610940` (§BMS overrides) |

### Gap-walk dispositions

One row per §7 "Known Gaps" / §9 "Port Divergences" entry of the jodemo-era spec; all but one
were closed by the grill sections above:

| Gap | Disposition |
|---|---|
| §7.1 env CRC error strings ("bad sky/fog/water CRC", near `0x7480a0`) | Closed as intentional divergence: retail validates env state server-side (`server_handle_client_crc_validation @ 0x519110`, §BMS overrides); our parser does not gate load on CRC (assets are user-edited, not network-delivered) |
| §7.2 fog D3D state slots | Closed: `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` mapping witnessed (§Fog policy); the jodemo-era curve claims (exp `ln(64)/end` for type 0, 0.5/0.25-scaled linear starts) were behaviorally correct |
| §7.3 snapshot stride (`dword_FF375C`) | Closed: snapshot tables `Env_TrnSnapshotTable @ 0x26c7414` / `Env_EnvSnapshotTable @ 0x26c70d0`, keyframe count at +832 (§Load pipeline) |
| §7.4 advanced-clouds render path | Closed: `advanced_clouds 0` is a single fixed-function pass with the dome material set to the cloud block color (§Sky dome) |
| §7.5 unconsumed globals (`timeofday`, `iris_*`, `lightning/ceiling/floor/cloud_rgb`) | Closed: re-anchored to the `0x26c6xxx` state blocks with consumers identified — iris → entity/sector lighting, ceiling/floor → indoor ambient, cloud → `advanced_clouds 0` dome material, lightning → additive flash slots (§Color state blocks, §iris) |
| §9.1 `.trn`/`.env` pair load order | Closed: two-pass loader documented (§Load pipeline) via `Terrain_LoadEnvironmentConfig @ 0x610940` |
| §9.2 `terrain_rgb` reciprocal consumer | Closed: `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` effect-brightness compensation (§iris / terrain_rgb) |

New finding kept open from the audit: `Environment_UpdateWeatherTick @ 0x57e9b0` has signature
`void(int waterHeight, int isReflection)`. The reimpl weather tick does not differentiate a
reflection pass; whether those parameters drive a reflection-vs-main-pass rendering delta is
unverified. Tracked.

### Visual-parity check dispositions (sky / fog / weather / celestial)

The planned A/B capture run (4 cameras × 4 curtimes via `scripts/ab_diff.py atmosphere`) was
never executed — zero visual deltas were recorded. The five pre-flagged checks were
dispositioned analytically by the grill instead:

| Check | Disposition |
|---|---|
| Reflection-pass sky in water surfaces | **Open** — see the weather-tick `(waterHeight, isReflection)` finding above |
| Fog-type curves (type 0 exponential, types 2/3 linear starts) | Closed numerically: device-layer mapping witnessed (§Fog policy) |
| Sun position within ~1° of retail | Closed: sun/moon direction verdict **matching** (float-vs-fixed quantization sub-1e-4, §Verdicts) |
| Sky horizon gradient (skybase → skybright → skyhighlight) | Closed: VS constant map verified and `nova_sky` aligned (§Sky dome) |
| Cloud scroll rate | Closed: smoothed-rate accumulators × (1, 1, 2/3, 4/3) with `sky_speed << 10` parse scale (§Sky dome, §Format truths) |

---

## Appendix: C6 consumer/combine grill session record (2026-06-11)

Closed all six grill targets from [env-honored-matrix.md](env-honored-matrix.md); no
target ended inconclusive. Doc-only slice: no reimplementation changes (the fix wave is
roadmap slice C7).

### Per-target verdicts

| Target | Question | Verdict | Evidence | C7 action |
|---|---|---|---|---|
| G1 sky combine | how c11/c12/c15/c24-c27 combine per fragment; cloud_rgb scope | **recovered** — embedded vs_1_1 sources + TSS tables decoded; two-pass spec | §Sky dome | rewrite `sky.gdshader` fragment + `nova_sky.gd` uniforms from the spec; delete keyframed-path `u_cloud_tint`; skip dead c25 |
| G2 iris curve | exact x87 tail shaping | **recovered** — closed-form auto-exposure gain, 64 = identity | §Iris auto-exposure | spec ready for a future `env_render::iris_exposure_gain` with pinned constants; needs the modulator chain first (divergence #17) |
| G3 terrain_rgb consumers | any consumer beyond the effects reciprocal? | **refuted "terrain-inert"** — texture bake, water quads, foliage tint, all live | §iris/terrain_rgb | matrix row stays PARTIAL with the bigger gap; port decision per consumer (divergence #19); recip sole-consumer claim now *confirmed* |
| G4 ApplyFogAndAmbient | full state walk; ceiling/floor points; 0x5c7a00 wiring | **complete** — 8-row walk table; exposure application point = `Render_LightScaleRGB` shader constant | §Environment_ApplyFogAndAmbient walk | informs C7 fog/exposure plumbing; ceiling/floor wire-or-delete now decidable (keep: they are live exposure inputs + effect ambient) |
| G5 overcast precedence | fallback or always-after? | **corrected** — never a fallback; additive after `.trn` success (no count reset); missing/failed `.trn` aborts all | §Load pipeline | comment-level in `libs/env`; future overcast cross-fade uses the corrected order |
| G6 thunder/oscillators | epoch→sound mapping; .env tunables | **feeder complete** — trigger ids 0/0x80, bank `dword_24E0914`, `SETFLASH1` net command; sequencer B unreachable; no rain/wind `.env` keywords | §weather tick | consumed by the WAC weather wave, not C7 |

### IDB edits applied (sanctioned-grill policy; verify-before-edit; `idb_save` checkpoints)

| Addr | Old | New | Evidence |
|---|---|---|---|
| 0x605e20 | `sub_605E20` | `PolyTrn_SetTerrainTintColors` | writes both tint globals; three witnessed consumers |
| 0x57d4b0 | `sub_57D4B0` | `Env_GetTerrainColorPacked` | 2-insn getter |
| 0x31a1824 | `dword_31A1824` | `PolyTrn_TerrainTintFull` | consumer sweep |
| 0x31a1828 | `flt_31A1828` | `PolyTrn_TerrainTintHalf` | `(c>>1)&0x7f7f7f` packing (was mistyped float) |
| 0x57d940 | `sub_57D940` | `ColorBlock_SetStepDeltas` | per-channel `|target−current|/frames` body |
| 0x58db30 | `Render_UnpackFogColor` | `Render_UnpackModulatorToLightScale` | input = modulator block value; output consumed by `apply_shader_parameters` (old kong name wrong) |
| 0x5aaef0 | `CEffectWorld_UnpackAmbientLightColor` | `EffectWorld_UnpackModulatorToAmbientScale` | same shape, foliage/effects consumers |
| 0x610920 | `sub_610920` | `Terrain_PushSkyDomeHeightFloat` | 16.16→float → `sub_579070` |
| 0x8409f4-fc | `flt_8409F4..` | `Render_LightScaleR/G/B` | modulator÷64 shader constant |
| 0x840b24-2c | `flt_840B24..` | `EffectWorld_AmbientScaleR/G/B` | effects/foliage gain |
| 0x7d7338 | `aVs11DclPositio` | `SkyVS_GradientPassSource` | embedded vs_1_1 source, pass 1 |
| 0x7d6fc0 | `aVs11DclPositio_0` | `SkyVS_CloudPassSource` | embedded vs_1_1 source, pass 2 |
| 0x4ed500 | `sub_4ED500` | `Env_TriggerLightningFlashA` | body sets timer A = 16 (old kong comment "font size" wrong) |
| 0x4ed510 | `TextResource_GetStringOrDefault` | `Env_TriggerLightningFlashB` | 2-insn body sets timer B = 32 — prior kong name provably wrong |
| 0x5c7550 | return type `void` → `int` | (type fix) | tail-calls `_ftol2_sse`; fix made Hex-Rays recover the whole iris tail |

Plus explanatory comments at 0x57db30/0x57dbca/0x57dbf5/0x57dbf7/0x57dce0 (loader),
0x60fc66/0x57d4c0/0x606030 (terrain tint), 0x5c7550/0x5c7a00 (exposure), 0x57e440/0x57d940/
0x5f7163 (walk), 0x5789e0/0x579080/0x27219f0 (sky), 0x4ed500/0x4ed510/0x57ecfb/0x57edc4/
0x429ec9 (lightning). No speculative renames were left applied; everything above is
witnessed in the listed bodies.

---

## Appendix: C7 pre-port grill addendum (2026-06-11)

Targeted reads that closed the C6 record's three port-blocking open questions before the
`sky.gdshader` rewrite (all findings woven into §Sky dome above):

1. **Dome normals + scale** — full read of `build_sky_dome_mesh @ 0x578db0`: normal =
   `normalize(x, y_scaled/v14², z)` with `v14 = skyHeight/175.69`; height scale is Y-only.
   The reimpl computes the exact formula in the vertex stage (no baked normals needed,
   height changes need no mesh rebuild).
2. **prox dot form** — both embedded vs_1_1 sources normalize `clipPos.xyz` and take a
   `dp3` against `c14.xyz`: strictly 3-component.
3. **Dome fog color** — `D3DRS_FOGCOLOR` receives the active fog block value verbatim
   (`CD3DDevice_SetActiveFogColor @ 0x677040` → `CD3DDevice_SetFogAndBlendMode
   @ 0x677740`); mode 8 = VS-fog (table/vertex modes zeroed, `oFog` drives the blend).
   No dome-specific color derivation exists.

Bonus corrections: dome world anchor = camera with height halved (`@ 0x5790b2..0x5790d7`);
`advanced_clouds 0` flat pass is textureless (pass-1 NULL-texture effect).

### IDB edits applied

| Addr | Old | New | Evidence |
|---|---|---|---|
| 0x57e440 | `sub_57E440` | `Environment_ApplyFogAndAmbient` | re-applied C6 walk identity (rename had not stuck) |
| 0x677040 | `sub_677040` | `CD3DDevice_SetActiveFogColor` | sole writer of the FOGCOLOR source value |
| 0x3265718 | `dword_3265718` | `CD3DDevice_PendingFogColor` | read/write pattern in `SetFogAndBlendMode` |
| 0x326579C | `dword_326579C` | `CD3DDevice_AppliedFogColor` | redundant-state cache compare |

Plus comments at 0x578fbb (normal formula), 0x5790d0 (half-height anchor), 0x579b42
(textureless flat pass), 0x677040 (fog color path).
