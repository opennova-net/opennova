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
| `terrain_sector_compute_lighting @ 0x5c7550` | iris sampler (`env::iris_gain` + the live `ModulatorChain`, REN-5 #17); the entity-lighting writer side is [render/render-lighting-re.md](../render/render-lighting-re.md) |
| `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` | terrain_rgb reciprocal consumer (documented, deferred) |

Misnames fixed during the grill: `Render_SetFogParams @ 0x54b4b0` was an entity-pool sweeper
(now `Entity_DestroyUnreferencedPool4Entries`); `Environment_SetCurrentTime`/`Environment_SetTodRate` were "water
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

The float getter preserves the selected fixed tuple rather than renormalizing it: its
render/texture basis is `(-fixed[1], fixed[2], fixed[0]) / 65536`, stored at
`0x57d8be..0x57d8cd`. `PolyTrn_RenderTile @ 0x60da70` then byte-packs that direction
directly as `trunc((component + 1) × 127.5)` at `0x60e1d9..0x60e331` for the cached-tile DOT3 alpha bake. The hosted
`compute_sun_direction` and `compute_moon_direction` now expose the same direct
fixed-derived vectors. Consumers normalize only when their own witnessed math requires
it; the terrain byte-pack and star-field fixed-dot paths keep the getter tuple unchanged.

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
(identity in 6.6). The witnessed call ORDER (REN-5, decoded from the 16 `mov ecx, imm32`
call sites `@ 0x57ef97..0x57f03c`) is **modulator2 → modulator → light → sky → ground →
fog → skyfog → ceiling → cloud → floor → skybase/bright/highlight →
cloudbase/highlight/edge** — same-tick propagation (ported: `env::ModulatorChain`,
divergence #17). The hosts serve the post-modulator currents through the
`NovaEnvironment` per-tick writeback seam — fill/sun/fog/sky, skyfog, the
static ceiling/cloud/floor trio, and the six sky/cloud dome ramps, along with
the ÷64 gain (#17) and #27 scalars. The 2026-07-21 D-RLIT-1 closure hosts all
fourteen non-modulator color blocks in witnessed order: `cloud_rgb` feeds the
flat dome, while ceiling/floor feed the indoor iris samples
([render/render-lighting-re.md](../render/render-lighting-re.md)).
The modulator's target is the player's **iris auto-exposure sample**
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
- Scalar smoothers (the full set pinned at REN-6, 2026-07-06): spring-dampers
  `(d + 31) >> 5` with per-channel step and absolute clamps — fog distance
  (`Env_FogDistCurrent ← Env_FogDistTarget @ 0x57ede2`), camera FOV (`@ 0x57ee78`),
  the sun-dim percent (`Env_SunDimPct*` family `0x26c6830..`, default 0, no parser
  writes), **the rain percent** (`Env_RainPctCurrent/Target/Step/Max
  @ 0x26c6880/84/8C/90 @ 0x57ef01..0x57ef4c` — identified at REN-6: the debug
  overlay labels it `"Rain: %i%%"` `[orig: Debug_DrawEnvironmentValues
  @ 0x4efa91]`; the target is net-synced (`NapiNPClientMsg_0x00A @ 0x430311`;
  serialized at `NetPacket_WritePlayerState @ 0x4ffae8`) and mission-start-snapped
  (`@ 0x57d2d3`); current > 48 drives weather-particle vertical fall decay
  `[orig: @ 0x5de916]` and weather-particle render/update paths read it
  `[@ 0x5dec81; @ 0x5dee27]`), and the overcast blend (`@ 0x57ef4c..0x57ef92`);
  eighth-snaps `(d + 7) >> 3` with overshoot snap — sky height (`@ 0x57ee97`) and
  the cloud scroll rate (`@ 0x57eecc`).
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
  and restores fog for the world `[orig: sub_579CB0 @ 0x579cb0]`. Reimpl: tick and
  horizon-blend in undoubled block space using the smoothed (pre-overcast) fog
  distance, then double with saturation at the render tail. `NovaEnvironment`
  writes that one final skyfog value through `get_frame_clear_color()` to the
  GameWorld `ClearColor` and through `get_skyfog_color()` to `NovaSky`'s dome fog,
  preserving the invisible dome-rim/clear seam of the modulate2x path.
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
| 7 | @ 0x57e4f4–0x57e505 | smoothed sky height pushed on change → `Terrain_PushSkyDomeHeightFloat @ 0x610920` → `SkyDome_SetHeightAndRebuild` (dome) |
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

**Builder internals (ENG-2 sky-leg re-grill, 2026-07-06 — the libs/env port's witness
set):** the dome profile is a sphere cap of radius 3072 lowered so the rim (radius
1024 = 20 rows × 51.2) sits at y = 0 — `3072² − 1024² = 2²³`, so
`y(r) = sqrt(3072² − r²) − sqrt(2²³)` and the apex reference height is
`3072 − sqrt(2²³) ≈ 175.6906` (the exact value behind the shaders' rounded "175.69"
divisor). The x87 math is seeded from float32 literals (`.rdata`): `51.2f @ 0x7d75d4`,
`0.31415927f` (π/10) `@ 0x7d75c4`, `9437184.0f @ 0x7d75c8`, `3072.0f @ 0x7d75d8`,
UV scales `0.003125f @ 0x7d75d0` + `3/2048 @ 0x7d75cc`; `8388608.0 @ 0x7d75e0` is a
double. Index winding per quad: `(i, i+22, i+21), (i, i+1, i+22)` (`@ 0x578e00..0x578e86`)
— exactly the committed `sky/mesh` vector's order. Normal degenerate guard: zero length
→ `(0,0,0)` (`@ 0x578fd8`). Rebuild trigger: `Environment_ApplyFogAndAmbient` rebuilds
only when the SMOOTHED height changes (`Env_SkyHeightCurrent != Env_SkyHeightApplied
@ 0x57e4f4` → `Terrain_PushSkyDomeHeightFloat @ 0x610920` →
`SkyDome_SetHeightAndRebuild @ 0x579070`, which stores `this+0x48` and re-bakes;
buffer creation is `SkyDome_CreateBuffersAndBuild @ 0x579d10`, 441×40-byte VB +
2400-index IB). Ported: `env::build_sky_dome_mesh` + `kSkyDomeReferenceHeight`
(`libs/env/src/env_render.cpp`, dome section in `env_render_unit_test`).

**Scroll-rate state (back-filled citations):** the live rate `Env_CloudScrollRate
@ 0x26c686c` smooth-eighths toward `Env_CloudScrollRateTarget @ 0x26c6870` each tick
(`@ 0x57eecc`); the target — not the rate — is refreshed from the parsed
`Env_SkySpeedFixed` (= `sky_speed << 10`) at mission-start snap (`@ 0x57d2da` in
`Environment_SnapStateToTargets`) and by the net apply (`NapiNPClientMsg_0x00A
@ 0x4302ec`; the server serializes the live rate at `NetPacket_WritePlayerState
@ 0x4ffae2`). The snap never sets the rate itself: after a mission (re)start the rate
RAMPS from its previous value (0 at boot) toward the new target. The four accumulators
(`@ 0x57f1a5..0x57f1d1`: `0x26c680C/0x26c6810` += rate, `0x26c6814` += rate−rate/3,
`0x26c6818` += rate+rate/3) feed the render-side texture-transform translation
(`@ 0x5791de..0x579260`): layer 1 **U** = `−(camY_eng + acc_0x6810)·2⁻²⁸`,
**V** = `+(camX_eng + acc_0x680C)·2⁻²⁸`; layer 2 **U** = `−(camY + acc_0x6818[4/3])·2⁻²⁹`,
**V** = `+(camX + acc_0x6814[2/3])·2⁻²⁹`. In the render/Godot basis
(`Math_FixedPointToFloat3_YNegated @ 0x611210`: `d3d = (−engY, engZ, engX)/65536`)
that is `U = +camX_render·2⁻¹² − acc·2⁻²⁸`, `V = +camZ_render·2⁻¹² + acc·2⁻²⁸` — the
accumulator term is **negative on U**, and the u/v accumulator pairing is
(U ← 0x6810/0x6818, V ← 0x680C/0x6814). The pre-port GDScript added the accumulator
positively on both axes (divergence minted at the sky binding slice).

**VS constants** (uploads @ 0x579709..0x579868, both passes): c0-3 WVP, c4-7 world,
**c8 = eye world position** (fog reference — the pre-C6 "sky highlight float mirror" label
was wrong; @ 0x27219f0), c9 `[fogDist×0.9/65536, 0, 1, 0]`, c10 `[0, 0.5, 1, 0.25]`,
c11 skybase, c12 (skybright − skybase), c13 = direct near-unit sun getter tuple (pass 1) /
active getter tuple, moon at night (pass 2), c14 = the same point at camera + dir×2000 pushed
through the view-proj transform and normalized (clip-space proximity reference),
c15 skyhighlight, c16-19 / c20-23 the two UV scroll matrices (layer 1 `(camera +
scrollAcc1x)/2^28`; layer 2 `(camera + scrollAcc[4/3 U, 2/3 V])/2^29`), c24 cloudbase,
c25 (cloudhighlight − cloudbase) — **uploaded but read by neither shader (dead)**,
c26 cloudedge, c27 cloudhighlight.

**Color-constant upload scale (re-grilled 2026-07-14).** `render_skybox` passes the six
packed sky/cloud blocks through `Color_UnpackToFloat4` (`@ 0x579312`, `0x579326`,
`0x57933a`, `0x57934a`, `0x57935b`, `0x57936f`). Its normal branch multiplies each RGB
byte by `0.0078431377 = 2/255` (`@ 0x578985..0x5789c0`) and does **not** clamp the shader
constant; the vs.1.1 `oD0`/`oD1` color outputs are the saturation boundary before the
fixed-function stages. This scale is separate from the TSS `MODULATE2X` cloud-density
operation. Fog is deliberately excluded: `D3DRS_FOGCOLOR` receives the active packed fog
dword verbatim, so that already-weathered color remains byte/255 at the dome pass.

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

**Cloud texture load + the synthesized alpha (witnessed 2026-07-18, the
renderer-alignment session):** the sky maps load through the dedicated
render-texture loader `load_texture_from_archive @ 0x58b980`
(`terrain_init_rendering_resources @ 0x578a97/0x578aa5`, flags `0x100000`;
`Env_SkyMap1Path @ 0x26c63e8` / `Env_SkyMap2Path @ 0x26c63f8`, parse-coerced to
`.pcx`). For a PCX the loader decodes 32-bit RGB, re-reads the SAME file 8-bit,
builds a 256-entry palette-luminance table `A[i] = (85·(r+g+b)) >> 8`
(@ 0x58bc35..0x58bca9), and writes each pixel's alpha byte from its palette
entry (@ 0x58bcee) — **the cloud combine's alpha chain
(`a1 = tex0.a·tex1.a·2`, `a2 = a1²·2`) runs entirely on this synthesized
luminance alpha**. The `.env` comment "square rooted (bright gamma)" describes
the AUTHORED asset convention, not an engine transform — no sqrt exists in the
load path. A plain opaque decode (alpha = 1 everywhere) makes the cloud pass
cover the whole dome and the pass-1 gradient never shows through (divergence
row #36). Reimpl: `opennova::build_pcx_luminance_alpha_texture`
(pcx_texture_bridge) consumed by `EnvFile::_load_sky_map_texture*`; non-PCX
names keep the generic decode like retail's DDS-first path.

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

**Dome fog color (2026-07-21 correction).** The sky wrapper temporarily switches
`D3DRS_FOGCOLOR` to the final skyfog block while both dome passes run, then restores
the ordinary world fog `[orig: sub_579CB0 @ 0x579cb0]`. VS fog still comes from
`CD3DDevice_SetFogAndBlendMode(.., 8)`; its `oFog` blend converges on the same
post-horizon-blend, saturating-doubled skyfog used by the modulate2x frame clear.
Terrain and other world passes continue to consume the active ordinary fog block.

## Celestial bodies (`EffectWorld_LoadCelestialModels @ 0x5adc50`) — placement re-witnessed 2026-07-06

**The live renderers** (celestial-leg grill; three functions were misnamed, three dead):

- `render_celestial_bodies @ 0x5acaa0` (was "render_skybox_fog_layers"; called from
  `render_skybox @ 0x5798e0/0x579c7a`): sun and moon place at **camera + direction ×
  64.0** — full camera height, identity rotation, submit flag 0x100. Sun alpha =
  `clamp((1 − Env_OvercastBlend) × (0x640000 − Env_SunDimPctCurrent + 1)/100)`
  (`@ 0x5acbc1..0x5acbfa`; the SunDim channel is one of the #27 spring family, default
  0, no `.env` parser writes it). Moon alpha = `clamp01((fogDistInt − 400)/600) ×
  (1 − overcast)` without the fog shader (`× fogDistInt × 0.0002` with)
  (`@ 0x5acc40..0x5acccd`). There is NO `dir.y` visibility gate and NO
  `sky_height/175.69` distance scaling — the world overdraws the bodies (draw order:
  dome → bodies → world), which depth-tested no-write materials reproduce in the reimpl.
- `render_skybox_sun_glow @ 0x5acd00` (live: `render_main_scene @ 0x5c1904` +
  `Terrain_RenderSceneWithReflection @ 0x5c9714`): the glare pass — see the env #14
  closure below.
- `render_star_field @ 0x5ad9c0` (was "render_foliage_billboards_0"): 256 star
  instances (`Star_Instances @ 0x27E2E38`, 40-byte entries: camera-relative offset,
  billboard param, twinkle add/mask, brightness accumulator, direction). Per star:
  hidden when `dot(star_dir, light_dir) > ~0.98` (masked near the bright body),
  position = camera + offset, twinkle = `(accum + add + (prng16 & mask)) >> 1` with
  the rol4/rol11 PRNG (`Star_TwinklePrng @ 0x840B38` — the PRNG_Next16 algorithm on a
  separate state). A second part renders `Celestial_UplModel`. **The instance-table
  GENERATOR was found at REN-6 (2026-07-06)**: `Star_GenerateInstanceTable
  @ 0x5ac850` (ex kong misnomer `init_weather_particles`; its loop pointer starts
  at table+16 and strides 40 to `Celestial_SunModel @ 0x27e5648` — why no xref to
  the table base existed). Sole caller `EffectWorld_LoadCelestialModels
  @ 0x5add40` — the table regenerates per celestial load. Per star, all draws from
  `Star_TwinklePrng` (low-16 of `state = rol4(state + rol11(state, 11), 4) ^ 1`;
  a standalone step function exists dead at `Star_TwinklePrngNext_unused
  @ 0x5ac010` — the live sites inline it): entry dwords `[0]` offX = `(r −
  0x8000) << 9`, `[1]` offY same, `[2]` offZ = `((r + 0x20000) << 6) − ((|offX| +
  |offY|) >> 3)` (the dome shaping — high overhead, pulled down toward the rim),
  `[3]` billboard param = `(r & 0x3FF) + 12288`, `[4]` twinkle add = `r & 0xFF`
  (0 → 1, clamped to `255 − mask`), `[5]` twinkle mask = `31 >> (r & 3)` ∈
  {31, 15, 7, 3}, `[6]` UNWRITTEN (BSS zero — the render loop's brightness
  accumulator), `[7..9]` = `normalize(off >> 8)` 16.16 direction (the
  near-light cull input). Like the water noise field, the table CONTENT depends
  on the shared PRNG's call history at load (value-history quirk); a
  deterministic reimpl builds from a documented seed state.
- **Dead variants** (zero callers): `render_skybox_layers @ 0x5ac230` (the +64/+16
  fixed offsets + 0x2000 alpha the earlier notes described),
  `render_sun_lens_flare`, `render_foliage_at_camera`.

**env #14 closure — the glare occlusion** (`@ 0x5acd9e..0x5acf7f`): TWO jittered rays
per frame feed an 8-bit SLIDING window (`Glare_OcclusionWindow >>= 1` per sample, bit
0x80 = visible) — the window spans the last 4 frames (not 8 rays at once, correcting
the earlier summary). Ray = camera → camera + sunDir × 1024, jittered per sample from
`Glare_JitterFrameIndex` bits (engine-Y ±16 / ±8, height ±16); the coarse test is
`Terrain_RaycastLoResNoNormal @ 0x610860` → `Terrain_RaycastHeightmapLoRes @ 0x60cb80`
(the lo-res DDA — the ENG-3 raycast seed), refined by `Physics_RaycastIntContext` +
player-occlusion checks. Brightness (`Glare_OcclusionBrightness`) steps ±16 with a
±16 DEAD-BAND HOLD (never snapping) toward `popcount(window) × 32 ×
(Env_FogDistCurrent / 1000)` (flt_7DA0C4 = 1/65536000). Glow submit alpha =
`dot_view⁴/2 × brightness >> 8 × (1 − overcast) × SunDim fold` (`@ 0x5acfb8..0x5ad0a9`);
a scope check (`sub_581F60`) quarters it. Ported: `env::GlareOcclusionState`/
`glare_ray_jitter`/`glare_occlusion_tick`/`glare_glow_alpha_fixed` +
`celestial_sun/moon_alpha_fixed` + `kCelestialBodyDistance` (ctest landmark-pinned;
`glare_brightness_step` corrected to the witnessed dead-band form). The dot³² curve
(`compute_sun_glare_and_fog_blend @ 0x5ad610`) stays live via `sub_5AD8B0`.

### Original notes (pre-2026-07-06, kept for provenance)

Lazy-loaded by name into handles: sun (`Celestial_SunModel @ 0x27e5648`), moon (`0x27e5644`),
glare (`0x27e5640`), star (`0x27e563c`), plus hardcoded `upl.3di` (`0x27e5638`). Glare, star,
and upl load under render mode `0x300000` (additive); sun/moon load plain.
`render_skybox_layers @ 0x5ac230` submits the glare/sun models at camera + offsets (+64/+16
units, alpha `0x2000`, submit flag `0x110`); `render_skybox_sun_glow @ 0x5acd00` drives the
glare with 8 jittered terrain raycasts feeding a ±16/frame brightness hysteresis
(target = 32 × visible rays) and `compute_sun_glare_and_fog_blend @ 0x5ad610` computes
`dot(view,sun)^32 → glare (×192, clamp 255)` and `dot^128 → fog whitening (×40, clamp 40)`.

## Water surface (`render_water_surface @ 0x5c32c0`) — witnessed at the ENG-2 water leg (2026-07-06)

The pass was hiding as a SPLIT function: an 8-byte header (`sub esp, 68h` + a call to
the scar/decal setup `@ 0x58aa80`) fell through into unclaimed code — merged and named
`render_water_surface(is_underwater_view, is_reflection_subpass)`. Callers:
`FrameFX_RenderBloomPass @ 0x582a5d` and the terrain pass wrapper `@ 0x610650`. Flow:

- **Side gate**: bail when `Env_WaterHeightFixed == 0`, or when the camera is on the
  wrong side for the requested view (`is_underwater_view` ? camera must be below :
  above — the surface renders from either side with its own blend mode + texture set).
- **Per-frame noise textures** (`Water_GenerateNoiseTextures @ 0x5c0360`, was misnamed
  `generate_terrain_noise_textures`): pass 1 animates the static 128×128 field through
  the sine LUT — per byte `lut[(uint8)(field + (counter << (field & 1)))]`, two speed
  classes (odd bytes advance twice as fast). Pass 2: toroidal 9-tap kernel
  (3× the four corners + 4× the center cross, `>> 5`), folded to a ridge intensity
  `i = max(0, 128 − |k − 128|)`, packed `R=G=B=i`, `A = 255 − max(0, i²>>9)`. Pass 3:
  the DuDv/normal map — per pixel from the intensity byte,
  `R = wrap8(2·satsub8(c − c_up) + 0x80)`, `G` the same against `c_left`, `B = 0xFF`,
  `A = 0` (the MMX `psubsb/paddsb/paddb 0x008080FF` chain `@ 0x5c07c2..0x5c087d`).
  Both textures upload every frame. Wave phase `Water_WavePhase = frame_counter ×
  0x3000000` (`@ 0x5c0374`).
- **Init tables** (`Water_InitNoiseFieldAndSineLut @ 0x5c01a0`, once from
  `Water_InitSurfaceShaders @ 0x5c19b0` — renamed at REN-4 from the kong misnomer
  `Terrain_InitShaders`; call site `@ 0x5c19f8`): field = 128×128 samples
  `2·PRNG_Next16() −
  0x10000` min/max-normalized as `((v−min)<<8)/(range + range>>8)`; LUT =
  `128 + 64·sin(2πi/256)` (truncating ftol). `PRNG_Next16 @ 0x6130a0` =
  `state = rol4(state + rol11(state)) ^ 1` (state `@ 0x31BFBB0`) — the ALGORITHM is
  deterministic but the field CONTENT depends on the shared PRNG's call history before
  terrain init (value-history quirk; the reimpl builds from the boot state 0 for a
  deterministic witnessed-faithful instance — `libs/env water_init_noise_tables`).
- **The water surface material set** (REN-4, `Water_InitSurfaceShaders
  @ 0x5c19b0`): builds the two 128×128 GTextures `"w2a"` (color+ridge-alpha) /
  `"w2b"` (DuDv) over `Water_NoiseColorPixels`, then — gated on
  `Water_DetailLevel @ 0x24d2050` ≥ 2 (downgraded to 1 without caps bit 0x100
  = ps1.1) — assembles the **bump-reflection ps.1.1** (`t0` = DuDv,
  `texm3x2pad/texm3x2tex` → `t2` = the reflection RTT
  (`Water_ReflectionTexture @ 0x28ee8d8`, written by `sub_5C08B0`), `mul_x2`
  by `v0` vertex color, `mul_x4` by `t3` = the noise color, `add v1`
  specular) plus a **nightvision variant** (luminance `dp3 (0.25, 0.60,
  0.15)` + `mad_sat` squash + remodulate), and creates four materials
  (`GfxShader_Create4TexDesc`, slots {w2b, 0, reflection RTT, w2a}):
  `Water_ShaderBlend @ 0x28ee8c4` — **SrcBlend ONE, DestBlend SRCALPHA**
  (out = src + dst·α; blend mode 11 of `decode_blend_mode_to_d3d_states
  @ 0x680f00`), pass flags 0x30000 (specular+fog); `Water_ShaderOpaque
  @ 0x28ee8c8` — blend off, 0x20000; and the NV pair (`@ 0x28ee8cc/d0`).
  `Water_DetailLevel < 2` falls back to fixed-function TSS: the blend
  material = 2 stages {s0 `MODULATE2X(reflectionTex, DIFFUSE)`,
  α `SELECTARG1(tex)`; s1 `MODULATE2X(noiseTex, CURRENT)`,
  α `MODULATE(tex, DIFFUSE)`} with the same ONE+dst·SRCALPHA blend (flags
  0x1030000 add stage-0 clamp); the opaque one = mode 0x1020600 over
  {reflection, w2a}. Also `Water_ShaderReflectSimple @ 0x28ee8c0` (mode
  0x600 over the reflection RTT) and `Water_ShaderAdditiveFlat @ 0x28ee8d4`
  (textureless additive-diffuse mode 0x222; consumed by
  `render_main_scene @ 0x5c1913`). **Selection in `render_water_surface`
  (`@ 0x5c33e6..0x5c34ea`)**: camera-above view → the BLEND material;
  underwater view → the OPAQUE one; the second caller arg = the NIGHTVISION
  post-redraw (FrameFX re-renders water through the NV shaders and skips
  decals); every path applies pass flags 0x400000 (cull NONE — two-sided)
  via `GfxShader_ApplyPassChecked @ 0x677020` and calls
  `CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419/@ 0x5c3484` — which only
  latches ALPHAFUNC/ALPHAREF on the device (`[orig: @ 0x6770a0]` — no
  ALPHATESTENABLE write). **RE-GRADED at the 2026-07-07 fidelity grill: the
  water passes never enable the alpha test** — D3DRS_ALPHATESTENABLE rides
  pass-flag bit 0x40000 (`[orig: CGfxShader_ApplyPass @ 0x68326b; RS15
  commit in CGfxDevice_ApplyRenderStates @ 0x67e2fc]`), which 0x30000/
  0x20000 lack, and the per-pass state commit forces the test OFF; the
  technique blend block sets only blend states (`[orig:
  GfxBlend_ApplyToDevice @ 0x6817d0]`). The REN-4 "ref-32 far-fade cutoff"
  reading — and the `discard` it put in `water.gdshader` — was a misread
  that amputated the far water (the grazing-collapsed alpha term fell under
  the threshold from eye height); the far surface renders to the full march
  extent, converging to the fog color. Ported at REN-4 (corrected
  2026-07-07): `water.gdshader` re-expresses the witnessed blend exactly
  (`blend_premul_alpha` with `ALPHA = 1 − a`, no discard), cull_disabled;
  the reflection RTT consumer stays env #30, the underwater OPAQUE swap is
  hosted as the shader's `u_underwater_view` branch (`ALPHA = 1` under the
  premul pass = blend-off exactly), and the murk knob is the per-row angle
  chain (env #34).
- **UV state** (`@ 0x5c3348..0x5c33db`): texture scale `= 0.99996948 · w/(w − 0.2)`
  with `w` = the INTEGER part of the SMOOTHED fog distance (the word at
  `Env_FogDistCurrent+2`); bias `= 0.2 · scale` (→ `flt_8412B0/B4`). UV offsets ride
  the **layer-1 CLOUD-SCROLL accumulators** with a 32× camera term:
  `u = (uint32)(acc_26C680C + 32·camX_eng)·2⁻²⁸`, `v = (uint32)(acc_26C6810 −
  32·camY_eng)·2⁻²⁸` — in the render basis `u = cam_z/128 + acc·2⁻²⁸`,
  `v = cam_x/128 + acc·2⁻²⁸`, both accumulator terms POSITIVE (unlike the sky
  layers' negative-U). Ported: `env::water_uv_state`, `water_noise_color_pixels`,
  `water_noise_normal_pixels`, `water_init_noise_tables` (ctest-pinned landmarks +
  checksums).
- **The strips** (both marched in projected screen space, rows advancing away from
  the camera; REN-6 full decode 2026-07-06 — the earlier "2..9 columns" reading was
  wrong: **2..9 is the adaptive ROW-march stride**, `steps = clamp(int(row_1/w ×
  500.0), 2, 9)` `[orig: @ 0x5c265a low / @ 0x5c30d0 detailed; flt_7D6FB4 = 500.0]`,
  re-derived per row so near rows pack dense and far rows stride wide). The march
  substrate (both helpers fully decoded at the witness addendum): the 40-byte
  screen block from `terrain_project_sector_to_screen @ 0x5c0bf0` — the water
  plane (at the strip's height) projected at camera + horizontal-forward × 2000
  → screen origin `[0..1]`, the per-1000-units-along-view screen delta `[2..3]`
  (dy forced 1e-6 when 0), a 1000-unit reference point `[4..5]`, the normalized
  screen march direction `[6..7]`, and visibility `[8..9]` (in-viewport or
  halfplane tests); then per row `clip_line_to_viewport @ 0x5c0a30` intersects
  the row's screen line (point + 1/slope form) with the viewport rect,
  returning the row's LEFT and RIGHT screen endpoints (`out[0..3]`) + a
  crossing flag (`out[4]`) — the three row vertices are the clipped left
  endpoint, the midpoint, and the right endpoint, unprojected to world through
  the inverted view matrix (`Math_InvertMatrix4x4_Float_ToStatic @ 0x611960`
  cached per pass) with the per-vertex 1/w chain. Each stored
  row = **3 vertices (left edge / midpoint / right edge)** of the projected row
  span; rows cap at **1024** (low: byte counter `/120` `[@ 0x5c26cb]`; detailed:
  `/192` `[@ 0x5c3135]`); rows submit as **≤5-row triangle-strip batches stepping 4
  rows** (overlap 1 row) through STATIC index tables (`unk_8412B8` low /
  `word_841328` detailed) with `8·rows − 10` primitives per batch
  `[orig: @ 0x5c2734; @ 0x5c3209]`:
  - `render_water_strip @ 0x5c1d60` (water detail ≤ 1): 40-byte FVF verts (XYZRHW-
    style pos + fog W + diffuse + spec + 1 UV); **sin-table Y displacement** —
    noiseIndex = `Water_WavePhase + 0x200000`, `+= 0x55555555` per row, table index
    `>> 22` into the SHARED 1281-entry sin table + `off_849934` cos alias (the
    D-INF-4 table), amplitudes ×2⁻³⁰/×2⁻²⁹ `[@ 0x5c25c7..]`; per-vertex diffuse =
    gray brightness ×0x10101 + alpha byte; specular = `Env_WaterColorLit` bytes ×
    brightness `>> 8`; distance alpha `255 − dist_scaled/(fogEnd>>16)` clamped;
    murk angle term (`− Env_WaterMurk`, the witnessed constant chain 0.8 / 0.2 /
    0.15 / 19.2 / 96 / 128 / 255 / 300 / 400 / 0.25); per-vertex fog W clamped to
    **[4e-5, 1 − 2⁻¹⁵ (0.99996948)]**; UV = `t × fogScale − fogBias`
    (`flt_8412B0/B4` — the render_water_surface UV state).
  - `render_water_strip_detailed @ 0x5c27d0` (detail > 1; was MISNAMED
    `render_foliage_sprite_billboards`): 64-byte verts (FVF 0x1404C4 —
    XYZRHW + diffuse + specular + TEX4 sizes 2/3/3/2 `[@ 0x5c28e1]`; t1/t2 =
    the texm3x2 reflection-bump rows, t3 duplicates t0); **NO vertex
    displacement** — flat rows; the animated color+DuDv textures carry the
    wave look. **Chain re-graded at the 2026-07-07 port leg (IDB re-read;
    ported as `env::water_build_strip_rows` + ctest pins)**: colors are
    ROW-CONSTANT `[@ 0x5c2f0a..0x5c2f2b]` — `base = 1 − murk` (underwater 1,
    nightvision flat 0.1), `k = 0.2 + 0.8·base`, `sin = |Δy|/|Δ|` of the
    right-edge camera ray; brightness = `int(lerp(192k, 38.4k, sin))`
    `[flt_7DBFA4/A8]`, alpha_term = `int(lerp(0, 229.5·base, sin))`
    `[flt_7DBFA0 — a FLOAT, distinct from the low tier's dbl_7DBF70]`,
    `a = clamp(int(t·2²⁴/fogEnd_fp), 0, 255)` `[dbl_7DBF98 = 2²⁴ on BOTH
    paths]`, dist_alpha = `255 − a²/255`, diffuse =
    `(alpha_term·dist_alpha/255) << 24 | 0x10101·brightness`; specular =
    `(dist_alpha << 24) | WaterColorLit RGB × int(lerp(255(1−base),
    128(1−base), sin)) >> 8` (nightvision drops the RGB `[@ 0x5c2ef8]`);
    per-vertex depth ("fog W") = `(t·uvScale − uvBias)·(1/t)` clamped to
    **[8.0422355e-05 (0x38A8A8AC), 1 − 2⁻¹⁴ (0x3F7FFC00)]**
    `[flt_7DBF7C/flt_7C4658 @ 0x5c2c1f..0x5c2c4a]` — the earlier
    "[4e-5, 1 − 2⁻¹⁵]" reading was the low-tier approximation. Batches lock
    `8·rows − 10` VERTICES and draw `8·rows − 12` primitives
    (`DrawPrimitive @ 0x5c3209` passes count − 2; the earlier "8·rows − 10
    primitives" was the vertex count).
  - **The detailed tier's PIXEL SHADER (witness addendum, 2026-07-07 —
    found at the port's black-water debug).** `Water_InitSurfaceShaders
    @ 0x5c19b0` (detail ≥ 2 path) assembles an embedded ps.1.1 consuming the
    64-byte verts — the REN-4 water.gdshader reading had ported the LOW-tier
    (detail < 2) `GfxShader_Create2TexDesc` TSS material instead:
    `tex t0 (DuDv) / texm3x2pad t1, t0_bx2 / texm3x2tex t2, t0_bx2 (the
    REFLECTION texture, bump-perturbed) / tex t3 (color noise); mov r0, t2;
    mul_x2 r0.rgb, r0, v0; mul_x4 r0.rgb, r0, t3; +mul_x2 r0.a, t3.a, v0.a;
    add r0.rgb, r0, v1` — pixel = reflection ×2 diffuse ×4 colorNoise
    + specular, **alpha = noiseAlpha × diffuseAlpha × 2** (the co-issued
    mul_x2 — the ×2 is load-bearing: without it the ref-32 alpha test kills
    the whole surface). Materials: `GfxShader_Create4TexDesc(DuDv, 0,
    Reflection, ColorNoise, desc, 0x30000)` blend / `0x20000` opaque, + an
    NV pair whose ps appends the witnessed NVG dp3/mad_sat/mul tail; the
    first hosted water.gdshader ran this chain with `u_water_color` as a
    tracked t2 stand-in; env #30 replaced it with the live reflection RTT.
  - The third-arg variants (call sites `[@ 0x5c3489..97 above-water:
    (height, 0, nv); @ 0x5c3539..47 underwater: (height, underwater, nv)]` —
    the earlier "isReflection" reading was the UNDERWATER-VIEW pass): the
    underwater view skips the murk term (base = 1), renders solid-white
    diffuse with LINEAR dist_alpha `255 − a` (no square), keeps the boot
    stride 4 (never re-derives), and flips t2's V to `1 − V` on all three
    row vertices (the +34h/+74h/+B4h writes `[@ 0x5c306d..0x5c3085]` — a
    texcoord flip, not a vertex-Y mirror); the ×255 ↔ ×229.5 double swap
    belongs to the LOW tier alone (`dbl_7DBF70`'s single xref @ 0x5c244b).
    The nightvision arg is the flat-0.1 redraw.
  - **Depth, fog, and the pass-state word (the 2026-07-07 fidelity grill).**
    The strip's per-vertex depth `(t·uvScale − uvBias)·rhw` is an arithmetic
    REPLICA of the scene projection depth: `uvScale = 0.99996948·w/(w−0.2)`
    factors as viewport-MaxZ × the D3DX A-coefficient with near 0.2 and
    far = the fog INT `w` — the scene projection is
    `D3DXMatrixPerspectiveFovLH(near = g_ProjectionNearZ 0.2, far =
    g_ProjectionFarZ)` `[orig: Render_SetViewAndProjectionMatrices
    @ 0x58d9de]` with the range set by `Render_SetProjectionDepthRange
    @ 0x58abe0` (ex `sub_58ABE0`; near pinned 0.2 `@ 0x58ac0a`, the
    0.99996948 = 1 − 2⁻¹⁵ viewport-depth slot written `@ 0x58ac32`), and
    missions pass **far = 0x400 = 1024** — the dome-rim radius —
    `[orig: Game_StartMission @ 0x524721]`. Same-family curves (far `w` vs
    1024, sub-1e-4 apart) + ZWRITE ON + LESSEQUAL give retail its
    deterministic shoreline on the 16/24-bit z-buffer. The pass-state word
    (decoded at this grill, full map in
    [render/render-material-re.md](../render/render-material-re.md)):
    0x10000 SPECULARENABLE, 0x20000 FOGENABLE, 0x40000 ALPHATESTENABLE,
    0x100000 zwrite-OFF (inverted), 0x200000 zfunc-ALWAYS (inverted),
    0x400000 cull-NONE — the water blend pass 0x30000 = specular + fog,
    z-write ON, **no alpha test**; the opaque underwater pass 0x20000 = fog
    only. With FOGENABLE on and XYZRHW vertices, the D3D8 fog factor is the
    **specular alpha** — which the rows fill with `dist_alpha` (255 near →
    0 far); the water fogs by the row's own distance curve, not the scene
    fog table. Reimpl mapping (2026-07-07): `water.gdshader` gains
    `depth_draw_always` (the witnessed z-write) + a relative `3×10⁻⁴`
    view-depth pull (a TRACKED approximation standing in for "same-curve
    equal depth ⇒ deterministic shoreline"). The earlier 2⁻¹⁵-of-remaining-
    NDC form was rejected because reverse-Z compression caused far-altitude
    water overpaint. The live form preserves NDC x/y while moving depth only,
    fogs by `CUSTOM1.a` (the spec-alpha
    factor) toward the scene fog color, and drops the misported ref-32
    discard (see the material-set bullet re-grade above).
- **Reflection pipeline (#30 internals witnessed at REN-6, 2026-07-06).** The
  reflection prerender runs BEFORE the main frame (`Render_TerrainScene @ 0x610c80`
  → `Water_ReflectionPrerender @ 0x5c2780`, ex `sub_5C2780`, gated on
  `g_WaterActive`): it packs the LIVE camera block {x, y, z, yaw, pitch, roll} and
  calls **`render_main_scene @ 0x5c1240` — the reusable offscreen scene renderer**
  (also `render_cinematic_multiview`). Its frame: far-plane scale (`sub_58A920`)
  → projection → the PolyTrn render context (viewMatrix copy + camera dirs ÷65536 +
  a plane block: when water is active, `plane[4] = waterHeight_float − 0.1` with
  enable flag — the below-plane clip bias) → clear color ← **`Env_SkyfogBlock`** +
  clear depth `1 − 2⁻¹⁵` → **`GTexRT_SelectThunk(&Water_ReflectionTexture)` = RTT
  begin** → BeginScene/viewport/proj → scar ctx → `Environment_ApplyFogAndAmbient`
  (`@ 0x5c164c`) → fog color/mode → the sky dome (`sub_579CB0`) → the lo-res
  terrain leg (`Terrain_RenderSectorBatchLit`) → projection rebuilds + `sub_5C90A0` +
  **`Water_RenderReflectedWorldScene @ 0x5c8510`** → an inline effect quad (the
  `Water_ShaderAdditiveFlat` consumer `@ 0x5c1913`) → **celestial bodies + sun glow
  mirrored into the reflection** (`@ 0x5c18fb/0x5c1904`) → restore +
  `GTexRT_RestoreThunk` = RTT end. `Water_RenderReflectedWorldScene` (the ex
  "Water_RenderReflectedWorldScene three flushes" DECOMPILE-FAIL — a 5-byte header `call sub_58AA80`
  falling through into the body, real extent → retn `@ 0x5c8af8`): fog/ambient
  push (0,0) → `CTerrainRenderer_BuildLightingShaderConstants(0)` (NORMAL
  lighting — the arg-1 mirrored-lighting sub-passes belong to the MAIN pass's
  per-wave mirror, not this scene) → zeroes `g_WaterMirrorMatrix` and builds it as
  the water **CLIP-plane texture matrix** (`[+0]=0, [+0x10]=1, [+0x20]=0,
  [+0x30]=0.5−waterHeight_float, [+0x3C]=1` ⇒ `u = y − wh + 0.5` — the CLIP
  technique's TexClip1D coordinate, ref-128 alpha cut AT the plane) + arms
  `g_WaterMirrorActive = 1` → sector models → the first entity wave → flush(1) →
  below-side entities → above-side entities → flush(0) → (detail ≥ 2: foliage
  tile/LOD updates → flush(0)) → particle passes ×2 → trail strips. The camera
  MIRRORING about the plane is built inside `render_main_scene`'s mid-function
  view-matrix section (Hex-Rays elides it; the exact transform is pinned at port
  time). RTT allocation is actually `sub_5C08B0 @ 0x5c08d1..0x5c0937`:
  **256×256** for detail 2, **512×512** when `Water_DetailLevel >= 3` or the
  capture/special flag is set. `render_main_scene @ 0x5c1464..0x5c1614`
  applies `size−1` square viewport bounds and rebuilds projection from that
  square while preserving horizontal FOV. `sub_5D6150` only initializes
  reflection direction/state; it never allocates `Water_ReflectionTexture`.
  The Godot adapter therefore keeps the witnessed texm3x2 row dots intact, then
  maps their main-viewport-normalized result into the square camera projection
  around UV center with scale (1, source height/source width). Omitting that
  sim/present-boundary conversion makes the reflected image swim with view pitch.
- **Water height precedence** (witnessed): the `.env` parse writes
  `Env_WaterHeightFixed` first (`Game_LoadTerrainDuringConnect @ 0x52073b`), then
  `Terrain_Init @ 0x60fcb1..0x60fcba` OVERRIDES it — but only when the terrain value
  carries bit 31 (`jns` skips; the store masks `& 0x7FFFFFFF`); BMS overrides apply
  later still (`@ 0x525371`, attrib bit 0x1). Retail precedence: **BMS >
  TRN(flagged) > ENV**. The global is 16.16 world-Z (the file value is in half-units,
  `<< 15` = ×0.5×65536) — settles the units question flagged in
  world-wac-ai-re.md.

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
| day length s16 (minutes, min 60) `Bms_TodRateMinutes @ 0xa7641a` (was misnamed `Bms_WaveAmplitude`) | local play | `Environment_SetTodAdvanceRate @ 0x57d170` (was misnamed `Terrain_GenerateWaterNoiseTextures` — it never touched textures): `Env_TodAdvancePerTick = 0x18000000 / (3720 × max(v, 60))` |
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
  **Implemented at REN-5 (2026-07-06)**: the chain is live (`env::ModulatorChain` +
  `NovaWeatherCore`, divergence #17 FIXED below); the witnessed tick order is
  modulator2 → modulator → the color blocks, same tick
  `[orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c]`. The hosted
  exposure target is the OUTDOOR iris sample; the 3-point camera-ray march +
  interior sampling ride [render/render-lighting-re.md](../render/render-lighting-re.md)
  D-RLIT-2.
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
  - `sample_terrain_colormap_tinted @ 0x606030` is called inside
    `generate_foliage_instances_0 @ 0x5ffdd0` and computes the FULL-tint form, but
    the fresh 2026-07-13 instruction trace follows the output record through its
    final write: the packed diffuse is overwritten by
    `clamp(int(source_y×128),0,255)<<16`, the detail wind bend carrier. The sampled
    color therefore has no rendered foliage effect. The previous LIVE foliage-tint
    conclusion stopped at the internal sample around `0x600197` and was wrong.
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
| 8 | envscale applied at interpolation to keyframes only; original bakes at parse into every `*_rgb` (positional, leaks across files in a load) | **Tracked decision**: Config and raw `EnvFile` getters preserve authored values for round-trip; the `NovaEnvironment` engine view applies envscale at byte quantization to water/lightning and hosted ceiling/cloud/floor targets (equivalent when envscale precedes all colors, which the corpus sweep validates). `terrain_rgb` remains intentionally unscaled, matching retail |
| 9 | fog→skyfog mirror via per-keyframe flag vs `0xC0C0FF` value sentinel over leftover slots | **Tracked decision**: flag semantics (equivalent for well-formed files; leftover bleed not replicated) |
| 10 | `sky_map*` `.pcx` coercion not applied in reimpl parse | **Fixed** at texture-resolution layer (names stored verbatim for round-trip) |
| 11 | Negative color components: original packs garbage (no lower clamp) | **Tracked decision**: clamp to 0 (no UB replication) |
| 12 | Default sky_height raw-200 quirk (≈0.003 units) | Documented; reimpl default mirrors the quirk via comment, authoring template sets 175 |
| 13 | `vertex_rgb` parsed by reimpl, ignored by retail JO | Keep parsing for round-trip; engine view ignores (modulator identity) |
| 14 | Sun glare occlusion — the witnessed model (re-grilled 2026-07-06) is TWO jittered rays per frame into an 8-bit sliding window + dead-band hysteresis, not 8 rays at once; the glow alpha is the `dot⁴/2` chain, not `dot³²` | **FIXED 2026-07-06 (the celestial leg)**: `env::glare_occlusion_tick` + `NovaGlareOcclusion` + the NovaCelestial terrain ray march (32-unit-step bilinear stand-in for the lo-res DDA `@ 0x60cb80`, flagged for ENG-3); ctest + `celestial/occlusion`/`celestial/glow` vectors pin it |
| 15 | Thunder sounds on lightning timer epochs | Deferred; **fully specced by C6** (SoundBank trigger 0 / 0x80 on bank `dword_24E0914`, `SETFLASH1` net-command start; sequencer B unreachable in retail) — wiring lands with WAC weather |
| 16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade | Documented; **C6 corrected the precedence**: overcast.def is additive-after-success (and the sole table for NULL map), never a fallback; a missing/failed `.trn` aborts the whole TOD load. Runtime port carries .env table only until weather/WAC work lands (overcast blend defaults 0 = pure .env, matching clear weather) |
| 17 | Iris auto-exposure (modulator gain) | **FIXED 2026-07-06 (REN-5 — the modulator chain went LIVE); full sixteen-block set completed 2026-07-21.** `env::ModulatorChain` ticks modulator-2 → modulator → all fourteen color blocks in witnessed same-tick order `[orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c]`; the modulator chases the iris target over 62 ticks (`WeatherColorBlock::set_step_deltas` `[orig: ColorBlock_SetStepDeltas @ 0x57d940]`), and the ÷64 render scales reach consumers. Skyfog, ceiling/cloud/floor, and the six sky/cloud dome ramps join fill/sun/fog/sky; fog/skyfog operate undoubled through the block pass, then horizon-blend and double at the render tail `[orig: @ 0x57f037..0x57f1b1]`. The static cloud current feeds the flat dome and pre-modulated ceiling/floor feed indoor iris samples. D-RLIT-2 retains only its bounded geometry residuals; see [render/render-lighting-re.md](../render/render-lighting-re.md). |
| 18 | Earthquake / rain / wind oscillator rings | Weather-system scope; ported constants documented, wiring deferred with WAC weather |
| 19 | `terrain_rgb` terrain-stack consumers | **FIXED; corrected by foliage re-grill 2026-07-13.** The observable renderer consumer is the `.til` tile overlay (`EnvFile.tile_overlay_tint_factor` → `u_tile_overlay_tint`, MODULATE2X over HALF, 254/255 at default `[orig: PolyTrn_RenderTile @ 0x60df0d]`); the reciprocal remains live in `EffectWorld_TickInstancesAndLightScale @ 0x5aa170`. The texture bake is dead. `sample_terrain_colormap_tinted @ 0x606030` executes inside the detail generator, but its result is overwritten by the source-height bend byte before vertex emission, so foliage terrain tint is not observable and the removed reimpl `terrain_tint` property was an invented consumer. Untinted terrain remains faithful. |
| 20 | Sky dome combine | **FIXED by C7; upload-scale corrected 2026-07-14; reverse-Z facet closed 2026-07-15; skyfog seam corrected 2026-07-21**: `sky.gdshader` + `nova_sky.gd` structurally port the recovered two-pass gradient/cloud chains, builder dome, per-pass anchor, textureless flat path, and reverse-Z conversion. Six packed sky/cloud constants upload at **2/255** and saturate at the vs.1.1 outputs. Dome VS fog now consumes the final post-horizon-blend doubled skyfog selected by `sub_579CB0`, matching the frame clear; world passes retain ordinary fog. |
| 21 | skyfog frame clear color | **FIXED 2026-07-05**: the horizon blend is ported libs/env-first (`horizon_blend_skyfog`, byte-exact vs the MMX sequence, ctest-pinned + parity-vector cell) and consumed - `NovaEnvironment.get_frame_clear_color()` drives the GameWorld `ClearColor` WorldEnvironment (above-water skyfog blend / underwater lit-water, the witnessed choice); the vehicle alternate-fog view and the dome-fog application ride their subsystems. Editor preview adoption rides ENV-1. **REN-7 close-note (2026-07-07)**: the consumer was WRITTEN but never RENDERED — the `ClearColor` Environment shipped `background_mode = BG_SKY` (Wave-1) with no Sky resource, which Godot draws as BLACK while ignoring `background_color`; every runtime view carried a pure-black dome-rim seam row (aerial views a black band; #29's grazing fade exposed it in water-horizon views — D-TERRAIN-3's substance). Fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` (Godot ambient must never inject into the witnessed lighting model), GUT-pinned in `game_world_test`. Rendering the clear exposed a SECOND facet — the 2026-07-05 port's "undoubled = the non-modulate2x device path" reasoning was INVERTED for this reimpl: since D-RMAT-7 the reimpl reproduces the **MODULATE2X** device's framebuffer bytes (the ×2 combine + doubled fog table, calibrate-proved), and on that path the Clear consumes the post-blend DOUBLED skyfog VERBATIM (the halving `(c>>1)&0x7F7F7F7F` `[orig: @ 0x67715d]` is the non-modulate2x fallback, no reimpl analog); the undoubled clear rendered the below-rim band at exactly half the fogged dome rim ((77,91,138) vs (151,179,251) measured at noon). `get_frame_clear_color()` now doubles-with-saturation after the blend (the witnessed order: blend `@ 0x57f037..0x57f0a1` THEN double `@ 0x57f1b1`); the underwater branch was already render-space (`Env_WaterColorLit` = the ×2-gain `>>7` form); and the blend's distance input corrected from the load-time parsed field to the smoothed current (`get_fog_level()` ladder — the witness reads `Env_FogDistCurrent @ 0x26c681c`; #27's "every consumer" claim now actually holds for the clear). Env vectors re-dumped (the frame-clear token only; the input correction moves no corpus row — the bare-env grid never drives the smoothed scalars). The witness gap closed with it: the below-rim region is **clear-only in retail** — no skirt/ring geometry exists anywhere in the frame walk; the sky-pass terrain leg (`Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`, ex `sub_60C670`) is the plain fogged sector batch bracketed by `D3DRS_AMBIENT=0xFFFFFF`/`LIGHTING=1`, and the seam invisibility mechanism is convergence-in-the-same-block (the dome pass fogs toward the DOUBLED SKYFOG while drawing the dome `[orig: sub_579CB0]` — the same value the clear paints; terrain/water fog out at the 1024 fog reference = the dome rim radius) |
| 22 | Weather PRNG carry: the GDScript port added bit-31 (0/1) where the original's cdq/and/add idiom adds `0x1ABB09` on a negative rotate — a Hex-Rays transcription bug (signed `(next >> 31) & 0x1ABB09` re-typed unsigned) that silently forked the sequence from the first negative rotate | **FIXED 2026-07-05 (minted-and-closed at the ENG-2 port)**: `env::WeatherOscillator::reroll` implements the signed idiom `[orig: Environment_UpdateWeatherTick @ 0x57e9fc..0x57ea16]`; seed `0x12333333` — the mov imm32 `[orig: @ 0x57d2ff in Environment_SnapStateToTargets @ 0x57d1e0]` (the port initially transcribed it `0x12345633`; corrected as #25); the ctest pins the witnessed word sequence and explicitly guards against the bit-31 variant. Invisible to the sampled sway vectors (the spring saturates), so no wa/wb key moved for THIS fix alone |
| 23 | Lightning long-sequencer epochs were max-combined (`maxf`) in the GDScript, holding a C8 plateau; the original SETS each epoch level (the witnessed staircase C8 C8 C8 96 96 C8 C8 96 64 32 32 00), and the additives are integer-truncated bytes, not floats | **FIXED 2026-07-05 (minted-and-closed)**: `env::LightningSequencers` + `lightning_additives_packed` port the SET semantics and the exact pmullw/psrlw byte math `[orig: Environment_UpdateWeatherTick @ 0x57ec6f/@ 0x57ed0a; Environment_SetLightningFlash @ 0x57d320 — which also zeroes the directional-light slot]`; parity vectors `wc/long_seq` + `wc/long_k01/k09/k12` re-dumped with this witness |
| 24 | The NovaWeather wind model vs the witnessed engine: (a) `wind_strength` mapped 0..100 onto 0..8192, but the oscillator's `15*prev` feedback term is stable only for intensity <= 273 — the old mapping drove the 32-bit state divergent and "survived" via GDScript's 64-bit wrap + clamps; (b) the default was still air, where retail runs `Env_WindScale = 256` constantly (`[orig: Environment_InitDefaults @ 0x57c1d1]`, its ONLY writer — the ambient foliage sway every retail map has); (c) the smoothers chased their own written-back output instead of the TOD keyframe targets (`[orig: Environment_ComputeTimeOfDayColors @ 0x57de40]` refreshes every block's target slot each frame) | **FIXED 2026-07-05 (minted-and-closed)**: strength now maps 0..100 → 0..256 with default 100 (= the retail constant); the duration/decay gust remains an OpenNova authoring extension, now armed-only (unarmed wind never decays, matching the constant-WindScale witness); NovaWeather feeds `get_*_target()` keyframe targets into NovaWeatherCore. Parity vectors `wa/*`, `wb/*`, `we/*` re-dumped under these witnesses (`wa` now IS the witnessed ambient-256 series, cross-pinned byte-equal in `env_render_unit_test`) |
| 25 | Weather-PRNG seed transcription: the reimpl carried `0x12345633` (libs/env slice 1, inherited from libs/wac); the binary's immediate is `0x12333333` — the SAME constant seeds the WAC RNG (`mov dword_C6EA40` `[orig: WacScript_InitAndLoad @ 0x4f966b]`), where the mistranscription originated | **FIXED 2026-07-06 (minted-and-closed at the ENG-2 sky-leg re-grill)**: `WeatherOscillator.prng = 0x12333333` `[orig: seed imm32 @ 0x57d2ff]`; the WAC VM's `next_rand` ALSO carried #22's unsigned bit-31 carry — both libs/wac bugs fixed in the same commit `[orig: rol9 + sar/and/add @ 0x4f5a83..0x4f5a91]` (no committed test pinned the wrong WAC stream). env ctest word/wind pins regenerated; the sway-bearing GUT keys (`wa/k004..k256`, `wb/k016..k096`, `wc/long_k*`, `we/k*`) re-dumped under the witness — `wa/k001` is seed-invariant (both seeds share low-12 bits at tick 1); every level-only, color, float, and non-weather key unchanged |
| 26 | Cloud-scroll consumption model: the float GDScript (a) skipped the rate RAMP — the snap refreshes only the TARGET (`@ 0x57d2da`) and the live rate smooth-eighths toward it (`@ 0x57eecc`), so a fresh scene ran full-rate from tick 1; (b) added the accumulator term POSITIVELY on both UV axes where the witnessed texture transform NEGATES it on U (`@ 0x5791de..0x579260`); (c) the libs field labels had the layer-1 u/v pair inverted (value-equal — both advance at rate) | **FIXED 2026-07-06 (minted-and-closed at the sky binding slice)**: `NovaWeatherCore` owns `CloudScrollState` ticked at the witnessed tick tail; `NovaSky`/`NovaWater` consume through the weather seam (`get_cloud_uv_offset1/2`, `get_cloud_uv_rate_per_second`; standalone hosts fall back to a private core — one math home); the UV translation is `env::cloud_scroll_uv_offsets` (U-negative). `sky/k001`/`sky/k064` re-dumped under the witness; `nova_sky_test` pins the seam + the U sign **2026-07-21 cadence correction:** GameWorld owns a separate fixed 62 Hz accumulator from the 62.5 Hz mission simulation and repeats `advance_mission_clock(1)` then `NovaWeather.tick_fixed()` per quantum; standalone weather, sky, and water fallbacks use the same 62 Hz banking. Water noise generation intentionally remains once per rendered water frame. |
| 27 | Smoothed scalar spring channels unwired: the tick smooth/spring-steps fog distance (`Env_FogDistCurrent @ 0x57ede2`, consumed by the dome fog c9 `@ 0x5792c2` and `Environment_GetFogEndDistance`), sky height (`Env_SkyHeightCurrent @ 0x57ee97`, gating the dome rebuild `@ 0x57e4f4`), camera FOV (`@ 0x57ee78`), the now-identified `Env_SunDimPct` channel (`0x26c6830` family — dims the sun body + glare, default 0, no parser writes it), the rain percent (`Env_RainPctCurrent @ 0x26c6880` family — identified at REN-6, see the weather-tick smoother list), and the overcast blend (#16's runtime facet, `@ 0x57ef62`); the reimpl's consumers read PARSED `.env` values — a TOD/env scrub snaps instantly where retail ramps in | **FIXED (2026-07-06, the REN-6 port leg)**: `env::EnvScalarChannels` ticks the witnessed springs in-order between the sequencers and the color blocks (`NovaWeatherCore.scalar_channels`, ctest-pinned steps `((d+31)>>5` / eighth-snap); targets refresh from the PARSED values each tick and the mission snap touches targets only — currents always ramp `[orig: @ 0x57d1e0]`; the smoothed currents write back through the env seam (`NovaEnvironment.set_smoothed_scalars`) so EVERY consumer (dome c9/u_sky_height, water UV state, object/terrain fog ends, the #21 frame clear) serves the ramp; the SunDim channel is live end-to-end (celestial sun alpha + glare fold). Residuals: the rain%/overcast channels are state-live awaiting their consumers (weather particles / the overcast cross-fade), FOV rides the camera system, and the per-channel step/max clamps arrive with the weather-command wiring (`@ 0x57f1e0`) |
| 28 | Water-height precedence: the reimpl ladder ran override → `.env` → terrain-fallback (env wins over terrain); witnessed retail order is BMS > TRN (bit-31-flagged store at `Terrain_Init @ 0x60fcb5`, AFTER the env parse) > ENV | **FIXED 2026-07-06 (minted at the water grill, closed at the water binding slice)**: the NovaWater ladder reordered — a flagged terrain height beats the `.env` one; the reimpl override rung stays on top as the authoring seam **2026-07-21 lifecycle correction:** the reimpl ladder is direct authoring > explicit BMS (including zero) > signed nonzero TRN > ENV; an authoritative zero disables the surface, shader split, and reflection RTT instead of leaving the packaged scene's former 10.5-unit phantom plane. |
| 29 | Water surface tessellation: witnessed = screen-marched adaptive strips from the camera (3 vertices per row — left/mid/right of the projected span; adaptive ROW stride `clamp(int(row_1/w × 500), 2, 9)` — the earlier "2..9 columns" reading corrected at REN-6; 1024-row cap; ≤5-row strip batches stepping 4 through static index tables; the low-detail path adds sin-table Y displacement from the shared D-INF-4 table) `[orig: render_water_strip @ 0x5c1d60; render_water_strip_detailed @ 0x5c27d0]`; the reimpl draws a static camera-snapped 65×65 plane | **FIXED (2026-07-07, the REN-6 tail)**: the DETAILED tier is live end to end — `env::water_*` structural translation (screen block + row clip + adaptive stride + backstep hunt + 1024 cap + row colors + batch table; 40 ctest pins), `NovaWaterCore.strip_*` packed-array bridge (godot==render componentwise basis, D3D row-vector view rebuild), `nova_water.gd` per-frame strip ArrayMesh (COLOR = row diffuse, CUSTOM0 = depth/rhw/screen UV, CUSTOM1 = specular; witnessed batch-table indices), and `water.gdshader` runs the witnessed ps.1.1 chain (§The strips addendum — alpha = noiseA × diffuseA × 2, reflection ×2 diffuse ×4 noise + specular; `u_water_color` was the historical t2 stand-in, replaced by #30's live RTT). Goldens re-pinned (`water/mesh` strip invariants, `water/strip` basis pin; `water/snap` retired). **2026-07-07 fidelity-grill facets (user-reported draw distance + shoreline z-fight)**: the far-water amputation was #34's misported ref-32 discard (re-graded there — the water passes never enable the alpha test) plus the wrong fog curve (now the witnessed spec-alpha `dist_alpha` factor, see §Depth/fog/pass-state); the shoreline flicker was the unhosted depth model — retail writes a projection-replica depth (ZWRITE ON, deterministic same-curve shoreline), hosted as `depth_draw_always` + the tracked relative `3×10⁻⁴` view-depth pull (the rejected 2⁻¹⁵ NDC form overpainted far-altitude terrain); the underwater opaque `0x20000` swap is now HOSTED (`u_underwater_view` → `ALPHA = 1` under the premul pass = blend-off; selection fed per frame from the camera side). Residuals in-row: the LOW tier (detail ≤ 1, sin-displacement) stays unported (the reimpl runs detail > 1); the nightvision redraw remains unhosted (FrameFX) |
| 30 | Water reflection passes exist in retail (`sub_5C08B0` RTT allocator `[orig: @ 0x5c08d1..0x5c0937]`, `Terrain_RenderSceneWithReflection @ 0x5c93a0`, per-strip mirrored verts + 229.5 alpha scale) — the reimpl renders none | **WITNESSED-READY-DEFERRED (internals closed at REN-6, 2026-07-06)**: the offscreen pipeline is fully witnessed — `Water_ReflectionPrerender @ 0x5c2780` → `render_main_scene @ 0x5c1240` (RTT begin/end on `Water_ReflectionTexture`, skyfog clear, the clip plane at `waterHeight − 0.1`, mirrored sky/terrain/world/celestial/glare) with `Water_RenderReflectedWorldScene @ 0x5c8510` (the ex three-flush decompile-fail; builds `g_WaterMirrorMatrix` as the water CLIP-plane texture matrix `u = y − wh + 0.5` and arms `g_WaterMirrorActive`) — see §Reflection pipeline; the earlier "strip far-edge mirroring" reading was RE-GRADED at the 2026-07-07 port leg: the `y' = 1 − y` flip / murk skip belong to the strip renderers' UNDERWATER-VIEW arg (§The strips — a t2 texcoord flip on all three row vertices, not a reflection mirror; the ×229.5 double is the low tier's), so the reflection's strip geometry rides the RTT scene itself; the camera-mirror transform sits in the Hex-Rays-elided view-matrix section (pin at port). **FIXED (2026-07-07, the REN-6 tail — reimpl planar reflection)**: a SubViewport (same World3D, fixed retail detail-2 `256×256` RTT with square projection and preserved horizontal FOV) with a mirror camera reflected about y = wh feeds the ps.1.1 chain's t2 sampler; the mirror form negates the UP column after reflection (proper det+1 mirror, yaw kept) because the witnessed rows PIN the mapping u = screenU / v = vbase − screenV — the fragment keeps the witnessed texm3x2 dots, then applies the Godot source-to-square projection scale around UV center before sampling. **2026-07-15 view-registration correction:** that scale is (1, source height/source width); without it the main-normalized V coordinate makes reflections swim with view pitch on widescreen displays. The water excludes itself from the mirror scene via a dedicated visual layer (the witnessed prerender pass list draws no water). Sky/terrain/world/celestial mirror by construction (one world), and the dome now anchors from each render pass camera instead of reusing the main-camera transform. Residual in-row: the witnessed wh − 0.1 clip plane is a TRACKED approximation (no oblique near plane in the reimpl; the mirrored camera predominantly sees above-water geometry). The 512 square branch remains documented for a future detail-3/capture selector **2026-07-21 lifecycle/current-camera correction:** the RTT sleeps for absent, unloaded, hidden, or off-screen water and reacquires viewport camera switches; sun/moon/glare/stars relocate from each active pass camera so the mirror no longer inherits main-view anchoring. |
| 31 | The water render LOOK was invented: sin/cos shader waves + Fresnel-style alpha with no witness; retail = per-frame animated 128×128 noise color + DuDv textures over `Env_WaterColorLit` per-vertex color, distance-alpha, murk term | **FIXED 2026-07-06 (minted-and-closed at the water binding slice)**: `water.gdshader` rewritten as a structural port of the witnessed detailed-path model over the libs/env textures (`water_noise_color_pixels`/`water_noise_normal_pixels`/`water_uv_state`, ctest-pinned); the invented waves/fresnel are deleted |
| 32 | Celestial placement inventions: the reimpl placed bodies at `camera.xz + dir × 2000 × (sky_height/175.69)` with ZEROED camera height and a `dir.y > -0.1` visibility gate — none witnessed. The live renderer places at camera + dir × 64 (full height, identity rotation) with alpha folds; the 2000 belongs only to the dome VS proximity ref, the +64/+16 offsets to a dead variant | **FIXED 2026-07-06 (minted-and-closed at the celestial leg)**: placement + witnessed sun/moon alphas ported (`kCelestialBodyDistance`, `celestial_sun/moon_alpha_fixed`, EnvFile statics, vector-pinned); the depth-test flip replaces the invented gate (the world overdraws bodies like retail's draw order) |
| 33 | Star field: retail renders 256 camera-anchored billboard instances with per-star twinkle (rol4/rol11 PRNG) and a hide-near-the-light dot cull `[orig: render_star_field @ 0x5ad9c0]`; the reimpl renders the star 3DI as ONE body. The instance-table generator is unfound | **FIXED (2026-07-06, the REN-6 port leg)**: the generator witnessed (`Star_GenerateInstanceTable @ 0x5ac850`, ex `init_weather_particles` — per-star math in §Celestial bodies; sole caller `EffectWorld_LoadCelestialModels @ 0x5add40`) and PORTED — `env::generate_star_instances`/`star_twinkle_tick`/`star_visible_fixed` (ctest-pinned: the PRNG draws from seed 1, star[0] offsets, the 256-star invariants) hosted by `NovaStarField` + a 256-instance camera-anchored billboard MultiMesh in `nova_celestial.gd` (per-star twinkle via instance color, the 0.98 near-light cull against the active light, regenerate-per-celestial-load, the sky-stars ladder rung). The single-body stand-in and its `0x2000` dead-variant opacity are deleted. Note: the brightness accumulator's exact scale/color application inside `Matrix_BuildTransformFromParts` is Hex-Rays-mangled (x87 handoff) — brightness-as-color-modulation is the structural reading **2026-07-21 reflection correction:** instances are camera-local inside a conservative MultiMesh AABB and the additive shader billboards/reanchors them from each active pass view, preserving far-off mission cameras and the mirror RTT. |
| 34 | Water surface framebuffer blend + far cutoff: the reimpl used standard alpha blending (`blend_mix` — src·α + dst·(1−α), water opaque near / transparent far) and no alpha test; witnessed retail draws the above-water surface with **SrcBlend ONE + DestBlend SRCALPHA** (out = src + dst·α — transparent near, surface-dominant far) `[orig: Water_InitSurfaceShaders @ 0x5c19b0; decode_blend_mode_to_d3d_states @ 0x680f00 mode 11]` | **FIXED (minted-and-closed at REN-4; the alpha-test half RE-GRADED 2026-07-07)**: `water.gdshader` re-expresses the blend exactly via `blend_premul_alpha` with `ALPHA = 1 − a` and stays two-sided (pass flags 0x400000). The REN-4 "alpha-test ref 32 GREATER far-fade cutoff" half was a MISREAD: `CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419/@ 0x5c3484` latches ALPHAFUNC/ALPHAREF only `[orig: @ 0x6770a0]`; D3DRS_ALPHATESTENABLE rides pass-flag bit 0x40000 `[orig: CGfxShader_ApplyPass @ 0x68326b]`, which the water passes (0x30000/0x20000) never set — the misported `discard` amputated the far water (the user-reported short draw distance) and is deleted; the witnessed far fade is the fog convergence, not a cutoff. Residuals live in their own rows: tessellation/murk-angle chain #29, reflection RTT #30 (the underwater OPAQUE swap hosted at the 2026-07-07 facet, see #29) |
| 35 | Water sine LUT provenance: the reimpl built the 256-entry LUT at init from runtime `std::sin`; the original builds once from x87 fsin (`trunc(sin(i·2π/256)·−64)`, stored `0x80 − v` `[orig: Water_InitNoiseFieldAndSineLut @ 0x5c0308..0x5c0334]`) — ONE deterministic instance. Last-ulp libm variance (first seen on the GitHub `macos-26-arm64` runner image, 2026-07-10) flips the truncation at non-landmark indices; the flipped byte survives the LUT landmark+symmetry-sum checks but forks every downstream noise color/DuDv pixel, failing `env_render_unit`'s pinned checksums on that platform only | **Tracked decision (2026-07-10)**: the LUT is a committed 256-byte constant in `water_init_noise_tables` — the deterministic instance every existing ctest/GUT pin was generated from (formula-identical on MSVC/UCRT x64; landmarks `0x80/0xAD/0xBF/0x80/0x41` and the 32768 symmetry sum unchanged). Runtime libm no longer participates, so all platforms render the same witnessed-faithful instance. Whether this instance byte-matches the retail x87 build at every index is unverified (needs a retail memory dump) — the same "pinned-current instance" caveat the noise FIELD already carries via the PRNG call-history quirk (§Water surface, Init tables) |
| 36 | Cloud-map alpha: the reimpl's shared texture resolver decoded the sky-map PCX as opaque RGB (alpha = 1 everywhere), so the cloud pass's alpha chain saturated and the cloud layer fully covered the dome — the pass-1 sky gradient never showed through (found at the 2026-07-18 sniper/aircraft retail A/B: our 08:00/06:32 skies read as all-cloud-ramp) | **FIXED (2026-07-18)**: the sky maps load through the witnessed per-pixel palette-luminance alpha synthesis `A[i] = (85·(r+g+b)) >> 8` `[orig: load_texture_from_archive @ 0x58b980 — table @ 0x58bc35..0x58bca9, per-pixel A @ 0x58bcee]` via `build_pcx_luminance_alpha_texture` + `EnvFile::_load_sky_map_texture*`; non-PCX cloud names keep the generic decode (retail's DDS-first path has no alpha synthesis). See §Sky dome, "Cloud texture load + the synthesized alpha" |

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
- **Closed at REN-4** — the pass-1 sky-gradient stage table: the effect is built
  by `GfxShader_Create1TexModeId(0, 0x20200)` (`[orig: terrain_init_rendering_resources
  @ 0x578aa8]`) — no texture, mode word 0x200 + fog bit — and the mode decoder
  (`decode_mode_color_stage @ 0x681080` case 0x200) emits stage 0
  `COLOROP = SELECTARG2(DIFFUSE)` with alpha `SELECTARG2(TFACTOR)` and blend
  OFF: the dome pass 1 IS the flat oD0 diffuse passthrough the C7 port
  assumed. `sky.gdshader` needs no change.
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
| Weather tick / smoothing / lightning | **matching for the ported scope**: the fixed 62 Hz clock drives the oscillator, both flash sequencers, rain fade, the two-stage iris modulator, all fourteen color blocks, the cloud-scroll tail, and #27's smoothed scalar springs in witnessed order. Fog/skyfog post-processing and every hosted sky/static current now reach their render consumers. Thunder (#15), quake/WAC wind-ring wiring (#18), and the rain/overcast consumers retained by #27 remain deferred. |
| Sky dome render: scroll / VS constants / mesh / advanced_clouds=0 | **matching** (including the six-color `2/255` upload and vs.1.1 color-output saturation; verified against `Color_UnpackToFloat4 @ 0x578900`, `render_skybox @ 0x579080`, and `build_sky_dome_mesh @ 0x578db0`) |
| Sky dome per-fragment combine | **matching** (C7): structural port of the recovered two-pass spec (§Sky dome) folded into one Godot pass; D3D forward clip depth is reconstructed for the proximity dp3 while raster position remains native Godot reverse-Z |
| Celestial + glare | **matching for the ported scope (ENG-2 celestial leg; #33 star field closed at REN-6, 2026-07-06)**: body placement (camera + dir × 64) + witnessed alphas + the #14 occlusion window/hysteresis/glow chain live in `libs/env` behind EnvFile statics + `NovaGlareOcclusion`; the 256-star field generates + twinkles per the witnessed table (`NovaStarField`); the former ENG-3 lo-res-DDA ray residual closed with #209 (2026-07-08) — the glare ray now marches the `libs/terrain_query` port via `NovaTerrainData.raycast_terrain` |
| BMS overrides | **matching** application semantics via EnvFile's non-persistent override layer (runtime apply on load / clear on unload; base file never mutated) |
| iris / terrain_rgb | iris **matching for the ported scope** after #17 and D-RLIT-2: the live modulator is fed by the marched three-point camera-ray average with per-sample indoor/outdoor classification and sun-occlusion rays; the smoothed ÷64 gain reaches shaders. D-RLIT-2 records the bounded entity/light-group geometry residuals. terrain_rgb **matching** after the 2026-07-13 correction to #19: tile overlay HALF×2X and the effects reciprocal are observable; the foliage FULL-tint sample is overwritten before emission, the bake is dead, and the terrain surface is faithfully untinted |
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
| `Foliage_BuildGeometry @ 0x5BF5F0` | stale / do not cite | address resolves inside `build_shader_pass_name @ 0x5bf5d0`; foliage placement anchors are `terrain_update_foliage_tiles @ 0x601F50`, `generate_foliage_instances @ 0x600980`, and `Foliage_SampleFoliageMapMask @ 0x606620` |
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
| G2 iris curve | exact x87 tail shaping | **recovered** — closed-form auto-exposure gain, 64 = identity | §Iris auto-exposure | shipped: `env::iris_gain` + the live `ModulatorChain` (REN-5; divergence #17 FIXED) |
| G3 terrain_rgb consumers | any consumer beyond the effects reciprocal? | **corrected 2026-07-13** — tile overlay is live; texture bake is dead; the foliage sample executes but is overwritten before emission | §iris/terrain_rgb + [foliage-re.md](../foliage/foliage-re.md) | keep tile tint and effects reciprocal; remove invented foliage tint consumer |
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
| 0x610920 | `sub_610920` | `Terrain_PushSkyDomeHeightFloat` | 16.16→float → `SkyDome_SetHeightAndRebuild` |
| 0x8409f4-fc | `flt_8409F4..` | `Render_LightScaleR/G/B` | modulator÷64 shader constant |
| 0x840b24-2c | `flt_840B24..` | `EffectWorld_AmbientScaleR/G/B` | effects/foliage gain |
| 0x7d7338 | `aVs11DclPositio` | `SkyVS_GradientPassSource` | embedded vs_1_1 source, pass 1 |
| 0x7d6fc0 | `aVs11DclPositio_0` | `SkyVS_CloudPassSource` | embedded vs_1_1 source, pass 2 |
| 0x4ed500 | `sub_4ED500` | `Env_TriggerLightningFlashA` | body sets timer A = 16 (old kong comment "font size" wrong) |
| 0x4ed510 | `TextResource_GetStringOrDefault` | `Env_TriggerLightningFlashB` | 2-insn body sets timer B = 32 — prior kong name provably wrong |
| 0x5c7550 | return type `void` → `int` | (type fix) | tail-calls `_ftol2_sse`; fix made Hex-Rays recover the whole iris tail |
| 0x60c670 | `sub_60C670` | `Terrain_RenderSectorBatchLit` | REN-7 dome-band check: `D3DRS_AMBIENT=0xFFFFFF` + `LIGHTING=1` bracket around `render_terrain_sector_batch` — the sky-pass/lo-res terrain leg carries NO below-rim skirt |

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

---

## Appendix: REN-6 witness session (2026-07-06) — IDB edits

The #33 generator hunt, the #27 rain-percent identification, the #29 strip
decode corrections, and the #30 reflection-pipeline internals (all woven into
the sections above). Sanctioned-grill policy; verify-before-edit; `idb_save`
checkpoint taken.

| Addr | Old | New | Evidence |
|---|---|---|---|
| 0x5ac850 | `init_weather_particles` | `Star_GenerateInstanceTable` | writes 256×40 B from `Star_Instances`+16 to `Celestial_SunModel`; per-star PRNG math; sole caller `EffectWorld_LoadCelestialModels @ 0x5add40` |
| 0x5ac010 | (undefined bytes) | `Star_TwinklePrngNext_unused` | the standalone rol4/rol11/xor-1 PRNG step on `Star_TwinklePrng`; zero callers (live sites inline it) |
| 0x26c6880 | `dword_26C6880` | `Env_RainPctCurrent` | the `(d+31)>>5` spring at `@ 0x57ef01`; debug label `"Rain: %i%%"` `[orig: @ 0x4efa91]`; >48 gates weather-particle fall decay `[@ 0x5de916]` |
| 0x26c6884 | `dword_26C6884` | `Env_RainPctTarget` | net-synced (`@ 0x430311`), serialized (`@ 0x4ffae8`), snap (`@ 0x57d2d3`) |
| 0x26c688c / 0x26c6890 | `dword_26C688C/90` | `Env_RainPctStep` / `Env_RainPctMax` | the spring's step/clamp slots |
| 0x5c8510 | `sub_5C8510` (5-byte decompile-fail) | `Water_RenderReflectedWorldScene` | the reflected-world subscene (header call + fall-through body to retn `@ 0x5c8af8`); builds the water clip-plane texture matrix + arms `g_WaterMirrorActive` |
| 0x5c2780 | `sub_5C2780` | `Water_ReflectionPrerender` | packs the live camera block; sole reflection caller of `render_main_scene` |
| 0x60f150 | `sub_60F150` | `PolyTrn_ResetFrameStatsAndRender` | zeroes 7 frame counters -> `PolyTrn_RenderFrame` |
| 0x680080 / 0x6800a0 | `sub_680080/A0` | `GTexRT_SelectThunk` / `GTexRT_RestoreThunk` | the RTT begin/end pair (`GTexRT_Select @ 0x67fa90`) around `Water_ReflectionTexture` |

Plus comments at 0x5c8510/0x5c1240 (the full reflection-pipeline walks),
0x27e2e38 (the star-entry layout), 0x5ac850/0x5ac010. The `sub_58AA80`
NORET mis-flag was cleared (it returns; the flag was truncating every
fall-through caller's analysis).

### IDB edits applied

| Addr | Old | New | Evidence |
|---|---|---|---|
| 0x57e440 | `sub_57E440` | `Environment_ApplyFogAndAmbient` | re-applied C6 walk identity (rename had not stuck) |
| 0x677040 | `sub_677040` | `CD3DDevice_SetActiveFogColor` | sole writer of the FOGCOLOR source value |
| 0x3265718 | `dword_3265718` | `CD3DDevice_PendingFogColor` | read/write pattern in `SetFogAndBlendMode` |
| 0x326579C | `dword_326579C` | `CD3DDevice_AppliedFogColor` | redundant-state cache compare |

Plus comments at 0x578fbb (normal formula), 0x5790d0 (half-height anchor), 0x579b42
(textureless flat pass), 0x677040 (fog color path).
