# .env environments, time-of-day, and the atmosphere stack

How Joint Operations loads `.env` environment files, drives its time-of-day pipeline, and
renders fog, sky, weather, and celestial bodies, as witnessed in the original engine. This is
the reverse-engineering record behind `engine/formats/env` (format + TOD math), the `EnvFile` /
`EnvKeyframe` wrappers, and the `MissionEnvironment` / `SkyDome` / `Water` / `Weather`
/ `Celestial` runtime.

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
| `Environment_InitDefaults @ 0x57c010` | `env::Config` member defaults (`engine/formats/env/env.h`) |
| `TimeOfDay_ParseProperty @ 0x57c590` | `env::load_env` keyword handling (`engine/formats/env/env.cpp`) |
| `Environment_ParseTimeString @ 0x57c500` | `env::hhmm_to_hours_fp` (`engine/formats/env/env.cpp`) |
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
| `Environment_SnapStateToTargets @ 0x57d1e0` | `world::WeatherState::seed` (`engine/runtime/world/weather_state.cpp`) via `env::weather_seed_from_config` (`engine/runtime/environment/weather_seed.h`) |
| `Environment_UpdateWeatherTick @ 0x57e9b0` | ONE home, ONE clock (2026-08-30): the sim legs `world::WeatherState::tick_sim` (`engine/runtime/world/weather_state.cpp`, run by `MissionKernel::tick_weather` after every 62.5 Hz logic tick — the no-net tick, the listen frame, the joiner bridge, the dedicated host) + the color legs `env::WeatherRuntime::tick_render` (`engine/runtime/environment/weather_runtime.h`, the kernel's `world::IWeatherRenderTick` hook implemented by `godot/src/env/weather.cpp`); the math cluster is `env::WeatherCore` (`engine/formats/env/env_weather_core.h`, split `tick_sim_head` / `tick_sim_tail` / `tick_render`) |
| `Environment_InterpolateWeatherColor @ 0x57d9e0` | `env_render` channel smoother |
| `Environment_SetLightningFlash @ 0x57d320` | `env_render` lightning additive injection |
| `Environment_GetFogEndDistance @ 0x57e3e0` | `env_render::compute_fog_params` (end distance) |
| `Environment_ApplyFogAndAmbient @ 0x57e440` | fog application (`engine/runtime/environment/environment_state.h` + `godot/src/env/mission_environment.cpp` + shaders) |
| `Render_SetFogState @ 0x58a950` | `env_render::compute_fog_params` (per-type start) |
| `Render_Skybox @ 0x579080` | `godot/src/env/sky_dome.cpp` (uniforms from `engine/runtime/environment/sky_frame.*`) + `godot/shaders/sky.gdshader` (pass 1 / flat dome) + `godot/shaders/sky_clouds.gdshader` (pass 2, the cloud `next_pass`) |
| `EffectWorld_LoadCelestialModels @ 0x5adc50` | `godot/src/env/celestial.cpp` (+ `engine/runtime/environment/celestial_frame.h`); the bodies draw through their authored ObjectModel materials |
| `Game_LoadTerrainDuringConnect @ 0x520710` | BMS override application (`EnvFile.apply_mission_overrides`) |
| `Game_StartMission @ 0x524360` (0x525371..0x525399) | BMS fog overrides + start TOD |
| `Environment_SetCurrentTime @ 0x57c4b0` / `Environment_SetTodRate @ 0x57c4f0` | runtime TOD state setters |
| `Terrain_SectorComputeLighting @ 0x5c7550` | iris sampler (`env::iris_gain` + the live `ModulatorChain`, REN-5 #17); the entity-lighting writer side is [render/render-lighting-re.md](../render/render-lighting-re.md) |
| `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` | terrain_rgb reciprocal consumer (documented, deferred) |
| `Environment_MissionStartInit @ 0x57f1e0` (ex `sub_57F1E0`) | `world::WeatherState::mission_start_init` + `env::EnvScalarChannels::mission_start_init`; the 255-tick settle = `MissionKernel::complete_mission_start`, reached through `env::WeatherRuntime::run_mission_start_boundary` |
| `WacCmd_Rain @ 0x4edf60` / `Snow @ 0x4edfd0` / `Overcast @ 0x4ee040` / `FogDist @ 0x4ee100` / `MoveFog @ 0x4ee0a0` / `SkySpeed @ 0x4edeb0` / `SkyHeight @ 0x4edec0` / `Quake @ 0x4ed4c0` / `Tod @ 0x4edc70` / `FogType @ 0x4eded0` / `SunFade @ 0x4edf10` / `ColorFade @ 0x4edcb0` / `Sun.. @ 0x4edcd0..` / `Script_SetLightningColor @ 0x4ede20` / `Env_TriggerLightningFlashA/B @ 0x4ed500/0x4ed510` | `world::WeatherState::command_*` through `world::EntityCommands` (the VM, the F3 window and the MCP rows share the one path) — §The WAC weather handlers |
| `NetPacket_WritePlayerState @ 0x4ff6b0` (the 0x0A phase-2 ENV block) / `NapiNPClientMsg_0x00A @ 0x430244..0x43034c` | `replication::connection_fan` narrows the weather home once; `inmatch::JoinerRole::apply_weather_sample` -> `WeatherState::apply_wire_sample` |
| `Precipitation_Reset @ 0x5df3a0` / `Precipitation_SeedPool @ 0x5debb0` / `Precipitation_FallTick @ 0x5de8f0` / `WeatherParticle_UpdatePositions @ 0x5dec40` / `Render_WeatherTrailParticles @ 0x5dee10` / `WeatherParticle_LoadTextures @ 0x5de840` | `env::PrecipitationField` (`engine/runtime/environment/precipitation.h`), `MissionKernel::update_precipitation`, `renderer::compile_precipitation_frame` (`engine/runtime/renderer/precipitation_frame.h`), the `Precipitation` node (`godot/src/env/precipitation.cpp`) copying that frame into the scene overlay stage (`renderer::append_precipitation_overlay`, `engine/runtime/renderer/scene_overlay.h`) — §Precipitation |
| `Sound_PlayTriggerSetScaled @ 0x527b90` (thunder @ 0x57ecfb / @ 0x57edc4) | `world::WeatherSoundEvent` from `MissionKernel::tick_weather` -> `Simulation.drain_weather_sounds` -> `MissionAudio.play_weather_sounds` |
| `Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e` (the rain ambient) | `world::infantry_rain_ambient` (`engine/runtime/world/infantry.cpp`) through the sound-emitter mailbox |
| `Entity_ApplyCollisionForce @ 0x4af4a0` case 3 (the hit blackout arm) | `env::HitDimState::arm`, armed by `world::apply_collision_force` (`engine/runtime/world/collision_force.cpp`, ported 2026-09-14 — local player only, the friendly/enemy split and the in-session NoFriendlyFire suppression; world-wac-ai-re §17.3b) (ex "rain fade" — the kong misnomer; nothing weather-related writes it) |
| `Debug_DrawEnvironmentValues @ 0x4ef000` | the F3 Environment window (`engine/runtime/devtools/environment_window.cpp`, ADR 0039 infrastructure that mimics the CONTENT) — §The environment debug page |

Misnames fixed during the grill: `Render_SetFogParams @ 0x54b4b0` was an entity-pool sweeper
(now `Entity_DestroyUnreferencedPool4Entries`); `Environment_SetCurrentTime`/`Environment_SetTodRate` were "water
reflection"/"ambient G" in kong comments but are the current-time and TOD-rate setters.

## Format truths (parser, `TimeOfDay_ParseProperty @ 0x57c590`)

- **Time is 16.16 fixed-point HOURS, not HHMM.** `Environment_ParseTimeString @ 0x57c500`
  parses positionally from the string tail: `minutes = last two digits` (clamped 59),
  `hours = preceding digits` (clamped 23), returns `(h << 16) + (m << 16) / 60` (integer
  division). Strings shorter than 3 chars yield 0. All keyframe bracketing and interpolation
  happen in this hours space; a day is `0x180000` (24.0).
- **TOD keyframes live in 16 fixed slots of 52 bytes** (`g_EnvTodParseSlots @ 0x26c6070`):
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
  (`g_EnvParseEnvScale @ 0x840950`) is a parse-state float applied to every subsequent `*_rgb`
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
- **`timeofday` maps to an enum** (`g_EnvTimeOfDayEnum @ 0x24d2364`: dawn/day/dusk/night),
  used elsewhere for classification; the gradient never reads it.
- **`vertex_rgb` has no parse case in retail JO** — vestigial from earlier titles. The global
  modulation block it once fed defaults to `0x404040` (1.0 in 6.6 fixed = identity).
- Unknown keywords are ignored. An optional parse hook (`g_EnvParseHook @ 0x26c6058`) can
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
   `g_EnvTodParseCount` is *not* reset between the two parses — so its `tod_begin` blocks
   continue after the `.trn`'s. Stock JO `.trn` files carry no TOD blocks, which is why
   overcast.def is the de facto first table. A NULL mapName jumps straight to the
   overcast.def parse (@ 0x57db98 → 0x57dbf7). The combined set snapshots into
   `g_EnvTrnSnapshotTable @ 0x26c7414` (stable bubble sort by time, count at +832;
   `Environment_SortAndSnapshotKeyframes @ 0x57c240`).
2. slot pointer + keyframe count reset, then `<env>.env` → on parse success (or
   missing/empty env name) the scratch keyframe's colors seed the 12 color blocks'
   parsed targets (@ 0x57dce0 — this is how "naked" color lines outside `tod_begin`
   become static targets) and the table snapshots into `g_EnvEnvSnapshotTable @ 0x26c70d0`.
   An `.env` parse *failure* skips the seeding and snapshot — the previous env table
   persists stale.

The runtime blend (`g_EnvOvercastBlend @ 0x26c6894`, spring-damped toward
`g_EnvOvercastBlendTarget`) interpolates the final TOD colors **between the .env snapshot and
the .trn/overcast snapshot** — overcast weather literally cross-fades to the second table.
Ported 2026-08-30 (#16): `env::blend_tod_states` (the same per-byte lerp with the
63356 snap) inside `EnvironmentState::update_tod` over the `.env` table and the
overcast table (`MissionEnvironment::set_overcast_data`, `overcast.def` loaded from the
resource root — stock `.trn` files carry no TOD blocks), by the overcast blend the
weather tick read BEFORE its spring stepped (`WeatherState::overcast_for_tod_q16`
— retail computes the colors ahead of the overcast spring, `@ 0x57e9c7` vs
`@ 0x57ef62`).

## Time-of-day compute (`Environment_ComputeTimeOfDayColors @ 0x57de40`)

Runs per tick with `curtime + advance` (8.24 hours, wraps at `0x18000000`); no-op when the
.env snapshot is empty. The wrap is a signed positive modulo on the stored word
(`@0x57DE51..0x57DE78`, stored `@0x57DE84`): the WAC `TOD` command stores `arg × 0x44444`
raw into `g_EnvCurTimeFixed24` (`WacCmd_Tod @0x4EDC70`, the stores `@0x4EDC74` / `@0x4EDC7A`), so
`TOD(-60)` becomes 23:00 at the next weather tick. Hardcoded day-phase windows (16.16 hours): sunrise ramp 05:40→06:20
with the sun/moon switch at 06:00, sunset ramp 18:25→19:05 with the switch at 18:45, ramp
width 20 minutes (`21840`); sets `g_EnvIsNightPhase @ 0x26c645c` and
`g_EnvDayPhaseBlend @ 0x26c6460`. Segment lookup (`Environment_FindKeyframeSegment @ 0x57dd80`)
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
it; the terrain byte-pack path keeps the getter tuple unchanged.

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

**Keyframed vs stepping blocks (witnessed 2026-08-30, ported the same day):**
`Environment_ComputeTimeOfDayColors @ 0x57de40`, run at the top of every weather
tick (`@ 0x57e9c7`) while a keyframe table exists (`g_EnvEnvSnapshotCount @ 0x57de8a`),
writes each of the ELEVEN keyframed blocks — light, sky, ground, fog, skyfog, skybase,
skybright, skyhighlight, cloudbase, cloudhighlight, cloudedge — directly: `[4]/[3]/[2]/[5]
= byte << 20` (the 12.20 channels), then `[0]`, `[11]` and `[10]` = the packed blend
(`@ 0x57e078..0x57e3c9`; `[1]` and the additive `[12]` are left alone). The step that
follows therefore finds `current == target` and moves nothing — a keyframed block never
smooths; only its lightning additive and the modulator chain apply on top. The three
statics (ceiling/cloud/floor) and the two modulators are the only blocks that ever step,
which is also why the WAC `sun/sky/ground/fogcolor/skyfogcolor` handlers (`[11]` +
`ColorBlock_SetStepDeltas`) are inert while keyframes exist — the next tick's compute
overwrites them — while `ceiling/cloud/floor/gain` act. Reimpl:
`env::WeatherColorBlock::snap_keyframe` per tick in `env::WeatherRuntime::tick_weather`
(gated on `EnvironmentState::has_tod_keyframes`); the F3 Environment pickers drive only
ceiling/cloud/floor/gain/lightning.

`Environment_InterpolateWeatherColor @ 0x57d9e0` (16 calls per tick): per channel
`step = ((target<<20) - current) >> 3` clamped to ±max-step, accumulate in 12.20, repack with
`+0x80000` rounding; then `[0] = ([0] + additive) × ModulatorBlock × rainFactor` where
`rainFactor = max(0, 0x8000 - Env_RainIntensity)`. Modulator chain: every block multiplies by
the modulator block; the modulator multiplies by modulator2; modulator2 by constant `0x404040`
(identity in 6.6). The witnessed call ORDER (REN-5, decoded from the 16 `mov ecx, imm32`
call sites `@ 0x57ef97..0x57f03c`) is **modulator2 → modulator → light → sky → ground →
fog → skyfog → ceiling → cloud → floor → skybase/bright/highlight →
cloudbase/highlight/edge** — same-tick propagation (ported: `env::ModulatorChain`,
divergence #17). The hosts serve the post-modulator currents through the
`MissionEnvironment` per-tick writeback seam — fill/sun/fog/sky, skyfog, the
static ceiling/cloud/floor trio, and the six sky/cloud dome ramps, along with
the ÷64 gain (#17) and #27 scalars. The 2026-07-21 D-RLIT-1 closure hosts all
fourteen non-modulator color blocks in witnessed order: `cloud_rgb` feeds the
flat dome, while ceiling/floor feed the indoor iris samples
([render/render-lighting-re.md](../render/render-lighting-re.md)).
The modulator's target is the player's **iris auto-exposure sample**
(`Environment_ComputeAmbientLightAlongDirection @ 0x5c7a00`, replicated to gray via ×0x10101 in
`Environment_ApplyFogAndAmbient @ 0x57e533`, chased over 62 ticks via
`ColorBlock_SetStepDeltas @ 0x57d940`) — indoor/under-cover dimming and night-brightening
are the same mechanism; the full curve is in §Iris auto-exposure below.

`Environment_UpdateWeatherTick @ 0x57e9b0`, once per 62.5 Hz simulation tick
(`Game_ProcessMainFrame @ 0x526774`, right after `Entity_UpdateAllEntities
@ 0x52674b`, per drained 16 ms quantum — the reimpl's former separate 62 Hz
weather clock was itself a divergence, closed 2026-08-30: the weather now
rides the simulation tick through `MissionKernel::tick_weather`). The weather
tick is UNGATED: the entity update sits behind the frame gate
(`@ 0x526703..0x526742` — the authority skips it while `g_WacVarTicks` is armed
with no humans, while `dword_A85B64` is set, or in session behind the
spawn-success gate), whereas the weather tick and `Camera_ComputeThirdPersonView
@ 0x526781` (only `dword_A87050 == 0`) run every drained quantum regardless, so
the weather keeps advancing while the entities are held (the reimpl's
`tick_weather` likewise runs after every kernel tick, held entities or not):

- `ComputeTimeOfDayColors(curtime + advance)` — the ELEVEN TOD-keyframed blocks SNAP
  (channels `[2..5]`, `[0]`, `[10]`, `[11]` written outright, `@ 0x57e078..0x57e3c9`;
  see §Color state blocks); only ceiling/cloud/floor, the modulators and the scalars
  actually smooth. The `tod` WAC (`WacCmd_Tod @ 0x4edc70`) stores `minute * 0x44444`
  RAW into `g_EnvCurTimeFixed24`; the wrap into `[0, 24 h)` happens HERE at the next
  tick (`@ 0x57de48..0x57de84`, which stores the wrapped value back), so a minute
  argument >= 1440 reads unwrapped until the following weather tick (nothing reads
  the clock in between: the WAC runs inside the tick's entity half and the 0x0A
  projection after the tick).
- 310-tick "TOD minute" cadence counter.
- Wind PRNG: `r = rol32(r, 9); if (r < 0) r += 0x1ABB09`; `rand = r & 0xFFF`. Wave amplitude
  `(windScale * (15*prev + rand²>>8)) >> 12` into a 256-entry ring (`0xFFFF - 2*amp`,
  clamped ≥ 0) plus a sprung oscillator ring (1/32 + 63/64 damping toward 0x8000).
  **Ring readers (witnessed 2026-08-30 — every path `g_EnvWindScale` reaches the frame):**
  `g_EnvWaveAmpRing[(z >> 15) + (y >> 14) + (x >> 14) + g_EnvWaveRingIndex]` (the low byte)
  is the light flicker sample (`Light_TickGenBlock @ 0x5a8ae0`,
  `Light_SetupTerrainProjectedPass @ 0x5aaa13`, `Light_SetupTerrainProjectedPassPS
  @ 0x5aadf6` — each writes it into the global CTRL `FLICKER` slot `0x83FD00` before its
  RgbGen evaluation) and, through `HUD_CacheEntityDisplayInfo @ 0x4a3d9e..0x4a3dd1`,
  the global CTRL `FLICKER` (ordinal 3, `0x83FD00`) and `SWING` (ordinal 4,
  `0x83FD08` = `g_EnvWaveOscRing[same]`) registers: the HUD and viewmodel legs
  (`HUD_RenderAllOverlays @ 0x5a8341`, `Player_RenderFirstPersonViewModel @ 0x4dee8b`)
  hash the LOCAL PLAYER, while the gnrc/Sway world bone callbacks
  (`BoneCallback_gnrc_World @ 0x4e286c`, `BoneCallback_Sway_World @ 0x4e2b22`) hash
  the RENDERED ENTITY right before its batch snapshots the registers
  (`Render_CollectRenderBatchesForEntity @ 0x5d968d` copies `dword_83FCE8[ordinal*8]`),
  so a world model with those callbacks swings/flickers on its own position;
  `g_EnvWaveOscRing[0]` — the ring's slot 0,
  refreshed once per 256 ticks — is the weather term of the DETAIL foliage sway phase,
  `c24.x = GetTickCount() * 0.003 + g_EnvWaveOscRing[0] / 655360` (`flt_7DE9D0` =
  `0x35CCCCCD`, the float nearest 1/655360, not 1/65536), with the 0.03 bend scale on
  `c25.x` (`flt_7C9B90`), not `c24.w`
  (`Foliage_SetupVertexShaderConstants @ 0x60074a..0x6007b4`;
  [foliage-re.md](../foliage/foliage-re.md)). Reimpl: `env::WeatherOscillator::ring_slot`
  is the shared hash; the light flicker (`renderer::light_flicker_value`) reads the
  ONE weather home's rings, the `Weather` node publishes the local player's pair as
  the frame default plus a ring copy (`ObjectData::set_weather_ctrl_registers` /
  `set_weather_rings`) and every `ObjectModel` whose 3DI declares FLICKER or SWING
  hashes its own position into its CTRL dictionary each frame (the bone-callback
  identity itself is not modeled: any declaring model hashes), and the
  `FoliageDispatcher` feeds the ms clock + `osc_ring[0]` into
  `renderer::FoliageFrameCompiler`. The former `wind_sway_amount/phase` shader globals
  (an unwitnessed normalization nothing consumed) are gone; `sway_amount/sway_phase`
  remain as diagnostics of the same ring state.
- Earthquake jitter while `g_EnvQuakeTicks` counts down (ported 2026-08-30 as
  `WeatherState::apply_quake_jitter`): pool-0 entities with an item (`ItemTypeIndex`),
  not airborne (`Flags & 0x2000` clear), no parent — `X += (int16)r >> 6`,
  `Y += 4 * (int8)r`, `bodyHeading += (16 * r) >> 7` with the PRNG re-rolled per
  displaced entity (`@ 0x57eb48..0x57eb9e`); the local player's displacement
  HARD-SETS the camera shake counter to 32 (`dword_B764B0`, now
  `g_CameraShakeCounter`, `@ 0x57eb7d`; the counter decays in
  `Player_UpdatePerFrame @ 0x4de590` from the client frame that precedes the
  entity update — once per quantum, never per rendered frame — and
  `Camera_ComputeThirdPersonView` samples it, advancing the three IIR filters,
  once per drained quantum right after this tick (`@ 0x526781`) AND once per
  rendered frame from the scene frame (`Render_ProcessMainSceneFrame
  @ 0x5ca34d`; a third time from the weapon Inset pass `Render_WeaponInsetScene
  @ 0x5c9740`, the call `@ 0x5c9841`, when the Inset draws), each call re-deriving the view so the frame's
  own sample renders: ported 2026-08-30 as the pre-tick decay, the per-quantum
  compose (`local_player_camera_compose` from `LocalPlayer::tick_view`) and the
  per-frame compose (`local_player_view_frame` from
  `LocalPlayer::present_view_frame`); since 2026-09-22 those are the only
  composes, and every other reader observes the last composed view. The
  compose's mode>=1 leg applies a SECOND,
  stateless shake instead of the IIR filters — `amp = (min(4*counter, 255) *
  ((prng & 0xFF) + 64)) >> 8`, `yaw += trunc(sin(C*0.4)*amp)` (on the BAM
  heading, `@ 0x43898B`, so the mission yaw subtracts it; the mode-0 IIR
  `>> 6` yaw applies the same way, `@ 0x4380D9`), `pitch +=
  trunc(sin(C*2/7)*amp) - (trunc(sin(T*25/34)*amp) >> 2)`, `roll =
  trunc(sin(C*2/11)*amp) - (trunc(cos(T*0.862069)*amp) >> 2)` over the
  look-at's zero roll (`@ 0x438939..0x4389e5`; the mode-4 lerp then overwrites
  the rotation, so only the chase renders it) — ported 2026-08-30 (tidy round)
  as `camera_shake_sample_chase`, applied by `local_player_camera_compose` in
  mode 1; the Inset overlay's extra call is a full second compose (its own
  shake step and, mounted, its own look-ahead step) whose view `g_view_*` keeps,
  the Inset offsetting a copy (`@0x5C9846..0x5C9903`; `@0x5C9841`; D-HUD-26
  FIXED 2026-09-19);
  pool-1 entities whose def carries
  `attrib & 0x40` likewise (`@ 0x57ebde..0x57ec29`, the shake when the player's
  parent is displaced); `--g_EnvQuakeTicks` (`@ 0x57ec61`). Then the HIT BLACKOUT
  fade (`g_EnvHitDimIntensity -= g_EnvHitDimFadeRate`, floor 0, `@ 0x57eaf9`) — the
  kong "rain fade" misnomer: `Entity_ApplyCollisionForce @ 0x4af4a0` case 3 is
  its only writer (self/friendly hit `@ 0x4af764`: intensity 0xA000, rate
  `0x8000 / ((124 * mult) >> 2)`; enemy hit `@ 0x4af724`: rate
  `(mult << 13) / ((310 * mult) >> 2)`; mult 3 when the source is within 90 deg
  of the yaw, else 4), consumed per color block as `0x8000 - intensity`
  (`env::HitDimState`, `hit_dim_factor`).
- **Two lightning sequencers** (`Environment_SetLightningFlash @ 0x57d320` scales
  `lightning_rgb` into the additive slots — sky >>8, fog/skyfog >>9, ground >>10):
  timer A at ticks 10/6/4/2 → flash 200/255/200/255, at 0 → flash 0 + thunder; timer B at
  31/28/26/24/23/22/20 → 200/150/200/150/100/50/0, at 0 → second thunder. **Thunder wiring
  (C6)**: epoch 0 calls `Sound_PlayTriggerSetScaled (ex sub_527B90) → SoundBank_PlayTriggerEntries @ 0x75ccd0` on the bank
  at `dword_24E0914` with the 24-byte emitter `{pitch 0x10000, bearing,
  g_SoundVolumeOption, 0, distance 16.16, 0}` (`Sound_PlayTriggerSetScaled @ 0x527b90`:
  its second argument is the DISTANCE `emitter[4]`, its third the BEARING `emitter[1]`
  — the pan `SoundBank_PlayTriggerEntries @ 0x75ce5b` takes for a positional layer;
  the IDB's old `trigger_id`/`position` argument names were stale, corrected
  2026-08-30) — sequencer A plays the set **1 m** from the listener at bearing **0**
  (`0x10000`, `0`, @ 0x57ecfb), sequencer B **10 m** behind at bearing **128**
  (`0xA0000`, `0x80`, @ 0x57edc4), gated on `g_NapiNPCtx.is_mp_session_peer`. **Starters**: the net text command **`SETFLASH1 [n]`**
  (`NapiNPClientMsg_HandleTextCommand @ 0x429ec9`: timer A = atol(arg), default 16; the
  host applies locally and sprintf-broadcasts `"SETFLASH1 16"` @ 0x4d2b6a) — one
  retail trigger path. **2026-08-30 correction:** `Env_TriggerLightningFlashA
  @ 0x4ed500` (= 16) and `Env_TriggerLightningFlashB @ 0x4ed510` (= 32) are the
  WAC `flash` / `farflash` command handlers (the command table's `0x4ED500` /
  `0x4ED510` rows), so sequencer B IS reachable from a mission script; no
  `SETFLASH2` net command exists. Both land as `WeatherState::command_flash` /
  `command_far_flash`; the thunder epochs surface as `WeatherTickEvents`
  (`thunder_a` = 1 m centred, `thunder_b` = 10 m from behind, bearing 128) that
  the kernel turns into `world::WeatherSoundEvent`s for the audio owner (the
  THUNDER trigger set through `MissionAudio.play_weather_sounds`). Rain/wind
  carry no `.env` keywords (parser keyword set enumerated in the 2026-06-09
  grill) — weather intensity is command/WAC-driven, not `.env`-tunable.
- Scalar smoothers (the full set pinned at REN-6, 2026-07-06): spring-dampers
  `(d + 31) >> 5` with per-channel step and absolute clamps — fog distance
  (`g_EnvFogDistCurrent ← g_EnvFogDistTarget @ 0x57ede2`), camera FOV (`@ 0x57ee78`),
  the sun-dim percent (`Env_SunDimPct*` family `0x26c6830..`, default 0, no parser
  writes), **the rain percent** (`g_EnvRainPctCurrent/Target/Step/Max
  @ 0x26c6880/84/8C/90 @ 0x57ef01..0x57ef4c` — identified at REN-6: the debug
  overlay labels it `"Rain: %i%%"` `[orig: Debug_DrawEnvironmentValues
  @ 0x4efa91]`; the target is net-synced (`NapiNPClientMsg_0x00A @ 0x430311`;
  serialized at `NetPacket_WritePlayerState @ 0x4ffae8`) and mission-start-snapped
  (`@ 0x57d2d3`); current > 48 drives weather-particle vertical fall decay
  `[orig: @ 0x5de916]` and weather-particle render/update paths read it
  `[@ 0x5dec81; @ 0x5dee27]`), and the overcast blend (`@ 0x57ef4c..0x57ef92`);
  eighth-snaps `(d + 7) >> 3` with overshoot snap — sky height (`@ 0x57ee97`) and
  the cloud scroll rate (`@ 0x57eecc`).
- Derived render colors: `g_EnvTerrainLightCombined` per lane =
  `min(light × 0xB5 + sky × 257, 0xFFFF) >> 8` (0xB5 ≈ 0.707): the sky byte widens by
  `punpcklbw mm1, mm1` (× 257), the light byte zero-extends (`punpcklbw mm0, mm2`) and
  multiplies by the weight, the two add with `paddusw` and shift
  (`@ 0x57f0a4..0x57f0d5`; the `0x5A` (0.35) variant `@ 0x57f117..0x57f148`), so
  light = sky = 100 gives 171, not 170 (ported exactly 2026-09-24, "Dim the sky dome under
  NVG, whiten it under thermal, exact light blend": `combine_terrain_light` in
  `engine/formats/env/env_render.cpp`); `g_EnvCeilingFloorBlend = ceiling*0.707 + floor*0.707`,
  `g_EnvWaterColorLit = water * combined >> 7` (×2 gain), and **fog + skyfog render colors are
  doubled with saturation** — `.env` fog colors are authored at half intensity.
  These blocks and derived colors stay RAW under NVG: the NVG hemisphere rewrite is applied
  only to the per-pass object lighting block
  (`CTerrainRenderer_BuildLightingShaderConstants @ 0x5c820b..0x5c82e9`) and to a stack copy
  of the terrain sky argument (`Render_TerrainScene @ 0x610d16..0x610e36`), so the water
  colour, the underwater fog and clear, the particle ambient, the scars and the slot drape
  are NVG-invariant (`g_EnvTerrainLightCombined @ 0x57f0d5` and `g_EnvWaterColorLit @ 0x57f177`
  read the raw blocks). Ported 2026-09-24 ("Keep the env colour blocks raw under NVG;
  rebuild the terrain NVG sky as bytes"): the `EnvironmentState` colour getters serve the raw
  blocks and `build_light_values` / `nvg_terrain_sky` apply the rewrite
  ([render/render-lighting-re.md](../render/render-lighting-re.md)).
- **Horizon blend (witnessed 2026-07-05, closes the previously uncited one-liner):** after
  the 16 channel smoothers and **before** the fog/skyfog doubling, when the smoothed
  `g_EnvFogDistCurrent @ 0x26c681c` is (unsigned) strictly below `g_EnvFogDistReference/2`
  the skyfog render color is overwritten **in place** with `fog*(1-t) + skyfog*t`,
  `t = ((dist - ref/4) << 16)/(ref/4)` clamped to 0 below `ref/4` — pure fog at <= ref/4,
  a linear fade across [ref/4, ref/2], untouched skyfog above (MMX: bytes x0x101 >> 1,
  pmulhw against t/~t >> 1, paddsw, >> 6, packuswb)
  `[orig: Environment_UpdateWeatherTick @ 0x57e9b0, blend @ 0x57f037-0x57f0a1]`; the
  doubling then applies to the post-blend value `[@ 0x57f1b1]`. `g_EnvFogDistReference
  @ 0x26c68a8` (16.16): default 1024.0 `[orig: Environment_InitDefaults @ 0x57c0b0]`,
  re-set at terrain init to 768.0/1024.0 by adapter-caps bit 0x40 and forced to 1024.0
  on the session authority `[orig: Terrain_Init @ 0x60fc9a/0x60fca3]`. The witnessed
  consumer is the **frame clear**: every scene entry clears with `is_alternate_fog ?
  0x808080 : camera strictly above water ? skyfog[0] : g_EnvWaterColorLit` (the `jle`
  `@ 0x5ca790` gives the lit water at exact equality; no blink letter reaches this clear)
  via CD3DDevice
  SetClearColor/SetClearDepth(1-2^-15) -> `IDirect3DDevice9::Clear(TARGET|ZBUFFER)`
  `[orig: Render_ProcessMainSceneFrame @ 0x5ca776-0x5ca7bf; NVG_RenderSceneToTarget
  @ 0x5d065a-0x5d0699; device Clear @ 0x677100 (IDB-misnamed CGfxTextOverlay_Draw)]`,
  the Clear halving the color `(c>>1)&0x7F7F7F7F` on non-modulate2x devices
  `[@ 0x67715d; cap dword_32656AC, cf. RenderState_DecodeModeColorStage @ 0x681080]`; the
  sky-dome pass sets only the device FOG color, to skyfog while drawing the dome,
  and restores fog for the world `[orig: SkyDome_RenderWithSkyfog @ 0x579cb0]`; the
  clear itself is the scene entry's `[orig: Render_ProcessMainSceneFrame
  @ 0x5CA771..0x5CA7BF]`. Reimpl: tick and horizon-blend in undoubled block space
  using the smoothed (pre-overcast) fog distance, then double with saturation at the
  render tail. `GameWorld::update_frame_clear_color` writes the selected clear
  (thermal grey, skyfog above water, the lit water at or below it; the NVG scene
  clears to the fog colour alone) to the GameWorld `ClearColor`, and
  `get_skyfog_color()` feeds `SkyDome`'s dome fog; the dome no longer mirrors the
  clear. One final skyfog value on both paths preserves the invisible
  dome-rim/clear seam of the modulate2x path.
- Cloud scroll: four accumulators advance by the smoothed rate × (1, 1, 2/3, 4/3).

`Environment_SnapStateToTargets @ 0x57d1e0` (mission start) copies every parsed target [10]
into the active target [11], snaps fog distance/sky height/scroll rate/overcast targets from
the parsed values, zeroes quake/lightning/rain, and seeds the weather PRNG with a constant —
weather is deterministic per mission start.

## The WAC weather handlers (witnessed 2026-08-30, ported byte-faithful)

Every handler writes the weather globals directly; the reimpl routes each
through `world::EntityCommands` into `world::WeatherState::command_*` (the VM,
the F3 Environment window and the MCP `environment_*` rows share that one
path). Seconds multiply by 62 (`imul 62`; 0 -> 1 tick); a timed step is
`|target + ticks/2 - current| / ticks`.

| WAC | Handler | Writes |
|---|---|---|
| `rain(pct, s)` / `snow(pct, s)` | `WacCmd_Rain @ 0x4edf60` / `WacCmd_Snow @ 0x4edfd0` (both through `Env_SetPrecipitationKind @ 0x5de8e0`, the calls @ 0x4edfbb / @ 0x4ee02b) | `g_EnvRainPctTarget = min((pct << 16) / 100, 0x10000)`, `g_EnvRainPctStep` = the timed step, `g_EnvPrecipitationKind @ 0x2c059d0` = 0 / 1 |
| `overcast(pct, s)` | `WacCmd_Overcast @ 0x4ee040` | the same on `g_EnvOvercastBlendTarget @ 0x26c6898` / step `@ 0x26c68a0` |
| `fogdist(d)` | `WacCmd_FogDist @ 0x4ee100` | `g_EnvFogDistTarget = clamp(d << 16, 2 m, g_EnvFogDistReference)`, `g_EnvFogDistAccelClamp = |target - current|` |
| `movefog(d, s)` | `WacCmd_MoveFog @ 0x4ee0a0` | the same target, the timed step as the accel clamp |
| `skyspeed(n)` | `WacCmd_SkySpeed @ 0x4edeb0` | `g_EnvCloudScrollRateTarget = n << 10` |
| `skyheight(n)` | `WacCmd_SkyHeight @ 0x4edec0` | `Env_SkyHeightTarget = n` (the RAW parameter — no `<< 16`) |
| `quake(n)` | `WacCmd_Quake @ 0x4ed4c0` | `g_EnvQuakeTicks = 6 * n` |
| `TOD(hh:mm)` | `WacCmd_Tod @ 0x4edc70` | `g_EnvCurTimeFixed24 = minute_of_day * 0x44444` |
| `fogtype(n)` | `WacCmd_FogType @ 0x4eded0` | `g_EnvFogType = n` (+ an immediate `Render_SetFogState`) |
| `sunfade(pct, s)` | `WacCmd_SunFade @ 0x4edf10` | the sun-dim channel: target `min(pct << 16, 0x640000)`, the timed step — INERT in retail: the channel's max clamp `@ 0x26c6840` has no writer, so the current never leaves 0 |
| `colorfade(s)` | `WacCmd_ColorFade @ 0x4edcb0` | `Env_ColorFadeTicks @ 0xc60dd0 = 62 * s` (ex `frameCount`; 0 at WAC init) |
| `sun/sky/ground/floor/ceiling/cloud/fogcolor/skyfogcolor(r, g, b)` | `WacCmd_Sun @ 0x4edcd0`, `Sky @ 0x4edd00`, `Ground @ 0x4edd60`, `Floor @ 0x4eddf0`, `Ceiling @ 0x4eddc0`, `Cloud @ 0x4edd90`, `FogColor @ 0x4ede40`, `SkyFogColor @ 0x4ede70` | the block's ACTIVE target `[11]` = the packed rgb + `ColorBlock_SetStepDeltas(block, Env_ColorFadeTicks)`; the TOD-keyframed blocks are overwritten by the next `ComputeTimeOfDayColors` (so `sun/sky/ground/fogcolor/skyfogcolor` are INERT while keyframes exist), the statics (ceiling/cloud/floor) keep the value. Every numeric WAC weather argument lands RAW: no handler takes an absolute value or clamps a negative (`quake` stores `6 * n` into the unsigned countdown, `colorfade` `62 * n` into the step divisor, the timed commands `62 * n` ticks with only the zero -> 1 guard, so a negative count installs a negative step — ported 2026-08-30) |
| `gain(r, g, b)` | `WacCmd_Gain @ 0x4edd30` | the modulator block target + step deltas (re-targeted every render pass by the iris) |
| `lightning(r, g, b)` | `Script_SetLightningColor @ 0x4ede20` | `g_EnvLightningColor @ 0x26c646c` |
| `flash` / `farflash` | `Env_TriggerLightningFlashA @ 0x4ed500` / `B @ 0x4ed510` | timer A = 16 / timer B = 32 |
| `ammorain(ammo)` / `fxrain(fx)` | `@ 0x4ee1a0` / `@ 0x4ee3e0` | the ammo effect at player + (`25 * ((rng << 8) >> 15)`, `50 * (int16)rng`, `z + 10 * ((rng & ~0xFF) + 0x30000)`) with the WAC RNG; the emitter at player + (`6 * hi16(rng)`, `6 * lo16(rng)`, `z + 8 m`) — effect spawns, not weather state (the VM's effect legs) |
| named values `wind` / `night` | the table `@ 0x82EEF0` | `g_EnvWindScale @ 0x26c68c0` (writable) / `g_EnvIsNightPhase @ 0x26c645c` (`Builtin::Wind` / `Builtin::Night`) |

## Environment_MissionStartInit (`@ 0x57f1e0`, ex `sub_57F1E0`)

From `Game_StartMission @ 0x525cb8`, after the eager WAC execution and before
play: every color block's current <- active target with the per-channel step
`0x2800000`; the FOV / sun-dim / cloud-scroll-rate / sky-height / fog-distance
currents <- their targets; `g_EnvRainPctMax = Env_OvercastMax = 0xFFFF`,
`g_EnvRainPctStep = Env_OvercastStep = 0x1000`, `g_EnvFogDistAccelClamp =
0xFF0000`, `g_EnvFogDistMax = 1000 << 16` (`@ 0x57f7d8..0x57f873`); then 255
complete weather ticks (`@ 0x57f878..0x57f880`). Both roles run it (the shared
client/host path); only the WAC execution before it is the authority's.
Reimpl: `WeatherState::mission_start_init` + `MissionKernel::
settle_weather_mission_start` (the Godot boundary `Weather.run_mission_start_boundary`
seeds the World's weather from the loaded .env + the BMS clock through
`env::weather_seed_from_config` first — the ONE derivation the dedicated host
also runs). The `EnvScalarChannels` defaults ARE these running clamps: retail's
zero BSS maxes exist only between the load-time snap and the initializer, a
window no weather tick runs in.

## Precipitation — rain / snow drops (witnessed 2026-08-30, ported)

- **Pool**: 3072 slots x `{x, y, z, floor_z}` 16.16 at `g_EnvPrecipitationTable
  @ 0x2bf99d0` (the walkers hold the y column, `unk_2BF99D4`).
  `Precipitation_SeedPool @ 0x5debb0`: `x, y = PRNG_Next16_B() << 5` (0..32 m),
  `z = << 4` (0..16 m); `Precipitation_Reset @ 0x5df3a0` (from `Game_StartMission
  @ 0x5249d4`): memset the table + the trailing globals, seed, kind = rain. The B
  stream (`PRNG_Next16_B @ 0x6130f0` over `dword_31BFBBC`, `rol4(s + rol11(s)) ^ 1`)
  is seeded `0x5ADEADA5` by `Game_StartMission @ 0x52460b`. Retail's drops never
  spawn — they recycle through the volume forever.
- **Kind**: `g_EnvPrecipitationKind @ 0x2c059d0` (0 rain, 1 snow) from
  `WacCmd_Rain`/`Snow` and the 0x0A ENV block; read by the drawer, the fall tick
  and the rain-sound leg.
- **Fall tick** (`Precipitation_FallTick @ 0x5de8f0`, per logic tick from
  `Entity_UpdateAllEntities @ 0x4c2214` — between the pool-1 walk and
  `DeathPiece_TickAll @ 0x4c221c`, i.e. inside the ENTITY update and behind its
  frame gate: the 255-tick mission-start settle and a held entity update never
  fall a drop; ported 2026-08-30 into `World::run_logic_tick` on gameplay ticks,
  every peer falling its own pool): its "wind origin" writes
  (`0x2c059f8..0x2c05a00`) have no readers (dead); while `g_EnvRainPctCurrent > 48`
  every slot `z -= 12288` (rain) / `2048` (snow) and `g_EnvPrecipitationFallAccumZ
  @ 0x2c05a2c` accumulates the same.
- **Render update** (`WeatherParticle_UpdatePositions @ 0x5dec40`, once per
  render frame from the drawer): active = `clamp((3072 * RainPct + 0x8000) >> 16,
  > 1, 3072)`; per slot wrap x, y into `[cam - 4 m, cam + 28 m)` with
  `b ^ ((b ^ p) & 0x1FFFFF)` (+0x200000 if < b) and z into `[cam - 8, cam + 8)`
  with mask 0xFFFFF; on any wrap the floor = `max(Terrain_SampleHeightBilinear
  @ 0x6067b0, g_EnvWaterHeightFixed)`, then a ray from floor + 200 m
  (`0xC80000`) down to the floor (`Physics_RaycastIntContext @ 0x5385e0` +
  `Physics_RaycastProximityEntities @ 0x538350`) lifts the floor to the hit z.
- **Drawer** (`Render_WeatherTrailParticles @ 0x5dee10`, from
  `Terrain_RenderWorldScene @ 0x5c96a6` — after particle pass B
  (`@ 0x5c9690`) and the NVG laser beams (`Render_NVGLaserBeamsForVisiblePersons @ 0x5c9695`), before the coronas
  (`EffectWorld_RenderLightCoronas(1) @ 0x5c96ad`), the water glint and the murk quad;
  the earlier "projectile trails" / "foliage billboards" labels for those two
  neighbours were misreadings, corrected 2026-09-24 in
  [render/render-order-re.md](../render/render-order-re.md); the mirror never draws
  the streaks):
  only if `RainPct > 48` (below it the drawer returns before the pool update
  and its memory, `@ 0x5dee48`); velocity = the camera delta since the drawer's
  last call, measured only while `g_CameraMode` equals that call's
  (`dword_2C05A14`, `@ 0x5dee74..0x5dee7a`), else zero (clamped 0.2); that
  memory is zero-initialized data only the drawer writes
  (`Render_WeatherTrailParticles @ 0x5DEEB4..0x5DEED8`) and never reset, so it
  spans every scene pass (every pass calls the drawer at its own camera: the
  main scene, then the weapon Inset pass `@ 0x5c9de9`) and every mission load
  (the port carries it in `MissionKernel::carry_across_load_from`); fall = the
  accumulator (clamped 0.1) then zeroed; snow: trail =
  `cam_up * 0.05`, right `*= 0.025`; rain: trail = `(0, 0.1, 0) + velocity -
  fall`, right `*= 0.01`, width `1 + dist * 0.1`, length `1 + dist * 0.05`; per
  slot with `z >= floor`: color `g_EnvTerrainLightCombined | 0xFF000000`, one
  triangle (`pos + trail * len` uv .5/0; `pos - right * w` uv 0/1; `pos + right
  * w` uv 1/1); 768-vertex TRIANGLELIST batches; identity world matrix; textures
  `eraindrp.tga` / `jsnwflk.tga` (`WeatherParticle_LoadTextures @ 0x5de840` from
  `Render_InitMissionTextures @ 0x587120`); one-texture mode word 0x651
  (SRCALPHA/INVSRCALPHA, alpha MODULATE, color MODULATE2X) with pass flags
  0x10500000 (lighting off, z-write off, cull none, no fog, no alpha test).
- **Rain ambient** (`Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e`, the
  local player, on the frame's last 16 ms quantum): if `RainPctCurrent != 0`, the
  set handles are loaded (`dword_24E0E80`) and kind == rain: volume =
  `RainPctCurrent` (<= 0xFFFF), scaled `(lightTransfer * 0.5 + 0.5) *` when the
  FIRST BLINK HIT (`entity+0x1D0`, `>> 20` = the pool-2 index) is non-zero — that
  hit alone gates it, there is no indoor-flag test (`@ 0x4b4770..0x4b47a8`,
  ItemDef+0x218) — and the word's low 16 bits are the 8.8 volume (`mov word ptr
  [..], bx` @ 0x4b4845; a high byte of 0 is the registrar's unregister); registers
  `LPNV_RAIN_L` at `(x + 2 m, y, z + eyeOffsetZ)` slot type 1 and `LPNV_RAIN_R` at
  `(x - 2 m, y, z + eyeOffsetZ)` slot type 2 (the entity's +0x74 added to Z,
  `@ 0x4b47b0` / `@ 0x4b4865`), lifetime 20, pitch 0x10000, via
  `SoundEmitter_RegisterSetLayers @ 0x528340` (set handles `dword_24E0918/1C`,
  the resolver table `@ 0x82F590`).
- **Reimpl**: `env::PrecipitationField` (seed / fall_tick / update over
  `PrecipitationFloorSampler` callbacks; the kernel supplies the terrain field
  and `CollisionWorld::clip_segment_to_nearest_collision` through the local
  player's candidate slice), `renderer::compile_precipitation_frame` (the
  triangles in the Godot frame), the `Precipitation` node, which since 2026-09-24
  ("Draw the scene's post-particle tail in its own overlay stage") owns no mesh or
  shader: it copies the compiled triangles into the scene overlay stage
  (`renderer::append_precipitation_overlay`, `engine/runtime/renderer/scene_overlay.h`:
  MODULATE2X colour, SRCALPHA / INVSRCALPHA, z-test without z-write, the one diffuse
  `g_EnvTerrainLightCombined | 0xFF000000`, mode word 0x651, pass flags 0x10500000), drawn
  in scene views only, never in the water mirror (`precipitation.gdshader` is deleted),
  `world::infantry_rain_ambient` through the sound-emitter
  mailbox. ctests `precipitation`, `renderer_precipitation_frame`,
  `weather_state`.

## The environment debug page (`Debug_DrawEnvironmentValues @ 0x4ef000`)

The retail F3 environment page — 0xc1b bytes, zero xrefs like the rest of the
`Debug_Draw*` family (the page dispatcher is stripped from the retail image;
mimic the CONTENT, not the wiring). Title `"Script & Env Values"`; rows in two
columns (x 10 / x 200), each row's y scrolled by `dword_A895BC`:
`Env: %s` (`g_BmsEnvironmentName @ 0xa762ac`) / `Trn: %s` (`g_BmsMapBaseName
@ 0xa76214`); `Loc: %i` (`dword_B763E8`, unidentified) / `Reverb: %i`
(`sub_766470`); `Blink: %s` (`"-----"` with V/S/W/L/O for
`g_LocalPlayerBlinkFlags` bits 2/4/8/0x10/0x20); `Fogtype: %i`; `Fogdist: %im`
(`g_EnvFogDistCurrent` hi word); `ColorFade: %i seconds` (`(Env_ColorFadeTicks +
31) / 62`); `SunFade: %i%%` (`g_EnvSunDimPctCurrent` hi word); `MoonLight: %i`
(`g_EnvIsNightPhase`); `Fog`, `SkyFog`, `Cloud`, `Sun`, `Lightning`, `Sky`,
`Ground`, `Ceiling`, `Floor` as `(%i, %i, %i)` from the block `[0]` currents;
`FOV: %i degrees` (`dword_26C6844` hi word); `SkyHeight: %im`; `SkySpeed: %im`
(`g_EnvCloudScrollRate >> 10`); `OutDoor` (`g_EnvTerrainLightCombined`);
`InDoor` (`g_EnvCeilingFloorBlend`); `Gain` (`g_EnvModulatorBlock`); `Iris`
(`g_EnvModulator2Block`); `Rain: %i%%` (`(100 * g_EnvRainPctCurrent) >> 16`);
`Overcast: %i%%`; `Complexity: %i` (`g_ProxCandidateArenaUsed @ 0xb57c88`);
`DCB: %i` (the literal 692). Reimpl: the F3 `Environment` window (ADR 0039 —
tool chrome is infrastructure; every VALUE it shows is engine state read from
the weather home through `Simulation::native_environment_snapshot`), the same
rows plus swatches (the rows a WAC handler targets open a picker driving
that handler: sun/sky/ground/floor/ceiling/cloud/fogcolor/skyfogcolor/gain
and `lightning`; outdoor/indoor/iris are derived and read-only) and a
control strip of the WAC handlers above; `Loc` is
omitted (unidentified) and `Reverb` awaits the unported reverb bed
(docs/audio); the MCP `environment_*` debug rows drive the same commands.
The strip's `TOD` control is the WAC `tod` handler (minute × 0x44444, so a
9:00 scrub lands at 8:59.9993 exactly as retail's script would); the tool
scrub behind `debug_set_mission_minute_of_day` (the MCP row, GameWorld's
debug seam) is the exact minute on the 8.24 clock
(`WeatherState::debug_set_time_of_day_minutes`) — a tool convenience, not a
witnessed handler.

## Environment_ApplyFogAndAmbient walk (C6)

`Environment_ApplyFogAndAmbient @ 0x57e440` is the per-**pass** environment push, called from
every scene renderer (`Render_MainScene @ 0x5c164c`, `NVG_RenderSceneToTarget @ 0x5d07f6`,
`Render_ProcessMainSceneFrame @ 0x5ca3ce/0x5ca841`, `Terrain_RenderWorldScene
@ 0x5c949f`, the weapon Inset pass `Render_WeaponInsetScene` `@ 0x5c98b6`, `Render_CinematicMultiview @ 0x570adb`)
with `(is_underwater, is_alternate_fog)`; `is_underwater = g_ViewPosZ <
g_EnvWaterHeightFixed` at most sites. The Inset applies `(0, its eye below the
water)` for its scene (`Render_WeaponInsetScene @ 0x5c9d41..0x5c9d4f`), never
the thermal grey, and its scene core re-applies it with the pass's zero second
argument; ported 2026-09-26 as `EnvironmentState::build_inset_scene_fog`.
Complete ordered walk:

| # | Site | Action |
|---|---|---|
| 1 | @ 0x57e44c | `end = Environment_GetFogEndDistance(is_underwater)` (murk-derived when underwater) |
| 2 | @ 0x57e458 | `Render_UnpackModulatorToLightScale(modulator[0])` → `Render_LightScaleRGB @ 0x8409f4..fc` = modulator color ÷ 64 — **the application point of the iris auto-exposure**: these floats are a shader constant (consumed in `Material_ApplyShaderParameters @ 0x58e05d`); 64 = 1.0 gain |
| 3 | @ 0x57e464 | `EffectWorld_UnpackModulatorToAmbientScale(modulator[0])` → `EffectWorld_AmbientScaleRGB @ 0x840b24..2c` (foliage + effects renderers) |
| 4 | @ 0x57e471–0x57e4ad | device fog color ← underwater ? `g_EnvWaterColorLit` : `is_alternate_fog` ? `0x808080` : fog block `[0]`; fog type ← underwater ? 1 : `g_EnvFogType`. `is_alternate_fog` IS the thermal-view byte (`Render_ProcessMainSceneFrame @ 0x5ca2da..0x5ca2e3`: Player_IsOpticalViewVisible && equipped Def flags2 & Thermal); modeled 2026-09-10 in `EnvironmentState::build_scene_fog` / `frame_clear_color_for` via `set_thermal_view` |
| 5 | @ 0x57e4c3–0x57e4db | `Render_SetFogState(0.5, end/65536, type, g_EnvOvercastBlend/65536)` |
| 6 | @ 0x57e4ee | `g_EnvFogEndApplied = end` |
| 7 | @ 0x57e4f4–0x57e505 | smoothed sky height pushed on change → `Terrain_PushSkyDomeHeightFloat @ 0x610920` → `SkyDome_SetHeightAndRebuild` (dome) |
| 8 | @ 0x57e50b–0x57e538 | only while `g_LocalPlayerEntity` is set (`@0x57E50B..0x57E512`) and `g_WacVarAutoGain` is nonzero (`@0x57E514..0x57E51B`): `modulator.target[11] = 0x10101 × Environment_ComputeAmbientLightAlongDirection(player)`, then `ColorBlock_SetStepDeltas(modulator, 62)`, so the exposure reaches the new target in 62 ticks (1 s); otherwise the modulator keeps its last target (the exit `@0x57E53D`). `autogain` is the WAC named value at 0xC6EAFC, seeded 1 by `WacScript_FreeAll @0x4F6300` (the store `@0x4F6371`), so a script that writes 0 freezes the exposure. Ported 2026-09-23: the weather tick samples both conditions into `WeatherState::iris_retarget_enabled`, read by `WeatherRuntime::feed_exposure_target`; a home with no World (previews, fixtures) keeps the gate open |

**Ceiling/floor application points** (the G4 question): (a) the **indoor branch of the
exposure sample** — `Terrain_SectorComputeLighting @ 0x5c7646..0x5c76fe` substitutes
ceiling block `[1]` (@ 0x26c6474) for sky and floor block `[1]` (@ 0x26c64dc) for ground,
with directional zeroed, when the position is under cover; and (b) the derived
`g_EnvCeilingFloorBlend @ 0x26c67f0` (= 0.707·ceiling + 0.707·floor, weather tick
@ 0x57f110) is stored by `Render_EmitterEffect @ 0x5f7163` into the effect world at
+0x3EC **beside** the outdoor `g_EnvTerrainLightCombined` at +0x3E8 — the indoor vs outdoor
ambient pair for particles/effects.

## Iris auto-exposure (C6 — the iris_percent / iris_center consumer)

`Terrain_SectorComputeLighting @ 0x5c7550` *returns* the iris gain (int 0..255; the
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
255 clamp in darkness. `Environment_ComputeAmbientLightAlongDirection @ 0x5c7a00` averages the
gain at **3 points marched from the camera-ray hit back toward the camera**
(`(s0+s1+s2)/3`), and that average becomes the modulator target (walk row 8 above) — so
iris is the engine's **global auto-exposure**, scaling *every* color block (sky, fog,
light, clouds…) through the modulator chain, not a niche terrain curve.

## Fog policy (`Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960`)

Driven from `Environment_ApplyFogAndAmbient @ 0x57e440`. `Render_SetFogState` passes
`useLinear = (fog_type != 0)`; the device layer then selects the model:

| fog_type | behavior |
|---|---|
| 0 | exponential fog, density = ln(64) / end; the caps pick table or vertex EXP fog (`@ 0x6779e4..0x677a48`), both on view depth. (The ln(64)/(3·end) form is the dormant LITE block, FOGMODE 4..7; [render/render-material-re.md](../render/render-material-re.md) §Fog parameter sets.) |
| 1 | linear, start = 0.5 world units (caller constant — effectively zero) |
| 2 | linear, start = (1 − density) × end × 0.5 |
| 3 | linear, start = (1 − density) × end × 0.25 |

The start the device receives is already folded: `Render_SetFogState` multiplies the
type-2/3 start by (1 − density) (`@ 0x58a9c5..0x58a9fe`); the caller's start constant 0.5
(`Environment_ApplyFogAndAmbient @ 0x57e4d2`, stored `@ 0x58a992`) is the type 0/1 start.
Every consumer (terrain, objects, water) uses that start as-is. The reimpl's per-type
shader re-derivation that dropped the overcast fold (`terrain_fog_start_for_type` /
`terrain_fog_factor_for_distance`) is deleted, and `compute_fog_params` keeps the
caller's 0.5 for type 0 too (2026-09-24, "Fog objects by their retail pass path and keep
the overcast fog start" and "Pin the type-0 fog start vector at the caller's 0.5"; the
`envfile/fog_type0` parity vector now reads start 0.5).

**Fog distance by pass path (2026-09-24).** Linear fog on the fixed-function path is
vertex fog with `D3DRS_RANGEFOGENABLE` (`CD3DDevice_SetFogParameters @ 0x677a50..0x677a69`):
the radial eye distance. A FOGMODE_SHADER pass (`SetFogAndBlendMode` modes 8..11 turn
table and vertex fog off, `@ 0x677783`) fogs from the VS
`oFog = 1 − (z − FogStart) × FogRangeRecip` (`Material_ApplyShaderParameters @ 0x58e20d..0x58e26f`):
linear in view depth for EVERY fog type, type 0 included, from start 0.5. The per-family
split (the `OBJ_FOG_VERTEX_SHADER` / `OBJ_FOG_VERTEX_SHADER_DEVICE` defines read by
`obj_scene_fog_visibility`) lives in [render/render-material-re.md](../render/render-material-re.md)
§Fog parameter sets.

**The viewmodel's fog (2026-09-24, "Fog the viewmodel under the dry pass").** The
first-person viewmodel draws under the DRY pass fog even when the eye is underwater:
`Environment_ApplyFogAndAmbient(0, thermal)` runs `@ 0x5ca3bf..0x5ca3ce` before the
viewmodel (`@ 0x5ca829`) and the underwater re-apply comes after it
(`@ 0x5ca82e..0x5ca841`). Its fog colour is the one `SkyDome_RenderWithSkyfog` restores when the sky
pass drew (`g_EnvFogBlock`, `@ 0x579ce6..0x579cf6`, under thermal too), else the dry
pass's own colour (`0x808080` under thermal). Reimpl: `EnvironmentState::build_viewmodel_fog`,
published as `opennova_viewmodel_fog_color` / `opennova_viewmodel_fog_range`.

`start == end` disables fog entirely. The jodemo-era claims in `engine/runtime/terrain` (exp ln64 for
type 0, 0.5/0.25 linear starts) are behaviorally correct; only their address citations were
wrong for the retail image.

`density = g_EnvOvercastBlend / 65536`. End distance (`Environment_GetFogEndDistance
@ 0x57e3e0`): above water `smoothedFogDist × (1 − overcast/2)`; **underwater the end distance
comes from water murk**: `(1 − 0.992·(1 − (1−m)(2−m)/2)) × 200` units (m = 0.8 → ≈ 25 u), and
the fog color switches to the lit water color. A "white fog" pass uses constant `0x808080`.
The applied fog distance also feeds the far clip/culling. The sky dome does NOT take it: `Render_Skybox` loads its c9.x fog constant from the raw smoothed `g_EnvFogDistCurrent` with no underwater or overcast leg (`@ 0x5792ab..0x5792c2`, see the VS-constant table below), so the dome keeps the unscaled distance while the world passes fog to the murk end (the shell's `sky_dome.cpp` substituted the murk end underwater until 2026-08-30; `sky_dome_test` pins the unconditional value on both sides of the water plane).

### Underwater full-frame murk composite

Distance fog is only the first underwater leg. After the world, first-person
viewmodel, particles, weather, and foliage have rendered,
`Terrain_RenderWorldScene @ 0x5c96c5..0x5c96fa` independently tests
`camera_z <= g_EnvWaterHeightFixed` (`jg` skips the draw). On the underwater side,
including exact equality, it calls `Render_DrawViewportColorQuad @ 0x5c38e0` over the
full current viewport with RGB = `g_EnvWaterColorLit` and alpha
`0x80 - trunc(g_EnvWaterMurk * -96.0)` = `128 + trunc(96 * murk)`. Thus CP01's
`water_murk 0.8` produces byte alpha 204. The pass disables Z-write, forces
ZFUNC ALWAYS, and uses standard `SRCALPHA` / `INVSRCALPHA` source-over blending:
`out = lit_water * (alpha/255) + prior * (1 - alpha/255)`. This is the retail
mechanism that gives the sky, terrain, particles, and weapon the near-uniform
lit-water attenuation while leaving the later HUD untouched.

The ordering has one important tail: skybox sun glow is drawn after the murk
quad (`@ 0x5c9714`, under the modulator forced to `0xFF404040` `@ 0x5c96fd`), and
the HUD later still (`@ 0x5cad04`). Since 2026-09-24 ("Draw the scene's post-particle
tail in its own overlay stage", "Draw the water glint and the sun glare at the end of
the overlay tail") the port draws the murk in each 3D view's post-particle overlay pass
(`renderer::kSceneOverlayOrder` in `engine/runtime/renderer/scene_overlay.h`, executed by
`SceneOverlayCompositorEffect`): full viewport, `g_EnvWaterColorLit` with the alpha byte
above, SRCALPHA / INVSRCALPHA, ZFUNC ALWAYS, whenever that view's render eye is at or
below the water height (spectator, third-person and death cameras included), after the
view's particle passes and before the bloom; the sun glare follows it in the same pass.
The former `PlayerViewEffects` murk quad and its "glare below the murk" residual are
retired, and D-RORD-9 closes (see
[render/render-order-re.md](../render/render-order-re.md)).

## Sky dome (`Render_Skybox @ 0x579080`) — full combine recovered (C6)

Dome = 441 vertices / 800 triangles (21×21 grid, FVF `0x212` = XYZ|NORMAL|TEX2, stride 40,
`SkyDome_BuildMesh @ 0x578db0`). C7 pre-port reads pinned the builder's remaining
unknowns: the height scale `v14 = skyHeight/175.69` stretches **Y only** (x/z stay at the
1024-unit rim radius, @ 0x578ed4), and the **dome normal is the builder's anisotropic
normal `normalize(x, y_scaled/v14², z)`** — equivalently `normalize(x, y_unscaled/v14, z)`
— *not* the normalized vertex direction (@ 0x578fbb..0x579023). The dome's world
translation is the camera position with **height halved** (`camHeight >> 1`,
@ 0x5790b2..0x5790d7) — the dome rides up at half the camera's vertical rate. The shader
path draws the dome **twice — but the two passes are NOT one-per-cloud-layer** (pre-C6
misreading): pass 1 is a **textureless sky gradient**, pass 2 draws **both cloud layers in
one multi-stage pass**, alpha-blended over it. Both vertex shaders are embedded as `vs_1_1`
*source text* (`g_SkyVSGradientPassSource @ 0x7d7338`, `g_SkyVSCloudPassSource @ 0x7d6fc0`)
and assembled at runtime with the statically-linked `D3DXAssembleShader` in
`Terrain_InitRenderingResources @ 0x5789e0` (handles → sky+116 / sky+120).

**Pass order and depth (2026-09-24 rendering parity pass).** `Render_Skybox` draws the
gradient pass (`@ 0x5798dc`), then `Render_CelestialBodies(0)` (`@ 0x5798e0`), then the
cloud pass (constants and draw `@ 0x5798f1..0x579b15`): the clouds cover the sun and
moon. The flat path (`@ 0x579b42`, `SetFogAndBlendMode(0)`) has no cloud pass. Both dome
passes run under pass flags 0x300000, z-write off and ZFUNC ALWAYS (`@ 0x579883`
gradient, `@ 0x579ac1` cloud, `@ 0x579c3e` flat path): the dome never writes depth and
never occludes the world. The world drawn after it wins everywhere; the bodies drawn
between the dome passes test only against the cleared depth. Reimpl: `sky.gdshader` (pass
1 and the flat dome) and `sky_clouds.gdshader` (pass 2, the dome material's `next_pass`,
`blend_mix`, attached only on the shader path with a bound cloud layer) are both
`depth_draw_never` with the clip depth pinned to the reverse-Z far plane
(`POSITION.z = 0`); the part of the dome beyond the Godot far plane is full skyfog, the
clear, so it is invisible there too. Because a no-depth-write surface sorts in Godot's
transparent list, each pass rides its own ladder rung
(`engine/runtime/renderer/render_order.h`): the gradient `kRungSkyDome` (-16) opens the
sky pass, then the bodies `kRungSkyBody` (-15), then the clouds `kRungSkyClouds` (-14),
all before the viewmodel, the slot drape and every world transparent (renumbered
2026-09-26 for `kRungSlotDrape`). Without the gradient's rung it
painted over the clouds, the discs and every far-side alpha at the default rung 0
(found at the 03tr-sun-sky capture; after the fix the upper sky matches retail to within
1 level). Commits: "Pin the sky dome to the far plane without a depth write", "Split the
dome's cloud pass after the bodies and port the sky pass gates", "Open the sky pass with
the dome gradient's own rung". Windowed `sky_dome_test` pins a world quad beyond the dome
surface at altitude drawing over the sky.

**Sky pass gates (2026-09-24).** The main frame's sky bracket (`SkyDome_RenderWithSkyfog` →
`Render_Skybox`, the dome and the bodies it draws, `@ 0x5ca81a`) runs only while blink
letter 0x4 is clear AND the eye is strictly above the water
(`@ 0x5ca1a3..0x5ca1bd` → `@ 0x5ca7c4..0x5ca81a`); the water mirror's own sky bracket is
skipped under the indoors letter 0x2 (`Render_MainScene @ 0x5c1342..0x5c1353` →
`@ 0x5c166b`). The glare, the glint and the sun veil are drawn outside the bracket and are
never gated. Reimpl: `renderer::scene_pass_gates` (`engine/runtime/renderer/scene_pass_gates.h`)
→ `OcclusionFrame.apply_scene_pass_gates()` feeds `u_beauty_pass_drawn` /
`u_mirror_pass_drawn` per pass camera
([render/render-occlusion-re.md](../render/render-occlusion-re.md) §4).

**Builder internals (ENG-2 sky-leg re-grill, 2026-07-06 — the engine/formats/env port's witness
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
only when the SMOOTHED height changes (`g_EnvSkyHeightCurrent != g_EnvSkyHeightApplied
@ 0x57e4f4` → `Terrain_PushSkyDomeHeightFloat @ 0x610920` →
`SkyDome_SetHeightAndRebuild @ 0x579070`, which stores `this+0x48` and re-bakes;
buffer creation is `SkyDome_CreateBuffersAndBuild @ 0x579d10`, 441×40-byte VB +
2400-index IB). Ported: `env::build_sky_dome_mesh` + `kSkyDomeReferenceHeight`
(`engine/formats/env/env_render.cpp`, dome section in `env_render_unit_test`).

**Scroll-rate state (back-filled citations):** the live rate `g_EnvCloudScrollRate
@ 0x26c686c` smooth-eighths toward `g_EnvCloudScrollRateTarget @ 0x26c6870` each tick
(`@ 0x57eecc`); the target — not the rate — is refreshed from the parsed
`g_EnvSkySpeedFixed` (= `sky_speed << 10`) at mission-start snap (`@ 0x57d2da` in
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
was wrong; @ 0x27219f0), c9 `[fogDist×0.9/65536, 0, 1, 0]` (fogDist = the RAW smoothed
`g_EnvFogDistCurrent` `@ 0x5792c2`, NOT the overcast-scaled `Environment_GetFogEndDistance`
end the object/terrain passes fog with; confirmed 2026-08-29: `sky_frame.cpp` feeds the
unscaled current and the `environment_state` ctest pins it against the scaled device
end), c10 `[0, 0.5, 1, 0.25]`,
c11 skybase, c12 (skybright − skybase), c13 = direct near-unit sun getter tuple (pass 1) /
active getter tuple, moon at night (pass 2), c14 = the same point at camera + dir×2000 pushed
through the view-proj transform and normalized (clip-space proximity reference),
c15 skyhighlight, c16-19 / c20-23 the two UV scroll matrices (layer 1 `(camera +
scrollAcc1x)/2^28`; layer 2 `(camera + scrollAcc[4/3 U, 2/3 V])/2^29`), c24 cloudbase,
c25 (cloudhighlight − cloudbase) — **uploaded but read by neither shader (dead)**,
c26 cloudedge, c27 cloudhighlight.

**Color-constant upload scale (re-grilled 2026-07-14).** `Render_Skybox` passes the six
packed sky/cloud blocks through `Color_UnpackToFloat4` (`@ 0x579312`, `0x579326`,
`0x57933a`, `0x57934a`, `0x57935b`, `0x57936f`). Its normal branch multiplies each RGB
byte by `0.0078431377 = 2/255` (`@ 0x578985..0x5789c0`) and does **not** clamp the shader
constant; the vs.1.1 `oD0`/`oD1` color outputs are the saturation boundary before the
fixed-function stages. This scale is separate from the TSS `MODULATE2X` cloud-density
operation. Fog is deliberately excluded: `D3DRS_FOGCOLOR` receives the active packed fog
dword verbatim, so that already-weathered color remains byte/255 at the dome pass.

**Cloud texture axes and dome fog saturation (2026-09-27, OT-E6).** Retail draws the dome in
the render basis its builder authors: the world matrix is the anchor translation alone
(`Render_Skybox @ 0x5790e8..0x579146`), UV1/UV2 are the render x/z scaled by 1/320 and
3/2048 (`SkyDome_BuildMesh @ 0x578db0`), and the scroll's camera term runs on the same render
axes (`@ 0x5791de..0x579260`). The reimpl drew the builder's vertices identity into the Godot
world (Godot x = render z). Texture u therefore ran along render z, a mirrored cloud field
drifting the opposite way (the D-RLIT-9 class: a render-basis tuple consumed as Godot world).
At 03tr-sun-sky it read 2-3.7 levels high in R and 2.9-4.5 low in G/B over the upper sky.
`EnvFile::build_sky_dome_arrays` now places each vertex and normal through the util/axes.h
swap (UVs unchanged), and `SkyDome` hands the scroll the render-basis camera (render x =
Godot z). Both dome shaders cull back faces: the swap mirrors the winding, and retail culls
CCW (`CGfxShader_ApplyPass @ 0x6832a8..0x6832be`; the dome's 0x300000 pass flags and
0x20200/0x20000 effect flags carry no 0x400000 no-cull bit). Separately, vs_1_1 oFog is
saturated per vertex before the rasterizer interpolates it (the D3D9 output-register
contract, like oD0/oD1). The dome shader clamped per pixel, which fogged triangles
straddling the fog end up to 0.5 level more. A two-pass model built from these witnesses
reproduces the settled retail 03tr frame (RMSE 1.64, band means within 0.14 level) and a
00TRa 15:00 capture pitched 25 degrees up (RMSE 0.43) only with the render-basis mapping.
After the fix the port fits it too (RMSE 0.98 and 0.48). The frames then differ only by the
scroll phase (time since mission start), which the fixture contract does not pin. GUT
`sky_dome_test` pins the UV axes and the camera term; `env_parity_vectors_test` re-dumped
sky/verts, sky/k001 and sky/k064 under these witnesses.

**NVG and thermal dome (2026-09-24, "Dim the sky dome under NVG, whiten it under thermal,
exact light blend").** Under first-person NVG (`g_NVGActive` && `g_CameraMode` == 0,
`@ 0x578901..0x578911`) `Color_UnpackToFloat4 @ 0x578900` takes its NVG branch:
f = (level + 1) × 0.2, channel = byte × 2 × 0.25 / 255 + f × 0.0015625
(`@ 0x578913..0x57897b`; alpha stays 1); the normal branch is byte × 2/255, unclamped, a = 1
(`@ 0x578985..0x5789c3`). Under the thermal view `Render_Skybox` overrides the three sky
constants to 1.0 and the three cloud constants to 0.9 after the unpack
(`@ 0x579377..0x579447`, `flt_7C459C` = 0.9), and `SkyDome_RenderWithSkyfog(thermal, z_tested)` fogs the
dome toward `0x808080` instead of skyfog; the main frame passes the thermal byte
(`Render_ProcessMainSceneFrame @ 0x5ca363` → `@ 0x5ca81a`), while the water mirror calls
`SkyDome_RenderWithSkyfog(0, 0)`, so reflections keep the normal sky. Ported in
`engine/runtime/environment/sky_frame.cpp` (`sky_constant` gated by
`EnvironmentState::nvg_view_active()`; `kThermalSkyWhite` / `kThermalCloudWhite` /
`kThermalSkyFog`).

**Pass 1 — sky gradient** (VS `g_SkyVSGradientPassSource`, raw UVs, no texture — its effect
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

**Pass 2 — clouds** (VS `g_SkyVSCloudPassSource`, both scrolled UV sets; per-vertex):

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
render-texture loader `Texture_LoadFromArchive @ 0x58b980`
(`Terrain_InitRenderingResources @ 0x578a97/0x578aa5`, flags `0x100000`;
`g_EnvSkyMap1Path @ 0x26c63e8` / `g_EnvSkyMap2Path @ 0x26c63f8`, parse-coerced to
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
(`g_EnvCloudBlock @ 0x26c64a4` xref sweep: render path reads exist solely in that branch;
the keyframed path's cloud colors come exclusively from the cloudbase/cloudedge/
cloudhighlight blocks). C7 correction: the flat path applies the **pass-1 effect** —
created with `CEffect_SetTextureParam(NULL)` — so it is **textureless** (the earlier
"textures still bound" reimpl comment was wrong). A white-sky variant forces sky colors
to 1.0 and cloud colors to 0.9 (the thermal view's override on the shader path; see
"NVG and thermal dome" above). `Environment_ApplyFogAndAmbient` pushes the smoothed sky
height to the dome (`@ 0x57e4f7`) only when it changes.

**Dome fog color (2026-07-21 correction).** The sky wrapper temporarily switches
`D3DRS_FOGCOLOR` to the final skyfog block while both dome passes run, then restores
the ordinary world fog `[orig: SkyDome_RenderWithSkyfog @ 0x579cb0]`. VS fog still comes from
`CD3DDevice_SetFogAndBlendMode(.., 8)`; its `oFog` blend converges on the same
post-horizon-blend, saturating-doubled skyfog used by the modulate2x frame clear.
Terrain and other world passes continue to consume the active ordinary fog block.

**Godot background color bridge (2026-09-16 review).** Retail's selected
packed frame-clear color goes directly to the device clear, while the dome
fog uses the same final skyfog bytes. `[orig: Render_ProcessMainSceneFrame
@ 0x5ca776..0x5ca7bf; CGfxTextOverlay_Draw @ 0x677100 (IDB misnomer: calls device Clear); SkyDome_RenderWithSkyfog @ 0x579cb0]`
Our D-RMAT-7 gamma-domain scene shaders preserve those values, but Godot
4.6.1's BG_COLOR path calls `srgb_to_linear` on `background_color`
([Forward+ implementation](https://github.com/godotengine/godot/blob/4.6.1-stable/servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp)).
The unadapted clear therefore appeared darker below the dome, making a large
curved seam visible in `00TRa.bms` and `00TRg.bms`. The `SkyDome` and
`GameWorld` device writes now apply `linear_to_srgb` once before setting the
Godot background property, including underwater/indoor paths. Engine colors,
dome geometry and fog math are unchanged. This completes the existing #20/#21
clear-to-dome contract at the Godot boundary; it is not a new retail rule.

Evidence: windowed GUT `sky_dome_test` compares a fully fogged dome pixel with
exposed background in the same viewport (red before the bridge, green after).
Windowed captures of both reported training maps confirm the seam is gone.
Headless runs explicitly skip the pixel test. `game_world_test` also pins the
underwater property conversion. No IDB edits were made.

## Celestial bodies (`EffectWorld_LoadCelestialModels @ 0x5adc50`) — placement re-witnessed 2026-07-06

**The live renderers** (celestial-leg grill; three misnamed; four dead, the star
renderer among them):

- `Render_CelestialBodies @ 0x5acaa0` (was "render_skybox_fog_layers"; called from
  `Render_Skybox @ 0x5798e0/0x579c7a`): sun and moon place at **camera + direction ×
  64.0** — full camera height, identity rotation, submit flag 0x100. Sun alpha =
  `clamp((1 − g_EnvOvercastBlend) × (0x640000 − g_EnvSunDimPctCurrent)/100)` with a
  signed division (the `+1` the earlier text showed is only the signed-division
  rounding fix-up, `@ 0x5acb96..0x5acbfa`; `celestial_sun_alpha_fixed` is right)
  (`@ 0x5acbc1..0x5acbfa`; the SunDim channel is one of the #27 spring family, default
  0, no `.env` parser writes it). Moon alpha = `clamp01((fogDistInt − 400)/600) ×
  (1 − overcast)` without the fog shader (`× fogDistInt × 0.0002` with)
  (`@ 0x5acc40..0x5acccd`). The bloom-pass redraw `Render_CelestialBodies(1)
  @ 0x582a77` is the fog-shader call (`CD3DDevice_SetFogAndBlendMode(&dword_3262260,
  2) @ 0x5acb80`), so the bloom pass evaluates the discs' RgbGen at the fog-shader leg's
  UPL_INTENSITY (the Q3 push carries the NormalCopy emissive
  `sat(SelfLumColor × gain) × 2` under the material's fog policy) and the Q3 blend follows
  the authored material (the sun/moon `FF_ST_AD_LUM` discs add). There is NO `dir.y`
  visibility gate and NO `sky_height/175.69` distance scaling — the world overdraws the
  bodies (draw order: dome gradient → bodies → dome clouds → world). The dome writes no
  depth, so the bodies test only against the cleared depth; the reimpl pins both to the
  far plane and lets every world surface win.

  **The alphas reach the discs through the model's CTRL register, not a tint
  (2026-09-24).** `g_CtrlGlobalUplIntensity @ 0x83fde8` is the global CTRL register ordinal
  32, UPL_INTENSITY (`dword_83FCE8 + 32*8`). The submit writes it; the batch FLUSH reads
  it: `CRenderBatchQueue_FlushBatches` → `Material_ApplyShaderParameters @ 0x58ddfb` →
  `RgbGen_EvaluateColor @ 0x5b2453` evaluates the material's RGB generator (style 113 on
  register 0) at that value. The stock models are FF_ST_AD_LUM with CTRL 0 named
  UPL_INTENSITY: Msun 0 → FFFFFF, FMoon4 0 → 828282, Mglare material 1 0 →
  (255, 200, 108), material 2 (45, 45, 45) → (128, 252, 255). `_FFP.fx` SELFLUM: emissive =
  SelfLumColor × ColorSrcGlobalGain, MODULATE2X, ONE/ONE, FOGMODE_NORMALADD (fogs toward
  black). The sun and moon submit back to back and flush once (`@ 0x5acbfa` sun alpha,
  `@ 0x5acc1c` sun submit, `@ 0x5accc1` moon alpha, `@ 0x5accdd` moon submit, flush
  `@ 0x5acce9`), but each disc still evaluates its OWN alpha: the submit snapshots the
  registers its strip's material lists into the batch entry
  (`Render_CollectRenderObjectsForBatch @ 0x5d91c0..0x5d91de`, entry +0x2C) and the flush
  writes them back before that batch's RgbGen (`CRenderBatchQueue_FlushBatches
  @ 0x5da1d6..0x5da1fd`). The glow flushes alone (`@ 0x5ad118`) and is not submitted at
  alpha <= 0 (`@ 0x5ad0ae`); the glint draws when its brightness is nonzero
  (`@ 0x5ad35c..0x5ad36b`, submit `@ 0x5ad470`). Submit flags 0x110 vs 0x100 differ only
  by ZFUNC ALWAYS; they never override the material blend. Reimpl (2026-09-24, "Draw the
  celestial bodies through their authored SELFLUM material", "Rebind the celestial sky
  hook after a model scene rebuild", "Keep each celestial disc's own alpha through the
  shared flush"): Celestial no longer overrides the body materials; the authored
  ObjectModel materials draw through the `vertex_standard.gdshaderinc` sky hook (camera
  re-anchor per pass, far pin, per-pass gates), Celestial writes UPL_INTENSITY per body
  (`celestial_frame.h` `build_celestial_discs_frame` / `build_glare_frame`, the sun and
  the moon each with its own value), and the glare and glint bodies go to the
  post-particle overlay stage through `Celestial.get_overlay_bodies()`. The former
  `celestial.gdshader` / `celestial_additive.gdshader` are deleted; render diagnostics
  report `sky_hooked_surfaces`; the first port had the sun take the moon's alpha. At the
  03tr-sun-sky fixture (revx02, 06:30) the saturated sun area (all channels 255, raw x0.96)
  reads 4318 against retail's 4725 with the centroid on retail's (520.5, 381.7), remeasured
  2026-09-27; the earlier 1747/1823 reading is not reproducible. The remaining gap is the
  sun glow ("Open after the 2026-09-24 pass").
- `Render_SkyboxSunGlow @ 0x5acd00` (live: `Render_MainScene @ 0x5c1904` +
  `Terrain_RenderWorldScene @ 0x5c9714`): the glare pass — see the env #14
  closure below.
- **Dead variants** (zero callers): `Render_SkyboxLayers @ 0x5ac230` (the +64/+16
  fixed offsets + 0x2000 alpha the earlier notes described),
  `Render_SunLensFlare`, `Render_FoliageAtCamera`, and the star renderer:
  - `Star_RenderField_Unused @ 0x5ad9c0` (was "render_foliage_billboards_0") has NO caller:
    no code xref, no E8/E9 rel32 call and no absolute `0x005AD9C0` pointer in any
    segment. The only other reader of `g_CelestialStarModel @ 0x27e563c` is the loader
    (`EffectWorld_LoadCelestialModels @ 0x5adcd6/0x5add06`).
    `Star_GenerateInstanceTable @ 0x5ac850` still runs per celestial load
    (`@ 0x5add40`), but only the dead renderer reads `g_StarInstances` (`@ 0x5ada86`),
    and `g_CelestialUplModel` is drawn only inside it (`@ 0x5adb92`). **Retail JO shows
    no stars** (re-graded 2026-09-24, #33). The dead code, kept as a record: 256 star
    instances (`g_StarInstances @ 0x27E2E38`, 40-byte entries: camera-relative offset,
    billboard param, twinkle add/mask, brightness accumulator, direction). Per star:
    hidden when `dot(star_dir, light_dir) > ~0.98` (masked near the bright body),
    position = camera + offset, twinkle = `(accum + add + (prng16 & mask)) >> 1` with
    the rol4/rol11 PRNG (`g_StarTwinklePRNG @ 0x840B38` — the PRNG_Next16 algorithm on a
    separate state). A second part renders `g_CelestialUplModel`. **The instance-table
    GENERATOR was found at REN-6 (2026-07-06)**: `Star_GenerateInstanceTable
    @ 0x5ac850` (ex kong misnomer `init_weather_particles`; its loop pointer starts
    at table+16 and strides 40 to `g_CelestialSunModel @ 0x27e5648` — why no xref to
    the table base existed). Sole caller `EffectWorld_LoadCelestialModels
    @ 0x5add40` — the table regenerates per celestial load. Per star, all draws from
    `g_StarTwinklePRNG` (low-16 of `state = rol4(state + rol11(state, 11), 4) ^ 1`;
    a standalone step function exists dead at `Star_TwinklePrngNext_Unused
    @ 0x5ac010` — the live sites inline it): entry dwords `[0]` offX = `(r −
    0x8000) << 9`, `[1]` offY same, `[2]` offZ = `((r + 0x20000) << 6) − ((|offX| +
    |offY|) >> 3)` (the dome shaping — high overhead, pulled down toward the rim),
    `[3]` billboard param = `(r & 0x3FF) + 12288`, `[4]` twinkle add = `r & 0xFF`
    (0 → 1, clamped to `255 − mask`), `[5]` twinkle mask = `31 >> (r & 3)` ∈
    {31, 15, 7, 3}, `[6]` UNWRITTEN (BSS zero — the render loop's brightness
    accumulator), `[7..9]` = `normalize(off >> 8)` 16.16 direction (the
    near-light cull input). Like the water noise field, the table CONTENT depends
    on the shared PRNG's call history at load (value-history quirk). The reimpl's
    port of the generator and renderer (`env::generate_star_instances` /
    `star_twinkle_tick` / `star_visible_fixed`, `StarField`) was deleted 2026-09-24
    ("Delete the star field: retail never draws stars").

**env #14 closure — the glare occlusion** (`@ 0x5acd9e..0x5acf7f`): TWO jittered rays
per frame feed an 8-bit SLIDING window (`g_GlareOcclusionWindow >>= 1` per sample, bit
0x80 = visible) — the window spans the last 4 frames (not 8 rays at once, correcting
the earlier summary). Ray = camera → camera + sunDir × 1024, jittered per sample from
`g_GlareJitterFrameIndex` bits (engine-Y ±16 / ±8, height ±16); the coarse test is
`Terrain_RaycastLoResNoNormal @ 0x610860` → `Terrain_RaycastHeightmapLoRes @ 0x60cb80`
(the lo-res DDA — the ENG-3 raycast seed), refined by `Physics_RaycastIntContext` +
player-occlusion checks. Brightness (`g_GlareOcclusionBrightness`) steps ±16 with a
±16 DEAD-BAND HOLD (never snapping) toward `popcount(window) × 32 ×
(g_EnvFogDistCurrent / 1000)` (flt_7DA0C4 = 1/65536000). Glow submit alpha =
`dot_view⁴/2 × brightness >> 8 × (1 − overcast) × SunDim fold` (`@ 0x5acfb8..0x5ad0a9`);
the FBEFFECTS ≥ 3 quality gate quarters it (`FrameFX_QualityAtLeast3 @ 0x581f6a` reads the
frame-buffer-effects level, ex the "scope check" misread; `glow_intensity >>= 2
@ 0x5ad033..0x5ad03c`) — the bloom pass re-adds the glare, so the
highest-quality program dims the direct draw. The BLOOM-pass call is
occlusion-INDEPENDENT: `FrameFX_RenderGlowSource @ 0x582a80` calls
`Render_SkyboxSunGlow(0, 0)` — no occlusion test — and the no-occlusion
brightness is `(fog_km + 1.0) × 0.5 × dot_factor`
(`@ 0x5ad013..0x5ad027`; `flt_7C3280` = 1.0, `flt_7C3B94` = 0.5, fog_km =
`g_EnvFogDistCurrent × flt_7DA0C4` = 1/65536000), through the same
quarter/overcast/SunDim tail. The bloom pass also redraws the celestial
discs and the glow through the FAR-BAND viewport: `Render_SetViewportFarDepth
@ 0x582a70` fills a D3DVIEWPORT9 with MinZ 0.98000002 / MaxZ 0.99996948
(`@ 0x58a840`) before `Render_CelestialBodies(1) @ 0x582a77` and
`Render_SkyboxSunGlow(0, 0) @ 0x582a80`, and both draws end in the ordinary
z-tested flush (`@ 0x5accee` / `@ 0x5ad118`) with the beauty depth-stencil
still bound. The viewport remaps the primitives' depth into [0.98, 0.99997],
and the beauty depth they test was written through the scene viewport
`Render_SetViewport @ 0x58a720` (MinZ 0 `@ 0x58a72f`, MaxZ 0.99996948 `@ 0x58a739`;
main frame `@ 0x5ca5fc`, re-set by `FrameFX_RenderGlowSource @ 0x582a45` before the band).
A fragment at view depth w survives over beauty depth D iff
MinZ + (MaxZ − MinZ) z(w) <= SceneMaxZ z(D), i.e.
1/D <= (MinZ/f + (MaxZ − MinZ)/w) / SceneMaxZ, independent of the near plane: at a
700 u fog a 60 u disc survives only over beauty depth past about 577 u. So the disc and
the glare survive only where the beauty depth is at or near
the far plane (cleared sky): EVERY nearer surface, terrain past 64 u included,
occludes them in the bloom source (the earlier "terrain past 64 u does not
occlude the disc" reading was wrong). Ported 2026-08-23, source replaced
2026-08-29, far band ported 2026-08-29: `glare_q3_alpha_fixed` +
`glare_q3_peak_opacity` + the Celestial `SunGlow` producer's bloom-pass SelfLumColor
(`FrameFx.set_q3_celestial_self_lum`, since 2026-09-24; the former `u_q3_opacity` read is
gone). The Q3 disc and glow blend follows the body material's registered
classification, mapped like the object NormalCopy (_OP replace, _AB alpha, _AD add);
the submit flags 0x100 / 0x110 (`@ 0x5ad0f5..0x5ad0fe`) differ only by ZFUNC ALWAYS and
never override it. The Q3 colour is the NormalCopy emissive
`sat(SelfLumColor × gain) × 2` (`renderer::q3_celestial_emissive`; 2026-09-24, "Saturate
the celestial bloom emissive like the SELFLUM copy"; the first port's
`SelfLum × min(gain, 1) × 2` left the glow's bloom source 16% short at the 06:30 gain of
19/16). The beauty-only shader keeps the far-plane disc pin while the focused draw scales
its own reverse-Z depth by (MaxZ − MinZ)/SceneMaxZ (`q3_far_band_reverse_z`,
`engine/runtime/renderer/q3_frame.h`; exact for the beauty camera sharing the scene far
plane) and z-tests it against resolved beauty depth. The earlier
[1 − MaxZ, 1 − MinZ] remap treated the beauty depth as [0, 1] and let the disc through
from about 427 u (fixed 2026-09-24, "Test the bloom far band against the scene viewport's
MaxZ").
Ported: `env::GlareOcclusionState`/
`glare_ray_jitter`/`glare_occlusion_tick`/`glare_glow_alpha_fixed` +
`celestial_sun/moon_alpha_fixed` + `kCelestialBodyDistance` (ctest landmark-pinned;
`glare_brightness_step` corrected to the witnessed dead-band form). The dot³² curve
(`Environment_ComputeSunGlareAndFogBlend @ 0x5ad610`) stays live via
`Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0` (ex `sub_5AD8B0`) —
witnessed in full 2026-08-20 (the 03tr-sun-sky fixture round):

**The sun veil + exposure stop-down** (`@ 0x5ad8b0`, once per main scene frame
from `Render_ProcessMainSceneFrame @ 0x5cac4b`, gated on `g_CelestialSunModel`):
`Environment_ComputeSunGlareAndFogBlend` runs for the primary view ray (occlusion
brightness) and, when `g_EnvWaterHeightFixed`, a secondary water-reflected ray
(`Water_ComputeReflectedSunPoint` clipped point, its own brightness accumulator
`dword_27E2E28 >> 2`), sums clamped 192. Each output already folds SunDim
(`×(0x640000 − dim + 1) >> 8, /25600` — identity at dim 0) and overcast
(`×(1 − blend)`) inside `@ 0x5ad610` (`@ 0x5ad822..0x5ad8a3`). Consumers:

- `glare > 2` draws a FULLSCREEN WHITE quad with alpha = the glare byte
  (`(glare << 24) + 0xFFFFFF`, mode 1 → `Render_FullscreenDecalQuad
  @ 0x5c6590`, an alpha-blended viewport quad) — the visible sun-glow veil,
  far larger than the additive glare 3DI body. It draws after the damage/white
  flash quads (`@ 0x5cabb0..0x5cac36`) and is skipped on the death screen.
- The dot¹²⁸ output (previously summarized as "fog whitening") is really an
  EXPOSURE STOP-DOWN: it writes the modulator-2 TARGET (`g_EnvModulator2Target
  @ 0x26c66a4` — its ONLY writer) to `0x10101 × (64 − (3·min(v,40)) >> 1)`
  with 8-tick step deltas (`@ 0x5ad96a..0x5ad989`); at v ≤ 0 it releases to
  `0x404040` over 124 ticks (`@ 0x5ad996..0x5ad9a7`). Staring into the sun
  therefore stops the whole modulated frame down while the white veil blooms.

Ported 2026-08-20: `env::sun_veil_from_dot` (exact integer chain incl. both
folds), `ModulatorChain::set_sun_veil_stopdown` (modulator-2 had wrongly been
pinned to a constant identity target), the `Celestial` per-frame veil leg
(pushes the `opennova_sun_veil_alpha` shader global; the world's veil leg
forwards the stop-down to `Weather`), and the `PlayerViewEffects` white veil
rect. The same round witnessed and FIXED the AXIS divergence the fixture's
"sun rises in the wrong place" half came from: the celestial tuples are
RENDER-FLOAT (D3D world) axes — mission `(x,y,z)` -> render `(-y, z, x)`
`[orig: Math_FixedPointToFloat3_YNegated @ 0x611210]` — while the reimpl's
Godot world is mission `(x, z, -y)`; the Godot-facing seams had
identity-mapped the tuple (bodies/glare/dome/object-directional/shadow 90
deg off in yaw + mirrored). Fixed via the `godot/src/util/axes.h` x/z
swap at every consumer seam (render-lighting-re.md carries the closed note
and the per-seam list).

**The water-reflected sun glint** (`Environment_UpdateSunGlare @ 0x5ad130`, once per
scene pass from `Terrain_RenderWorldScene`, the call `@ 0x5c96c0`: the main
scene's, then the weapon Inset pass's at its own camera, on one accumulator;
the sun veil `@ 0x5cac4b` reads the brightness after both, the Inset pass
being called `@ 0x5ca949`; per scene pass since 2026-09-26, with the capture
settle stepping both calls per frame) —
witnessed and ported the same round: its own 4-bit window
(`dword_27E2E2C`, one sample per frame, bit 8 = visible) and ±16 chase
toward `popcount × 64` (`dword_27E2E28` — no dead-band, no fog scale). The
sample point is the reflected-sun position on the water plane
(`Water_ComputeReflectedSunPoint`: reflect the camera about the water height with a
`0.25 × (frame & 3)` lift, interpolate along sun × 2048; then ±2 x/z point
jitter), visible when the point sees both the sun (`point -> cam +
sun × 2048`) and the camera over terrain plus the physics/player checks.
The settled brightness submits the glare 3DI mirrored below the eye
(camera + sun × 128, height negated) at alpha `(dot⁴ − 28672/65536) ×
brightness >> 8 × SunDim fold >> 2` (no overcast fold), and `brightness >>
2` feeds the sun veil's SECONDARY term (both veil sums clamp 192).
Ported: `env::WaterGlintState`/`water_glint_tick`/`water_glint_point`/
`water_glint_alpha_fixed` + the Celestial "glint" body leg and the veil
secondary term. The glare gate ray's start-height lift
(`+1.0 + 0.5 × (frame & 3)` `[orig: @ 0x5acde4]`, fine rays from the exact
camera height) is ported as `glare_coarse_start_lift` with the witnessed
coarse-gate-then-jittered-fine cadence; the fine rays' entity leg keeps the
documented sun-occlusion statics posture (render-lighting-re.md
D-RLIT-2/D-RLIT-3). The veil rect draws on the PlayerViewEffects
behind-parent stack (veil, then the NVG mask), the reimpl home for the retail
post-scene screen quads. Since 2026-09-24 ("Draw the water glint and the sun glare at
the end of the overlay tail") the glint and the glare are no longer 3D bodies in the
transparent list: each view's post-particle overlay stage draws the glare model's SELFLUM
surfaces (read from `Celestial.get_overlay_bodies()`: the model, its UPL_INTENSITY value
and whether retail submits it) as tex.rgb × 2 × sat(SelfLumColor × light scale), ONE/ONE,
fogged to black, ZFUNC ALWAYS (submit 0x110). The glint (only while the mission water
height is nonzero, `@ 0x5c96b5`, drawn `@ 0x5c96c0`) runs under the frame's light scale
(`Render_UnpackModulatorToLightScale @ 0x58db30`, byte / 64 per channel, read by
`Material_ApplyShaderParameters @ 0x58e05d`); the glare comes last, after the murk, under the
modulator forced to `0xFF404040` = 1.0 (`@ 0x5c96fd`, glare `@ 0x5c9714`). The murk moved
out of `PlayerViewEffects` into the same stage (§Underwater full-frame murk composite).

### Original notes (pre-2026-07-06, kept for provenance)

Lazy-loaded by name into handles: sun (`g_CelestialSunModel @ 0x27e5648`), moon (`0x27e5644`),
glare (`0x27e5640`), star (`0x27e563c`), plus hardcoded `upl.3di` (`0x27e5638`). Glare, star
and upl load after `Model_SetNextLoadPassFlags(0x300000, 0)`, which sets the next model's pass-state OR word
(+0x1FC) and AND mask (+0x200 = ~0) (`@ 0x5adcb5 / 0x5adcea / 0x5add16`; stored
`@ 0x5b020c..0x5b021d`); 0x300000 is z-write off + ZFUNC ALWAYS in the pass-state decode
(§Water surface), not a blend mode (corrected 2026-09-24; the earlier text read it as
"additive"). Sun/moon load plain. The additive blend is the authored FF_ST_AD_LUM
material's.
`Render_SkyboxLayers @ 0x5ac230` submits the glare/sun models at camera + offsets (+64/+16
units, alpha `0x2000`, submit flag `0x110`); `Render_SkyboxSunGlow @ 0x5acd00` drives the
glare with 8 jittered terrain raycasts feeding a ±16/frame brightness hysteresis
(target = 32 × visible rays) and `Environment_ComputeSunGlareAndFogBlend @ 0x5ad610` computes
`dot(view,sun)^32 → glare (×192, clamp 255)` and `dot^128 → fog whitening (×40, clamp 40)`.

## Water surface (`Render_WaterSurface @ 0x5c32c0`) — witnessed at the ENG-2 water leg (2026-07-06)

The pass was hiding as a SPLIT function: an 8-byte header (`sub esp, 68h` + a call to
the scar/decal setup `@ 0x58aa80`) fell through into unclaimed code — merged and named
`Render_WaterSurface(is_underwater_view, is_reflection_subpass)`. Callers:
`FrameFX_RenderGlowSource @ 0x582a5d` and the terrain pass wrapper `@ 0x610650`. Flow:

- **Side gate**: bail when `g_EnvWaterHeightFixed == 0`, or when the camera is on the
  wrong side for the requested view (`is_underwater_view` ? camera must be below :
  above — the surface renders from either side with its own blend mode + texture set).
  Both sides are strict: the above-water call skips on `jle` (`@ 0x5c330a`) and the
  underwater call on `jge` (`@ 0x5c32fc`), so at cam.z == wh neither call draws
  (`env::water_surface_sides`, 2026-09-24).
- **The bloom pass's nightvision redraw (2026-09-24, "Port the water strip's texcoords,
  depth and nightvision redraw").** `FrameFX_RenderGlowSource` calls
  `Render_WaterSurface(0, 1)` (`@ 0x582a59..0x582a5d`) unconditionally: no
  `g_WaterActive` or Blink gate, only this function's own gates (water height nonzero,
  camera strictly above water `@ 0x5c3304`, detail > 1). It regenerates the noise pair and
  marches the above-water rows with nightvision = 1 (the flat 0.1 base `@ 0x5c2d5a`, the
  specular RGB dropped `@ 0x5c2ef8`, mode-2 black fog, `g_WaterShaderBlendNV`). It never
  draws underwater, so a Q3 "underwater Replace" branch does not exist. Port: `Water`
  owns a second strip (`WaterNightVisionStrip`, layer mask 0, the typed Q3
  `WaterNightVision` source) marched with the NV row colours every frame the camera is
  above water, independent of the beauty pass.
- **Per-frame noise textures** (`Water_GenerateNoiseTextures @ 0x5c0360`, was misnamed
  `generate_terrain_noise_textures`): pass 1 animates the static 128×128 field through
  the sine LUT — per byte `lut[(uint8)(field + (counter << (field & 1)))]`, two speed
  classes (odd bytes advance twice as fast). Pass 2: toroidal 9-tap kernel
  (3× the four corners + 4× the center cross, `>> 5`), folded to a ridge intensity
  `i = max(0, 128 − |k − 128|)`, packed `R=G=B=i`, `A = 255 − max(0, i²>>9)`. Pass 3:
  the DuDv/normal map — per pixel from the intensity byte,
  `R = wrap8(2·satsub8(c − c_up) + 0x80)`, `G` the same against `c_left`, `B = 0xFF`,
  `A = 0` (the MMX `psubsb/paddsb/paddb 0x008080FF` chain `@ 0x5c07c2..0x5c087d`).
  Both textures upload every frame. The animation counter is `g_EntityUpdateCounter` (the read
  `@0x5C0366`), the ENTITY-UPDATE counter, not a render frame counter: its one writer is
  the tail of a non-epilog `Entity_UpdateAllEntities @0x4C2100` (`@0x4C2639`), so the noise
  and the wave phase advance once per entity update, freeze with it and never run faster
  than 62 Hz. Wave phase `g_WaterWavePhase = counter × 0x3000000` (`imul` `@0x5C036E`, the
  store `@ 0x5c0374`). Port: the pair still regenerates once per rendered water frame, at
  `World::entity_update_counter`, which `godot/src/world/game_world_frame.cpp` feeds to
  `Water::set_noise_frame_counter` each render frame (2026-09-23; a `Water` nobody feeds, a
  preview or a test node, counts its own render frames).
- **Init tables** (`Water_InitNoiseFieldAndSineLut @ 0x5c01a0`, once from
  `Water_InitSurfaceShaders @ 0x5c19b0` — renamed at REN-4 from the kong misnomer
  `Terrain_InitShaders`; call site `@ 0x5c19f8`): field = 128×128 samples
  `2·PRNG_Next16() −
  0x10000` min/max-normalized as `((v−min)<<8)/(range + range>>8)`; LUT =
  `128 + 64·sin(2πi/256)` (truncating ftol). `PRNG_Next16 @ 0x6130a0` =
  `state = rol4(state + rol11(state)) ^ 1` (state `@ 0x31BFBB0`) — the ALGORITHM is
  deterministic but the field CONTENT depends on the shared PRNG's call history before
  terrain init (value-history quirk; the reimpl builds from the boot state 0 for a
  deterministic witnessed-faithful instance — `engine/formats/env water_init_noise_tables`).
- **The water surface material set** (REN-4, `Water_InitSurfaceShaders
  @ 0x5c19b0`): builds the two 128×128 GTextures `"w2a"` (color+ridge-alpha) /
  `"w2b"` (DuDv) over `g_WaterNoiseColorPixels`, then — gated on
  `g_WaterDetailLevel @ 0x24d2050` ≥ 2 (downgraded to 1 without caps bit 0x100
  = ps1.1) — assembles the **bump-reflection ps.1.1** (`t0` = DuDv,
  `texm3x2pad/texm3x2tex` → `t2` = the reflection RTT
  (`g_WaterReflectionTexture @ 0x28ee8d8`, written by `Water_CreateReflectionRenderTarget`), `mul_x2`
  by `v0` vertex color, `mul_x4` by `t3` = the noise color, `add v1`
  specular) plus a **nightvision variant** (luminance `dp3 (0.25, 0.60,
  0.15)` + `mad_sat` squash + remodulate), and creates four materials
  (`GfxShader_Create4TexDesc`, slots {w2b, 0, reflection RTT, w2a}):
  `g_WaterShaderBlend @ 0x28ee8c4` — **SrcBlend ONE, DestBlend SRCALPHA**
  (out = src + dst·α; blend mode 11 of `RenderState_DecodeBlendModeToD3DStates
  @ 0x680f00`), pass flags 0x30000 (specular+fog); `g_WaterShaderOpaque
  @ 0x28ee8c8` — blend off, 0x20000; and the NV pair (`@ 0x28ee8cc/d0`).
  `g_WaterDetailLevel < 2` falls back to fixed-function TSS: the blend
  material = 2 stages {s0 `MODULATE2X(reflectionTex, DIFFUSE)`,
  α `SELECTARG1(tex)`; s1 `MODULATE2X(noiseTex, CURRENT)`,
  α `MODULATE(tex, DIFFUSE)`} with the same ONE+dst·SRCALPHA blend (flags
  0x1030000 add stage-0 clamp); the opaque one = mode 0x1020600 over
  {reflection, w2a}. Also `g_WaterShaderReflectSimple @ 0x28ee8c0` (mode
  0x600 over the reflection RTT) and `g_WaterShaderAdditiveFlat @ 0x28ee8d4`
  (textureless additive-diffuse mode 0x222; consumed by
  `Render_MainScene @ 0x5c1913`). **Selection in `Render_WaterSurface`
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
- **Texcoords and the depth curve (re-graded 2026-09-24).** The noise pair samples
  texcoords 0/3 = the unprojected ABSOLUTE render-basis world x/32, z/32 of each strip
  vertex (`Render_WaterStripDetailed @ 0x5c2aec..0x5c2b00`, `flt_7DBFAC` = 1/32; the t3
  copy `@ 0x5c3095..0x5c30bf`). No texture transform applies to the XYZRHW strip.
  `flt_8412B0/B4` (scale `0.99996948 · w/(w − 0.2)` with `w` = the INTEGER part of the
  SMOOTHED fog distance, the `movsx` of `g_EnvFogDistCurrent+2` `@ 0x5c332d`; bias
  `0.2 · scale`; stores `@ 0x5c3356` / `@ 0x5c3362`) feed ONLY the per-vertex depth chains
  (detailed `@ 0x5c2c0e/0x5c2c8b/0x5c2cf7`; low tier `@ 0x5c2201/0x5c22a6/0x5c230a`). The
  cloud-scroll "offsets" `g_WaterUVOffsetUUnread/V` (`@ 0x5c33b9/@ 0x5c33db`) and
  `flt_29169E8/EC` (`@ 0x5c3379/@ 0x5c3385`) are write-only (a `.text` byte scan finds only
  those four stores). The earlier reading of this bullet (a "UV state" with the scale/bias
  as texcoord terms and the cloud-scroll accumulators plus a 32× camera term as UV
  offsets) is retracted, and so is the reimpl note that Godot world axes coincide
  componentwise with the render basis: the render basis is the `util/axes.h` x/z swap of
  the Godot world. The strip now marches in the render basis (`WaterCore` converts the
  camera in and the positions out), so uv0 and the texm3x2 bases carry retail's
  components; the old shader's (z, x) order was the right axis mapping, everything else
  (scale, bias, camera and scroll offsets, /128) was wrong (2026-09-24, "Port the water
  strip's texcoords, depth and nightvision redraw"). Ported: `water_noise_color_pixels`,
  `water_noise_normal_pixels`, `water_init_noise_tables`, `water_depth_curve`
  (`engine/formats/env/env_water_render.h`; ctest-pinned landmarks + checksums); the env
  parity vectors' water_params key keeps only the water height.
- **The strips** (both marched in projected screen space, rows advancing away from
  the camera; REN-6 full decode 2026-07-06 — the earlier "2..9 columns" reading was
  wrong: **2..9 is the adaptive ROW-march stride**, `steps = clamp(int(row_1/w ×
  500.0), 2, 9)` `[orig: @ 0x5c265a low / @ 0x5c30d0 detailed; flt_7D6FB4 = 500.0]`,
  re-derived per row so near rows pack dense and far rows stride wide). The march
  substrate (both helpers fully decoded at the witness addendum): the 40-byte
  screen block from `Terrain_ProjectSectorToScreen @ 0x5c0bf0` — the water
  plane (at the strip's height) projected at camera + horizontal-forward × 2000
  → screen origin `[0..1]`, the per-1000-units-along-view screen delta `[2..3]`
  (dy forced 1e-6 when 0), a 1000-unit reference point `[4..5]`, the normalized
  screen march direction `[6..7]`, and visibility `[8..9]` (in-viewport or
  halfplane tests); then per row `Water_ClipLineToViewport @ 0x5c0a30` intersects
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
  - `Render_WaterStrip @ 0x5c1d60` (water detail ≤ 1): 40-byte FVF verts (XYZRHW-
    style pos + fog W + diffuse + spec + 1 UV); **sin-table Y displacement** —
    noiseIndex = `g_WaterWavePhase + 0x200000`, `+= 0x55555555` per row, table index
    `>> 22` into the SHARED 1281-entry sin table + `off_849934` cos alias (the
    D-INF-4 table), amplitudes ×2⁻³⁰/×2⁻²⁹ `[@ 0x5c25c7..]`; per-vertex diffuse =
    gray brightness ×0x10101 + alpha byte; specular = `g_EnvWaterColorLit` bytes ×
    brightness `>> 8`; distance alpha `255 − dist_scaled/(fogEnd>>16)` clamped;
    murk angle term (`− g_EnvWaterMurk`, the witnessed constant chain 0.8 / 0.2 /
    0.15 / 19.2 / 96 / 128 / 255 / 300 / 400 / 0.25); per-vertex depth
    `(t × scale − bias) × rhw` (`flt_8412B0/B4`, the depth curve above, read
    `@ 0x5c2201/0x5c22a6/0x5c230a`) clamped to **[4e-5, 1 − 2⁻¹⁵ (0.99996948)]**
    (`@ 0x5c2226` / `@ 0x5c2212`); the texcoords are the world x/32, z/32 of the bullet
    above, not a depth-curve product.
  - `Render_WaterStripDetailed @ 0x5c27d0` (detail > 1; was MISNAMED
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
    `a = clamp(int(t·16711680/fogEnd_fp), 0, 255)` = 255·t/fogEnd
    `[dbl_7DBF98 = 16711680.0 = 255 × 2¹⁶ on BOTH paths, fmul @ 0x5c2dfd / @ 0x5c2e4e;
    the earlier "2²⁴" transcription corrected 2026-09-24]`, dist_alpha =
    `255 − a²/255`, diffuse =
    `(alpha_term·dist_alpha/255) << 24 | 0x10101·brightness`; specular =
    `(dist_alpha << 24) | WaterColorLit RGB × int(lerp(255(1−base),
    128(1−base), sin)) >> 8` (nightvision drops the RGB `[@ 0x5c2ef8]`);
    per-vertex depth ("fog W") = `(t·uvScale − uvBias)·(1/t)` clamped to
    **[4.0e-5 (0x3827C5AC), 0.99996948 (0x3F7FFE00 = 1 − 2⁻¹⁵)] in BOTH tiers**
    (`flt_7DBF7C` read `@ 0x5c2c35`, `flt_7C4658` `@ 0x5c2c1f`; low tier `@ 0x5c2226` /
    `@ 0x5c2212`). The upper bound is the scene viewport MaxZ, the same constant
    `Render_MainScene` clears depth to (`@ 0x5c15af`). The 2026-07-07 "correction" to
    [8.0422355e-05 (0x38A8A8AC), 1 − 2⁻¹⁴ (0x3F7FFC00)] is retracted; the original
    "[4e-5, 1 − 2⁻¹⁵]" reading was right (re-read 2026-09-24, "Correct the water strip
    constants and the noise sine LUT"). The texm3x2 row base uses
    `vbase = 1 − min(300·rhw + 0.15, 2)/256` (`flt_7DBF68` = `0x43960000` = 300.0, fmul
    `@ 0x5c2f04`); the port had 297. Batches lock
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
    `[orig: Game_StartMission @ 0x524721]`; the scene frame then re-sets far to the
    fog word + 1 every frame (see the 2026-09-24 depth port below). Same-family curves
    (far `w` vs `w + 1`, sub-1e-4 apart) + ZWRITE ON + LESSEQUAL give retail its
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
    **2026-09-24 depth port ("Map the water depth through the retail scene curve, not
    the camera's"):** the `3×10⁻⁴` pull is retired. The beauty water writes the view
    depth whose RETAIL scene-curve depth equals the strip's replica z: the scene curve
    is near 0.2 (`flt_7C3340`, stored to `g_ProjectionNearZ` by
    `Render_SetProjectionDepthRange @ 0x58ac04..0x58ac0a`; the replica's own 0.2 is the
    same `flt_7C3340`, `@ 0x5c333e`) and far = w + 1 (the same fog word + 1,
    `Render_ProcessMainSceneFrame @ 0x5ca4ba..0x5ca4d0`), the replica's far is w. That
    view depth then takes the drawing Godot camera's own clip depth, so a camera with
    Godot's default near 0.05 cannot pull the water forward. The water therefore sits
    slightly behind its true depth (geometry on the plane wins the tie, as in retail),
    and rows past t = w clamp to the far plane (never far-clipped), so fogged water
    reaches the horizon. The Q3 copy maps the same way plus the copies' `3×10⁻⁴` pull.
    The mechanism behind the far band (XYZRHW strip, clamp to the viewport MaxZ,
    LESSEQUAL against the clear depth 1 − 2⁻¹⁵) is witnessed; whether that band shows
    water or dome depends on the dome's depth, which writes none (§Sky dome).
- **The water-active predicate (witnessed 2026-08-26, the World-tick perf
  pass).** `g_WaterActive @ 0x31BC918` is recomputed EVERY frame by
  `Terrain_SetupViewAndLighting @ 0x60fe40`: `Terrain_RenderVisibleSectors
  @ 0x6090c0` resets the tracked AABB (`@ 0x609177..0x60919f`) and traverses
  every routed sector with `trackBounds = 1` (`@ 0x609263`;
  `Terrain_TraverseQuadtreeNode @ 0x608a00` then ignores the distance emit
  heuristic `@ 0x608d84`, subdivides every frustum-surviving node to the LOD
  cap and accumulates the terminal nodes' bounds `@ 0x608ddf..0x608e8c`);
  `sub_605F70 @ 0x605f70` converts the min/max triples through
  `Math_FloatToFixedPoint3_YNegated` and returns their height components, and
  the water is active iff `lo <= g_EnvWaterHeightFixed || hi <= g_EnvWaterHeightFixed`
  (`@ 0x60ff12..0x60ff1a`, i.e. the lowest visible terrain sits at or below the
  water) OR the previous frame's `g_BlinkWaterVisible @ 0x29ACE40`
  (`dword_31BC910`, `@ 0x60ff31`). Only under that flag do the reflection
  prerender and the beauty pass's noise regeneration and strip march run
  (`Render_TerrainScene @ 0x610cd9..0x610ce0`; `Terrain_RenderWaterPass @ 0x610640`);
  the bloom pass's nightvision redraw is not gated on it (see above). Reimpl:
  `TerrainFrameCompiler` tracks the same bounds (`track_visible_bounds`,
  `TerrainDrawList::visible_bounds`), the terrain leg now precedes the water leg
  in the `GameWorld` leg table, and `Water::is_water_pass_active()` gates the beauty
  strip and the mirror SubViewport (the noise pair regenerates whenever either strip
  marches, the nightvision strip included); a world with no tracked bounds
  (no terrain in view) keeps the pass live. The walk runs whatever the blink
  letters: the indoors letter gates only the traversal (`@ 0x5ca654`) and the
  sector pass (`@ 0x5ca867`), never the view setup (`Render_TerrainScene
  @ 0x5ca504`). Reimpl (2026-09-26): `track_terrain_visible_bounds`
  (`terrain_frame.h`, the same routed window and cull as the compile); a hidden
  `Terrain` still reports the frame's bounds; ctest `terrain_frame_compiler`, GUT
  `blink_reopen_edge_test.gd`.
- **Reflection pipeline (#30 internals witnessed at REN-6, 2026-07-06).** The
  reflection prerender runs BEFORE the main frame (`Render_TerrainScene @ 0x610c80`
  → `Water_ReflectionPrerender @ 0x5c2780`, ex `sub_5C2780`, gated on
  `g_WaterActive`): it packs the LIVE camera block {x, y, z, yaw, pitch, roll} and
  calls **`Render_MainScene @ 0x5c1240` — the reusable offscreen scene renderer**
  (also `Render_CinematicMultiview`). Its frame: far-plane scale (`Render_GetTargetAspectRatio`)
  → projection → the PolyTrn render context (viewMatrix copy + camera dirs ÷65536 +
  a plane block: when water is active, `plane[4] = waterHeight_float − 0.1` with
  enable flag, armed while wh != 0 (`@ 0x5c1561..0x5c1578`): the terrain's CLIP
  plane, see "Mirror CLIP" below; the below-water word ctx+0x74 is 0 (`@ 0x5c153d`), so
  the reflected pass never swaps the terrain's stage 3 to the water noise) → clear
  color ← **`g_EnvSkyfogBlock`** (`@ 0x5c1597`, passed as the clear colour
  `@ 0x5c15aa` and to the RTT select `@ 0x5c15d3`; under the indoors blink bit 0x2 the
  PolyTrn render is skipped (`@ 0x5c1342..0x5c1353`, `@ 0x5c14a7..0x5c14ab` →
  `@ 0x5c15a2`) and the clear colour is the zeroed `esi` (`@ 0x5c1474`): black;
  ported 2026-09-26: the mirror camera carries its own BG_COLOR environment fed by
  `EnvironmentState::water_mirror_clear_color` (skyfog, black under the indoors
  letter), independent of the beauty clear's thermal, waterline and NVG legs) +
  clear depth `1 − 2⁻¹⁵` (`flt_7C4658`, `@ 0x5c15af`) →
  **`GTexRT_SelectThunk(&g_WaterReflectionTexture)` = RTT
  begin** → BeginScene/viewport/proj → scar ctx → `Environment_ApplyFogAndAmbient`
  (`@ 0x5c164c`) → fog color/mode → the sky dome (`SkyDome_RenderWithSkyfog`) → the lo-res
  terrain leg (`Terrain_RenderSectorBatchLit`) → projection rebuilds + `sub_5C90A0` +
  **`Water_RenderReflectedWorldScene @ 0x5c8510`** → at water detail ≥ 2 the mirror
  dim (`@ 0x5c1727` gate, quad `@ 0x5c186c..0x5c189e`, #37) → at FrameFX quality ≥ 3
  (`FrameFX_QualityAtLeast3` test `@ 0x5c18c6`) the far depth band
  (`Render_SetViewportFarDepth` called `@ 0x5c18f4`) around **`Render_CelestialBodies(0)`
  `@ 0x5c18fb` and `Render_SkyboxSunGlow(0, 0)` `@ 0x5c1904`**, redrawn over the
  dimmed target → the second inline quad (`g_WaterShaderAdditiveFlat` = mode 0x222:
  ONE/ONE, colour and alpha SELECTARG2(DIFFUSE); pass 0x700000; XYZRHW diffuse and
  specular 0xFF000000 over (0, 0)-(512, 512), z 0.1, lighting off,
  `@ 0x5c190c..0x5c1990`): RGB unchanged, alpha saturated to 255. No consumer reads
  the RTT alpha (both ps.1.1 programs overwrite r0.a with the co-issued `+mul_x2 r0.a,
  t3.a, v0.a`, `@ 0x7DBE88` / `@ 0x7DBD28`), so it has no observable output and the
  port draws none (walked 2026-09-26) → EndScene (`@ 0x5c1990`) → restore +
  `GTexRT_RestoreThunk` = RTT end (the earlier listing put that quad before the bodies;
  its addresses follow them). `Water_RenderReflectedWorldScene` (the ex
  "Water_RenderReflectedWorldScene three flushes" DECOMPILE-FAIL — a 5-byte header `call Render_ResetFixedFunctionState`
  falling through into the body; real extent `0x5c8510..0x5c860b`, ending in a tail
  `jmp sub_67CAA0`, corrected 2026-09-24 from "retn `@ 0x5c8af8`"): fog/ambient
  push (0,0) → `CTerrainRenderer_BuildLightingShaderConstants(0)` (NORMAL
  lighting — the arg-1 mirrored-lighting sub-passes belong to the MAIN pass's
  per-wave mirror, not this scene) → zeroes `g_WaterMirrorMatrix` and builds it as
  the water **CLIP-plane texture matrix** (`[+0]=0, [+0x10]=1, [+0x20]=0,
  [+0x30]=0.5−waterHeight_float, [+0x3C]=1` ⇒ `u = y − wh + 0.5` — the CLIP
  technique's TexClip1D coordinate, ref-128 alpha cut AT the plane) + arms
  `g_WaterMirrorActive = 1` → `Terrain_RenderSectorModels @ 0x5c5d30`
  (the call at `0x5c8576`; it renders `g_VisibleBuildingBatch` records —
  resolved through BMS pool 2, **buildings**, at `0x5c5d5c` — but that list
  was itself collected under the reflection pass's filterMask:
  `Terrain_CollectVisibleSectorUserpoints @ 0x5c6b60` applies `flagMatch` to
  `entity+36` at `0x5c6c32..0x5c6c39`, so above water it contains ONLY
  authored-Reflective buildings, see #30's 2026-08-16 correction) → the
  first, identically filtered entity wave at
  `0x5c857b` → flush(1) →
  below-side entities → above-side entities → flush(0) → (detail ≥ 2: foliage
  tile/LOD updates → flush(0)) → particle passes ×2 → trail strips → the coronas
  (`EffectWorld_RenderLightCoronas(1) @ 0x5c85fd`). **The camera block (witnessed
  2026-09-24):** `Render_MainScene` mirrors it ONLY while cam.z >= wh (`cmp`/`jl`
  `@ 0x5c1361..0x5c1370`; mirror `@ 0x5c1376..0x5c139c`: z' = 2wh − z
  `@ 0x5c1379..0x5c137b`, pitch and roll negated `@ 0x5c138e/@ 0x5c1390`, yaw kept);
  below the plane it copies the block unchanged (`@ 0x5c13f6..0x5c1414`), and the
  underwater strip rows then sample that RTT with V flipped (`@ 0x5c306f..0x5c3085`).
  **The reflected pass's fog:** `Environment_ApplyFogAndAmbient(0, 0)`
  (`Render_MainScene @ 0x5c1648..0x5c164c`; `Water_RenderReflectedWorldScene
  @ 0x5c8515..0x5c8519`): `g_EnvFogBlock` under the dry range and type, never the thermal
  grey or the underwater lit water; the sky pass sets skyfog and restores `g_EnvFogBlock`
  before the terrain (`SkyDome_RenderWithSkyfog @ 0x579ce7..0x579cf6`; the fog colour set
  `@ 0x5c1654..0x5c165a` is superseded by that wrapper). Reimpl (2026-09-24, "Port the
  water mirror's below-water eye, dry fog and clip planes"):
  `env::build_water_mirror_view` keeps the live eye below the plane,
  `EnvironmentState::build_water_mirror_fog` is published as
  `opennova_water_mirror_fog_color` / `opennova_water_mirror_fog_range`, and the shaders
  recognise the mirror pass as the one non-shadow camera whose mask omits the water
  layer (`godot/src/render/visual_layers.h`; the former `opennova_water_reflection_eye` /
  `_clip_active` globals are gone). RTT allocation is actually
  `Water_CreateReflectionRenderTarget @ 0x5c08d1..0x5c0937`:
  **256×256** for detail 2, **512×512** when `g_WaterDetailLevel >= 3` or the
  capture/special flag is set. The square target renders with the MAIN view's
  projection: `Render_MainScene` hands the main target's h/w as the projection's
  vertical scale (`Render_GetTargetAspectRatio` returns `flt_8409EC` `@ 0x5c1255`, fed to its
  `Render_SetViewAndProjectionMatrices` call `@ 0x5c163e`), so the 512 × 512 texels cover
  exactly the main field with non-square texels, 512 rows across the vertical field.
  (Re-graded 2026-09-24: the earlier "square projection with preserved horizontal FOV"
  reading and the (1, h/w) UV rescale built on it, both the 2026-07-07 FIXED note and the
  2026-07-15 view-registration correction, are retracted.) `RenderSlot_InitSystem` only initializes
  reflection direction/state; it never allocates `g_WaterReflectionTexture`.
  The selector is `if (dword_B4C3C0 || (dword_28EE8DC = 256, g_WaterDetailLevel >= 3))
  dword_28EE8DC = 512;` at `0x5c08eb..0x5c08ed` (`g_WaterDetailLevel @ 0x24d2050`),
  then `GTexRT_Construct(obj, size, size, 1, 1)`. The SHIPPED max-quality path runs
  detail 3: `Game_StartMission @ 0x524662..0x524668` copies the adapter caps
  via `sub_5899E0(0)` / `sub_676850`, and with caps 0xFDF the `@ 0x5C19DA`
  downgrade to detail 1 never fires (jo-c candidate 266 live 00TRA witness: a
  populated 512×512 target). PORTED 2026-09-14: `env::kReflectionRttSize` is
  512 (`engine/runtime/environment/water_mirror.h`), the locked max-quality path.
  Since 2026-09-26 the mirror renders into the 512 × 512 target with the main view's
  projection through a one-view XR projection (`TargetProjectionXrInterface`), the
  non-square texels `Render_MainScene` hands the RTT (`@ 0x5c1464..0x5c1482`,
  `@ 0x5c1619..0x5c163e`; `Render_SetViewAndProjectionMatrices @ 0x58d971..0x58d9de`;
  `env::build_water_mirror_view` carries the source aspect), so the strip rows'
  witnessed texm3x2 lookup (screen U, 1 − screen V) addresses it directly with no
  rescale. The 2026-09-24 round(512 × aspect) × 512 target and its device-limitation
  note are retired.

  **Mirror CLIP (2026-09-24, "Port the water mirror's below-water eye, dry fog and clip
  planes", "Arm the water mirror's CLIP technique per draw, as retail does").** Retail
  clips the reflected scene per pixel, on both camera sides, through the 4×4 `GSysClip`
  texture (`Render_CreateSystemTextures @ 0x58acc0..0x58acf6`: alpha 0 in columns 0-1,
  255 in 2-3; flags 0x140003 clamp + point + 1 mip) under AlphaRef 0x80. Objects:
  `g_WaterMirrorMatrix` row 3 = 0.5 − wh (`Water_RenderReflectedWorldScene
  @ 0x5c8540..0x5c856a`), keep y >= wh. Terrain: plane = wh − 0.1 (`Render_MainScene
  @ 0x5c1561..0x5c1578`, armed while wh != 0), texgen u = y + 0.45 − plane
  (`Terrain_RenderSectorBatch @ 0x6092c6..0x60935b`, `flt_7C6FAC` = `0x3EE66666`), keep
  y >= wh − 0.05. The CLIP technique is armed per DRAW inside the reflected pass:
  `Terrain_RenderSectorModels @ 0x5c5e57..0x5c5e75` arms a building whose Position.Z +
  graphicModel(+0xB0)->(+0x28) (the CMDL header bbox z-lo) lies below wh − 0x4000
  (wh − 0.25); `Terrain_RenderSectorEntities @ 0x5c7c1a..0x5c7c2e` arms an entity whose
  Position.Z − boundRadius (entity+0) lies below wh; the BySide waves (persons) never
  arm, nor do the sky bracket's celestial draws. An unarmed draw keeps NORMAL in the
  mirror (full SELFLUM/detail/fog, no clip). Port: the shaders discard in the mirror
  pass; `env::water_mirror_clip_armed`; `ObjectModel` re-tests on move / plane change and
  stamps `u_entity_light.w` bit 2 (bit 1 = the BySide wave); static rows carry the test
  as an origin offset in their lane's w (`env::water_mirror_clip_origin_offset`);
  `world::model_bound_floor_q16` is the CMDL header z-lo. The earlier "no oblique clip
  plane, the reimpl does not clip" and "wh − 0.1 tracked approximation" statements
  (#30) are retracted.
- **Water height precedence** (witnessed): the `.env` parse writes
  `g_EnvWaterHeightFixed` first (`Game_LoadTerrainDuringConnect @ 0x52073b`), then
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
| water height s16 `g_BmsWaterHeightOverride @ 0xa76268` | attrib bit 0x1 | `(v << 15)` → `Terrain_Init` water height (else 0) |
| fog level dword `g_BmsFogLevelOverride @ 0xa7626c` | attrib bit 0x2 | `g_EnvFogLevelFixed` (@ 0x525383) |
| fog color packed `g_BmsFogColorOverride @ 0xa76270` | attrib bit 0x4 | fog block parsed target (@ 0x525393) |
| water color bytes `g_BmsWaterColorOverride @ 0xa762c6..c8` | any byte nonzero | `Environment_SetWaterColor @ 0x57d510` |
| murk byte `g_BmsWaterMurkOverride @ 0xa762c9` | nonzero | × 0.01 → `Environment_SetWaterMurk @ 0x57d4f0` |
| start TOD s16 (8.8 h) `g_BmsStartTimeOfDay @ 0xa76418` | local play | `<< 16` → `Environment_SetCurrentTime @ 0x57c4b0` |
| day length s16 (minutes, min 60) `g_BmsTodRateMinutes @ 0xa7641a` (was misnamed `Bms_WaveAmplitude`) | local play | `Environment_SetTodAdvanceRate @ 0x57d170` (was misnamed `Terrain_GenerateWaterNoiseTextures` — it never touched textures): `g_EnvTodAdvancePerTick = 0x18000000 / (3720 × max(v, 60))` |
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
  `WeatherCore`, divergence #17 FIXED below); the witnessed tick order is
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
    `Render_MainScene` → `PolyTrn_DrawTileOverlayQuad @ 0x604700`): **LIVE** — half tint is the
    DIFFUSE on all four vertices of the `.til` tile-overlay quad (tri-strip FVF
    `0x2C4`, SPECULAR `0xFF000000`, composited into the 128-slot tile texture RT with
    `COLORWRITEENABLE=RGB`); combine pass `0x631` runs stage0 TEXTURE × DIFFUSE with
    MODULATE2X when caps `dword_32656AC` (default true) else MODULATE. The default
    tint renders 254/255 per channel — near-identity, one LSB dark, NOT exact.
  - `Terrain_SampleColorMapTinted @ 0x606030` is called inside
    `Foliage_GenerateInstances_0 @ 0x5ffdd0` and computes the FULL-tint form, but
    the fresh 2026-07-13 instruction trace follows the output record through its
    final write: the packed diffuse is overwritten by
    `clamp(int(source_y×128),0,255)<<16`, the detail wind bend carrier. The sampled
    color therefore has no rendered foliage effect. The previous LIVE foliage-tint
    conclusion stopped at the internal sample around `0x600197` and was wrong.
  - Vestigial: a runtime getter/setter pair (`Env_GetTerrainColorPacked @ 0x57d4b0`,
    setter @ 0x57d4c0) with **zero callers**, and a `(color & 0xC0C0C0) != 0xC0C0C0` mode
    flag write in `Terrain_Init @ 0x60fc66` whose target (`0x31beae8`) is never read.
- `terrain_rgb`'s reciprocal `32640/component` (clamp 255, 128-if-zero) in
  `g_EnvTerrainColorRecip @ 0x26c67f8` has exactly one consumer — **confirmed sole** by the
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
| 8 | envscale applied at interpolation to keyframes only; original bakes at parse into every `*_rgb` (positional, leaks across files in a load) | **PERMANENT (register 2026-08-30, class C)**: Config and raw `EnvFile` getters preserve authored values for round-trip; the `MissionEnvironment` engine view applies envscale at byte quantization to water/lightning and hosted ceiling/cloud/floor targets (equivalent when envscale precedes all colors, which the corpus sweep validates; reproducing the positional bake would re-manufacture the cross-file stale-scale bleed). `terrain_rgb` remains intentionally unscaled, matching retail |
| 9 | fog→skyfog mirror via per-keyframe flag vs `0xC0C0FF` value sentinel over leftover slots | **PERMANENT (register 2026-08-30, class C)**: flag semantics (equivalent for well-formed files; the sentinel's only extra behavior is accidental leftover-slot bleed — stale-state bytes not replicated) |
| 10 | `sky_map*` `.pcx` coercion not applied in reimpl parse | **Fixed** at texture-resolution layer (names stored verbatim for round-trip) |
| 11 | Negative color components: original packs garbage (no lower clamp) | **PERMANENT (register, class D)**: clamp to 0 (no UB replication) |
| 12 | Default sky_height raw-200 quirk (≈0.003 units) | **FIXED (faithful)**: the reimpl default mirrors the quirk (comment-pinned); the authoring template's 175 is authoring-side only |
| 13 | `vertex_rgb` parsed by reimpl, ignored by retail JO | **FIXED (faithful-unconsumed)**: retail JO ignores the keyword and the engine view ignores it identically (modulator identity); parsing kept for byte round-trip |
| 14 | Sun glare occlusion — the witnessed model (re-grilled 2026-07-06) is TWO jittered rays per frame into an 8-bit sliding window + dead-band hysteresis, not 8 rays at once; the glow alpha is the `dot⁴/2` chain, not `dot³²` | **FIXED 2026-07-06 (the celestial leg)**: `env::glare_occlusion_tick` + `GlareOcclusion` + the Celestial terrain ray march (32-unit-step bilinear stand-in for the lo-res DDA `@ 0x60cb80`, flagged for ENG-3); ctest + `celestial/occlusion`/`celestial/glow` vectors pin it |
| 15 | Thunder sounds on lightning timer epochs | **FIXED 2026-08-30 (the weather port)**: the sequencer epochs surface as `WeatherTickEvents::thunder_a/b`, the kernel logs `world::WeatherSoundEvent`s (1 m centred / 10 m behind, bearing 128) and `MissionAudio.play_weather_sounds` plays the THUNDER set around the listener; the WAC `flash`/`farflash` handlers ARE `Env_TriggerLightningFlashA/B` (the record's "unreachable" note corrected) |
| 16 | `.trn`/`overcast.def` first-pass TOD table + overcast cross-fade | **FIXED 2026-08-30**: `EnvironmentState` carries the overcast table (`overcast.def` through `MissionEnvironment::set_overcast_data`) and cross-fades per tick by the weather's pre-spring overcast blend (`env::blend_tod_states`, the 63356 snap included); the `overcast` WAC drives the blend through the one weather home. Stock `.trn` files carry no TOD blocks (the retail first pass would append theirs; a `.trn` with `tod_begin` blocks is not modeled — none ship) |
| 17 | Iris auto-exposure (modulator gain) | **FIXED 2026-07-06 (REN-5 — the modulator chain went LIVE); full sixteen-block set completed 2026-07-21.** `env::ModulatorChain` ticks modulator-2 → modulator → all fourteen color blocks in witnessed same-tick order `[orig: Environment_UpdateWeatherTick @ 0x57ef97..0x57f03c]`; the modulator chases the iris target over 62 ticks (`WeatherColorBlock::set_step_deltas` `[orig: ColorBlock_SetStepDeltas @ 0x57d940]`), and the ÷64 render scales reach consumers. Skyfog, ceiling/cloud/floor, and the six sky/cloud dome ramps join fill/sun/fog/sky; fog/skyfog operate undoubled through the block pass, then horizon-blend and double at the render tail `[orig: @ 0x57f037..0x57f1b1]`. The static cloud current feeds the flat dome and pre-modulated ceiling/floor feed indoor iris samples. D-RLIT-2 retains only its bounded geometry residuals; see [render/render-lighting-re.md](../render/render-lighting-re.md). |
| 18 | Earthquake / rain / wind oscillator rings | **FIXED 2026-08-30**: the quake jitter over pools 0/1 with the per-entity PRNG re-roll and the camera-shake arm (`WeatherState::apply_quake_jitter`, `PlayerViewState::shake` sampled into the FP view), the rain/snow drop pool end to end (§Precipitation), the `wind` named value writable into `g_EnvWindScale`; the WAC weather handlers all land through `EntityCommands` |
| 19 | `terrain_rgb` terrain-stack consumers | **FIXED; corrected by foliage re-grill 2026-07-13.** The observable renderer consumer is the `.til` tile overlay (`EnvFile.tile_overlay_tint_factor` → `u_tile_overlay_tint`, MODULATE2X over HALF, 254/255 at default `[orig: PolyTrn_RenderTile @ 0x60df0d]`); the reciprocal remains live in `EffectWorld_TickInstancesAndLightScale @ 0x5aa170`. The texture bake is dead. `Terrain_SampleColorMapTinted @ 0x606030` executes inside the detail generator, but its result is overwritten by the source-height bend byte before vertex emission, so foliage terrain tint is not observable and the removed reimpl `terrain_tint` property was an invented consumer. Untinted terrain remains faithful. |
| 20 | Sky dome combine | **FIXED by C7; upload-scale corrected 2026-07-14; reverse-Z facet closed 2026-07-15; skyfog seam corrected 2026-07-21**: `sky.gdshader` + the sky presenter (now `godot/src/env/sky_dome.cpp`) structurally port the recovered two-pass gradient/cloud chains, builder dome, per-pass anchor, textureless flat path, and reverse-Z conversion. Six packed sky/cloud constants upload at **2/255** and saturate at the vs.1.1 outputs. Dome VS fog now consumes the final post-horizon-blend doubled skyfog selected by `SkyDome_RenderWithSkyfog`, matching the frame clear; world passes retain ordinary fog. **2026-09-24 (R9-2, R9-3, rendering parity pass):** the dome drew with depth write and test on, hiding distant terrain and aircraft at altitude; it now writes no depth and is far-pinned (pass flags 0x300000 `@ 0x579883/0x579ac1/0x579c3e`), and the cloud pass is a second Godot pass after the bodies (`sky_clouds.gdshader` on `kRungSkyClouds`, the gradient on `kRungSkyDome`), so the clouds cover the sun and moon as in retail. NVG dims and thermal whitens the dome (§Sky dome, "NVG and thermal dome"). **2026-09-27 (OT-E6):** the dome is placed in the Godot world through the render-basis swap, so the cloud UV axes and the scroll's camera term follow retail's, and the dome fog factor is saturated per vertex (§Sky dome, "Cloud texture axes and dome fog saturation"). |
| 21 | skyfog frame clear color | **FIXED 2026-07-05**: the horizon blend is ported engine/formats/env-first (`horizon_blend_skyfog`, byte-exact vs the MMX sequence, ctest-pinned + parity-vector cell) and consumed - `MissionEnvironment.get_frame_clear_color()` drives the GameWorld `ClearColor` WorldEnvironment (above-water skyfog blend / underwater lit-water, the witnessed choice); the alternate-fog view (= the thermal-scope byte, modeled 2026-09-10 through `EnvironmentState::set_thermal_view` → grey 0x808080 clear + fog, the 0.5 world block and the terrain ramps) and the dome-fog application ride their subsystems. **REN-7 close-note (2026-07-07)**: the consumer was WRITTEN but never RENDERED — the `ClearColor` Environment shipped `background_mode = BG_SKY` (Wave-1) with no Sky resource, which Godot draws as BLACK while ignoring `background_color`; every runtime view carried a pure-black dome-rim seam row (aerial views a black band; #29's grazing fade exposed it in water-horizon views — D-TERRAIN-3's substance). Fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` (Godot ambient must never inject into the witnessed lighting model), GUT-pinned in `game_world_test`. Rendering the clear exposed a SECOND facet — the 2026-07-05 port's "undoubled = the non-modulate2x device path" reasoning was INVERTED for this reimpl: since D-RMAT-7 the reimpl reproduces the **MODULATE2X** device's framebuffer bytes (the ×2 combine + doubled fog table, calibrate-proved), and on that path the Clear consumes the post-blend DOUBLED skyfog VERBATIM (the halving `(c>>1)&0x7F7F7F7F` `[orig: @ 0x67715d]` is the non-modulate2x fallback, no reimpl analog); the undoubled clear rendered the below-rim band at exactly half the fogged dome rim ((77,91,138) vs (151,179,251) measured at noon). `get_frame_clear_color()` now doubles-with-saturation after the blend (the witnessed order: blend `@ 0x57f037..0x57f0a1` THEN double `@ 0x57f1b1`); the underwater branch was already render-space (`g_EnvWaterColorLit` = the ×2-gain `>>7` form); and the blend's distance input corrected from the load-time parsed field to the smoothed current (`get_fog_level()` ladder — the witness reads `g_EnvFogDistCurrent @ 0x26c681c`; #27's "every consumer" claim now actually holds for the clear). Env vectors re-dumped (the frame-clear token only; the input correction moves no corpus row — the bare-env grid never drives the smoothed scalars). The witness gap closed with it: the below-rim region is **clear-only in retail** — no skirt/ring geometry exists anywhere in the frame walk; the terrain surface pass (`Terrain_RenderMainSectorPass @ 0x610ac0`: it draws no sky, re-read 2026-09-24; → `Terrain_RenderSectorBatchLit @ 0x60c670`, ex `sub_60C670`) is the plain fogged sector batch bracketed by `D3DRS_AMBIENT=0xFFFFFF`/`LIGHTING=1`, and the seam invisibility mechanism is convergence-in-the-same-block (the dome pass fogs toward the DOUBLED SKYFOG while drawing the dome `[orig: SkyDome_RenderWithSkyfog]` — the same value the clear paints; terrain/water fog out at the 1024 fog reference = the dome rim radius) 2026-09-16 review: the Godot BG_COLOR sRGB decode is compensated at the device property write; windowed dome/background pixel regression and both 00TRa/00TRg captures pass (Sky dome section). **2026-09-24 (R9-5):** the reimpl's indoors black clear cited `@ 0x5c1597`, which is the water mirror's clear (skyfog, black under the indoors blink bit, §Reflection pipeline), not the beauty frame's; the beauty clear has no blink leg, thermal ? `0x808080` : eye strictly above water ? skyfog : lit water (`@ 0x5ca771..0x5ca792`; the `jle` `@ 0x5ca790` keeps the lit water at eye == waterHeight). `EnvironmentState::frame_clear_color_for` now selects exactly that; the indoors black leg is deleted ("Split the dome's cloud pass after the bodies and port the sky pass gates"). |
| 22 | Weather PRNG carry: the GDScript port added bit-31 (0/1) where the original's cdq/and/add idiom adds `0x1ABB09` on a negative rotate — a Hex-Rays transcription bug (signed `(next >> 31) & 0x1ABB09` re-typed unsigned) that silently forked the sequence from the first negative rotate | **FIXED 2026-07-05 (minted-and-closed at the ENG-2 port)**: `env::WeatherOscillator::reroll` implements the signed idiom `[orig: Environment_UpdateWeatherTick @ 0x57e9fc..0x57ea16]`; seed `0x12333333` — the mov imm32 `[orig: @ 0x57d2ff in Environment_SnapStateToTargets @ 0x57d1e0]` (the port initially transcribed it `0x12345633`; corrected as #25); the ctest pins the witnessed word sequence and explicitly guards against the bit-31 variant. Invisible to the sampled sway vectors (the spring saturates), so no wa/wb key moved for THIS fix alone |
| 23 | Lightning long-sequencer epochs were max-combined (`maxf`) in the GDScript, holding a C8 plateau; the original SETS each epoch level (the witnessed staircase C8 C8 C8 96 96 C8 C8 96 64 32 32 00), and the additives are integer-truncated bytes, not floats | **FIXED 2026-07-05 (minted-and-closed)**: `env::LightningSequencers` + `lightning_additives_packed` port the SET semantics and the exact pmullw/psrlw byte math `[orig: Environment_UpdateWeatherTick @ 0x57ec6f/@ 0x57ed0a; Environment_SetLightningFlash @ 0x57d320 — which also zeroes the directional-light slot]`; parity vectors `wc/long_seq` + `wc/long_k01/k09/k12` re-dumped with this witness |
| 24 | The Weather wind model vs the witnessed engine: (a) `wind_strength` mapped 0..100 onto 0..8192, but the oscillator's `15*prev` feedback term is stable only for intensity <= 273 — the old mapping drove the 32-bit state divergent and "survived" via GDScript's 64-bit wrap + clamps; (b) the default was still air, where retail runs `g_EnvWindScale = 256` constantly (`[orig: Environment_InitDefaults @ 0x57c1d1]`, its ONLY writer — the ambient foliage sway every retail map has); (c) the smoothers chased their own written-back output instead of the TOD keyframe targets (`[orig: Environment_ComputeTimeOfDayColors @ 0x57de40]` refreshes every block's target slot each frame) | **FIXED 2026-07-05 (minted-and-closed)**: strength now maps 0..100 → 0..256 with default 100 (= the retail constant); the duration/decay gust remains an OpenNova authoring extension, now armed-only (unarmed wind never decays, matching the constant-WindScale witness); Weather feeds `get_*_target()` keyframe targets into WeatherCore. Parity vectors `wa/*`, `wb/*`, `we/*` re-dumped under these witnesses (`wa` now IS the witnessed ambient-256 series, cross-pinned byte-equal in `env_render_unit_test`) |
| 25 | Weather-PRNG seed transcription: the reimpl carried `0x12345633` (engine/formats/env slice 1, inherited from engine/runtime/wac); the binary's immediate is `0x12333333` — the SAME constant seeds the WAC RNG (`mov dword_C6EA40` `[orig: WacScript_InitAndLoad @ 0x4f966b]`), where the mistranscription originated | **FIXED 2026-07-06 (minted-and-closed at the ENG-2 sky-leg re-grill)**: `WeatherOscillator.prng = 0x12333333` `[orig: seed imm32 @ 0x57d2ff]`; the WAC VM's `next_rand` ALSO carried #22's unsigned bit-31 carry — both engine/runtime/wac bugs fixed in the same commit `[orig: rol9 + sar/and/add @ 0x4f5a83..0x4f5a91]` (no committed test pinned the wrong WAC stream). env ctest word/wind pins regenerated; the sway-bearing GUT keys (`wa/k004..k256`, `wb/k016..k096`, `wc/long_k*`, `we/k*`) re-dumped under the witness — `wa/k001` is seed-invariant (both seeds share low-12 bits at tick 1); every level-only, color, float, and non-weather key unchanged |
| 26 | Cloud-scroll consumption model: the float GDScript (a) skipped the rate RAMP — the snap refreshes only the TARGET (`@ 0x57d2da`) and the live rate smooth-eighths toward it (`@ 0x57eecc`), so a fresh scene ran full-rate from tick 1; (b) added the accumulator term POSITIVELY on both UV axes where the witnessed texture transform NEGATES it on U (`@ 0x5791de..0x579260`); (c) the libs field labels had the layer-1 u/v pair inverted (value-equal — both advance at rate) | **FIXED 2026-07-06 (minted-and-closed at the sky binding slice)**: `WeatherCore` owns `CloudScrollState` ticked at the witnessed tick tail; `SkyDome`/`Water` consume through the weather seam (`get_cloud_uv_offset1/2`, `get_cloud_uv_rate_per_second`; standalone hosts fall back to a private core — one math home); the UV translation is `env::cloud_scroll_uv_offsets` (U-negative). `sky/k001`/`sky/k064` re-dumped under the witness; `sky_dome_test` pins the seam + the U sign **2026-07-21 cadence correction:** GameWorld owned a separate fixed 62 Hz accumulator from the 62.5 Hz mission simulation. **2026-08-30 one-clock correction:** that separate clock was itself a divergence — retail runs `Environment_UpdateWeatherTick` once per drained 16 ms quantum right after the entity update (`Game_ProcessMainFrame @ 0x52674b -> @ 0x526774`); the weather now rides the simulation tick (`MissionKernel::tick_weather` -> the render owner's hook), standalone owners bank at the same 62.5 Hz. Water noise generation intentionally remains once per rendered water frame. |
| 27 | Smoothed scalar spring channels unwired: the tick smooth/spring-steps fog distance (`g_EnvFogDistCurrent @ 0x57ede2`, consumed by the dome fog c9 `@ 0x5792c2` and `Environment_GetFogEndDistance`), sky height (`g_EnvSkyHeightCurrent @ 0x57ee97`, gating the dome rebuild `@ 0x57e4f4`), camera FOV (`@ 0x57ee78`), the now-identified `Env_SunDimPct` channel (`0x26c6830` family — dims the sun body + glare, default 0, no parser writes it), the rain percent (`g_EnvRainPctCurrent @ 0x26c6880` family — identified at REN-6, see the weather-tick smoother list), and the overcast blend (#16's runtime facet, `@ 0x57ef62`); the reimpl's consumers read PARSED `.env` values — a TOD/env scrub snaps instantly where retail ramps in | **FIXED (2026-07-06, the REN-6 port leg)**: `env::EnvScalarChannels` ticks the witnessed springs in-order between the sequencers and the color blocks (`WeatherCore.scalar_channels`, ctest-pinned steps `((d+31)>>5` / eighth-snap); targets refresh from the PARSED values each tick and the mission snap touches targets only — currents always ramp `[orig: @ 0x57d1e0]`; the smoothed currents write back through the env seam (`MissionEnvironment.set_smoothed_scalars`) so EVERY consumer (dome c9/u_sky_height, the water depth curve (the "water UV state" of this note, re-graded 2026-09-24), object/terrain fog ends, the #21 frame clear) serves the ramp; the SunDim channel is live end-to-end (celestial sun alpha + glare fold). **2026-08-30:** the rain% channel drives the drop pool and the rain ambient, the overcast channel the two-table cross-fade and the fog end, the per-channel step/max clamps are `Environment_MissionStartInit`'s recovered values (the `EnvScalarChannels` defaults) and every timed WAC step lands through `WeatherState::command_*`; the sun-dim channel is live but inert exactly like retail (no max writer). **2026-09-08 FOV follow-up:** the FOV current/target now live in these scalar channels and take the original eighth step. WAC, scope, optical visibility and player resets share that target; the main camera reads the current. See world-wac-ai-re.md section 33.23. |
| 28 | Water-height precedence: the reimpl ladder ran override → `.env` → terrain-fallback (env wins over terrain); witnessed retail order is BMS > TRN (bit-31-flagged store at `Terrain_Init @ 0x60fcb5`, AFTER the env parse) > ENV | **FIXED 2026-07-06 (minted at the water grill, closed at the water binding slice)**: the Water ladder reordered — a flagged terrain height beats the `.env` one; the reimpl override rung stays on top as the authoring seam **2026-07-21 lifecycle correction:** the reimpl ladder is direct authoring > explicit BMS (including zero) > signed nonzero TRN > ENV; an authoritative zero disables the surface, shader split, and reflection RTT instead of leaving the packaged scene's former 10.5-unit phantom plane. |
| 29 | Water surface tessellation: witnessed = screen-marched adaptive strips from the camera (3 vertices per row — left/mid/right of the projected span; adaptive ROW stride `clamp(int(row_1/w × 500), 2, 9)` — the earlier "2..9 columns" reading corrected at REN-6; 1024-row cap; ≤5-row strip batches stepping 4 through static index tables; the low-detail path adds sin-table Y displacement from the shared D-INF-4 table) `[orig: Render_WaterStrip @ 0x5c1d60; Render_WaterStripDetailed @ 0x5c27d0]`; the reimpl draws a static camera-snapped 65×65 plane | **FIXED (2026-07-07, the REN-6 tail)**: the DETAILED tier is live end to end — `env::water_*` structural translation (screen block + row clip + adaptive stride + backstep hunt + 1024 cap + row colors + batch table; 40 ctest pins), `WaterCore.strip_*` packed-array bridge (godot==render componentwise basis, D3D row-vector view rebuild), `godot/src/env/water.cpp` per-frame strip ArrayMesh (COLOR = row diffuse, CUSTOM0 = depth/rhw/screen UV, CUSTOM1 = specular; witnessed batch-table indices), and `water.gdshader` runs the witnessed ps.1.1 chain (§The strips addendum — alpha = noiseA × diffuseA × 2, reflection ×2 diffuse ×4 noise + specular; `u_water_color` was the historical t2 stand-in, replaced by #30's live RTT). Goldens re-pinned (`water/mesh` strip invariants, `water/strip` basis pin; `water/snap` retired). **2026-07-07 fidelity-grill facets (user-reported draw distance + shoreline z-fight)**: the far-water amputation was #34's misported ref-32 discard (re-graded there — the water passes never enable the alpha test) plus the wrong fog curve (now the witnessed spec-alpha `dist_alpha` factor, see §Depth/fog/pass-state); the shoreline flicker was the unhosted depth model — retail writes a projection-replica depth (ZWRITE ON, deterministic same-curve shoreline), hosted as `depth_draw_always` + the tracked relative `3×10⁻⁴` view-depth pull (the rejected 2⁻¹⁵ NDC form overpainted far-altitude terrain); the underwater opaque `0x20000` swap is now HOSTED (`u_underwater_view` → `ALPHA = 1` under the premul pass = blend-off; selection fed per frame from the camera side). The LOW tier (detail ≤ 1, sin-displacement, `Render_WaterStrip @ 0x5c1d60`) is unreachable in the ported configuration: `g_WaterDetailLevel` is the session copy of cfg waterQuality (`Game_ApplySessionSettingsToGlobals @ 0x55156a..0x551574`, clamped to [1, 3] by `Settings_ClampGraphicsOptions @ 0x54d51b..0x54d52d`), dropped to 1 only on a device without ps.1.1 (`Water_InitSurfaceShaders @ 0x5c19da..0x5c19eb`); OpenNova pins WATERQUALITY 3 on the pixel-shader path (`engine/runtime/menu/options_policy.h` kVideoQualityControls), so the LOW tier is unported by scope (2026-09-26). The high-detail nightvision redraw is hosted by the typed `WaterNightVision` Q3 producer in the terminal compositor. **2026-09-24 rendering parity pass (FIXED):** (a) the strip constants were mis-transcribed and are corrected: the depth clamp is [4.0e-5 (`0x3827C5AC`), 1 − 2⁻¹⁵ (`0x3F7FFE00`)] in both tiers (the 2026-07-07 "correction" is retracted), the texm3x2 vbase multiplier is 300 (`flt_7DBF68` `@ 0x5c2f04`, not 297), and the distance-alpha scale is `dbl_7DBF98` = 16711680.0 = 255 × 2¹⁶ (not 2²⁴) ("Correct the water strip constants and the noise sine LUT"); (b) the texcoords are the absolute render-basis world x/32, z/32 and the strip marches in the render basis (the "godot==render componentwise basis" above was wrong; the `util/axes.h` x/z swap applies), `flt_8412B0/B4` feed only the depth chain, and the nightvision redraw is a second strip `Water` marches every above-water frame without the `g_WaterActive` gate ("Port the water strip's texcoords, depth and nightvision redraw"); (c) the tracked `3×10⁻⁴` view-depth pull is retired: the water writes the view depth whose retail scene-curve depth (near 0.2, far = w + 1) equals the strip's replica (far = w), mapped through the drawing camera's own clip depth ("Map the water depth through the retail scene curve, not the camera's"); (d) at cam.z == wh neither side draws (`env::water_surface_sides`). See §Water surface. |
| 30 | Water reflection passes exist in retail (`Water_CreateReflectionRenderTarget` RTT allocator `[orig: @ 0x5c08d1..0x5c0937]`, `Terrain_RenderWorldScene @ 0x5c93a0`, per-strip mirrored verts + 229.5 alpha scale) — the reimpl renders none | **WITNESSED-READY-DEFERRED (internals closed at REN-6, 2026-07-06)**: the offscreen pipeline is fully witnessed — `Water_ReflectionPrerender @ 0x5c2780` → `Render_MainScene @ 0x5c1240` (RTT begin/end on `g_WaterReflectionTexture`, skyfog clear, the clip plane at `waterHeight − 0.1`, mirrored sky/terrain/world/celestial/glare) with `Water_RenderReflectedWorldScene @ 0x5c8510` (the ex three-flush decompile-fail; builds `g_WaterMirrorMatrix` as the water CLIP-plane texture matrix `u = y − wh + 0.5` and arms `g_WaterMirrorActive`) — see §Reflection pipeline; the earlier "strip far-edge mirroring" reading was RE-GRADED at the 2026-07-07 port leg: the `y' = 1 − y` flip / murk skip belong to the strip renderers' UNDERWATER-VIEW arg (§The strips — a t2 texcoord flip on all three row vertices, not a reflection mirror; the ×229.5 double is the low tier's), so the reflection's strip geometry rides the RTT scene itself; the camera-mirror transform sits in the Hex-Rays-elided view-matrix section (pin at port). **FIXED (2026-07-07, the REN-6 tail — reimpl planar reflection)**: a SubViewport (same World3D, fixed retail detail-2 `256×256` RTT with square projection and preserved horizontal FOV) with a mirror camera reflected about y = wh feeds the ps.1.1 chain's t2 sampler; the mirror form negates the UP column after reflection (proper det+1 mirror, yaw kept) because the witnessed rows PIN the mapping u = screenU / v = vbase − screenV — the fragment keeps the witnessed texm3x2 dots, then applies the Godot source-to-square projection scale around UV center before sampling. **2026-07-15 view-registration correction:** that scale is (1, source height/source width); without it the main-normalized V coordinate makes reflections swim with view pitch on widescreen displays. The water excludes itself from the mirror scene via a dedicated visual layer (the witnessed prerender pass list draws no water). Sky/terrain/world/celestial mirror by construction (one world), and the dome now anchors from each render pass camera instead of reusing the main-camera transform. Residual in-row: the witnessed wh − 0.1 clip plane is a TRACKED approximation (no oblique near plane in the reimpl; the mirrored camera predominantly sees above-water geometry). The 512 square branch remains documented for a future detail-3/capture selector **2026-07-21 lifecycle/current-camera correction:** the RTT sleeps for absent, unloaded, hidden, or off-screen water and reacquires viewport camera switches; sun/moon/glare/stars relocate from each active pass camera so the mirror no longer inherits main-view anchoring. **2026-08-16 reflected-world admission correction (PORTED):** the 2026-08-05 "no buildings ever reflect" reading was INCOMPLETE, and an intermediate 2026-08-15 "all pool-2 buildings reflect" reading (never merged) was REFUTED in the same review. The truth: `Water_RenderReflectedWorldScene @ 0x5c8510` does render a sector-building pass (`Terrain_RenderSectorModels @ 0x5c5d30`, called at `0x5c8576`, resolving `g_VisibleBuildingBatch` through BMS pool 2 at `0x5c5d5c`) before the entity waves at `0x5c857b`/`0x5c8590`/`0x5c8599` — but every one of those legs draws only what `Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0` collected, and its filterMask (`camera_below_water ? 0 : 0x400`, asm `@ 0x5c9109..0x5c9119`) is applied by ALL collectors including the building walk (`Terrain_CollectVisibleSectorUserpoints @ 0x5c6b60` requires `(flagMatch & entity+36) == flagMatch` at `0x5c6c32..0x5c6c39`). `entity+36` flag `0x400` has exactly TWO writers: `Entity_InitFromModel @ 0x40e208..0x40e20a` (`ItemDefType(+0x5C)==1`, vehicles) and `Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b`, which maps the mission-authored BMS attribute `0x800000` (`bms::BmsiAttributeFlags::Reflective`) into it for every spawned pool. So above water the mirror draws vehicles plus exactly the records the mission author flagged Reflective; below water it is unfiltered; persons never draw (no player-render leg). Shipped data confirms per-record authoring: CP01 flags 217/956 buildings and 5/14 pool-1 items Reflective (the same building graphic appears both flagged and unflagged), 00TRa flags 63/926 buildings, CP12 none. Reimpl port: `placement_is_mirror_reflected(entity_attrib, item_type) = vehicle || (attrib & 0x800000)` (`engine/runtime/mission/placement_traits.h`), same-graphic static batches split per reflection policy with the split key carried through destruction routing, and the husk graft keeps the carved slot's reflect policy (destruction never clears flag `0x400`). Cross-run pixel sheets remain non-gating (water/particle phases differ between launches); the semantic admission tests are the gate. This closes semantic admission, not performance parity: Godot still batches by graphic rather than retail sector cell. **2026-09-01 mirror-terrain re-grade + foliage exclusion (PORTED):** the earlier "lo-res mirror terrain" reading is REFUTED for the poly terrain — the offscreen renderer's PolyTrn context passes the SAME quadtree quality scale as the live beauty scene (`Render_MainScene` ctx+0x54 = 1.0 `[orig: @ 0x5c154d]` == `Render_SceneWithWaterReflection @ 0x5d7ea0` ctx+0x54 = 1.0 `[orig: @ 0x5d8047]`; the scale multiplies the node-size subdivide test via `flt_319FB2C = ctx+0x54 × flt_8493D8` `[orig: PolyTrn_RenderFrame @ 0x60eb4a]`, so equal scales = equal tessellation; `NVG_RenderSceneToTarget @ 0x5d04c0`'s 0.5/1.66 belongs to the separate low-detail/no-water scene path), so the reimpl's full-resolution mirrored terrain IS the witnessed shape and its "tracked approximation" status is retired. The REAL witnessed mirror difference was foliage: the reflection context passes the foliage-collect field 0 where the beauty scene passes 1 (`ctx+0x68 = 0` `[orig: Render_MainScene @ 0x5c1522]` vs `= 1` `[orig: @ 0x5d801e]`), and that field alone gates `Terrain_CollectNearFoliagePatches` in the traversal `[orig: Terrain_TraverseQuadtreeNode @ 0x609069]` — retail's mirror draws NO near-foliage patches (and, with the visible-far key list left empty, no far foliage cells either). Ported: the foliage detail blanket rides its own visual layer (`Water::VISUAL_LAYER_TERRAIN_FOLIAGE`, bit 17, alone) admitted by the beauty camera mask (0x78C01; bit 18, the empty-sector `TERRAIN_FLAT_FALLBACK` layer, is likewise beauty-only and mirror-excluded since 2026-09-13, terrain/terrain-re.md "Empty-sector flat fallback") and excluded from `REFLECTION_CULL_MASK` above AND below water (the context field is set unconditionally — no underwater re-add); `foliage_runtime_adapter_test` pins the lone bit and the mirror exclusion. The former in-row "`waterHeight − 0.1` non-oblique clip" approximation closed 2026-09-24 (below). **2026-09-24 rendering parity pass (FIXED, §Reflection pipeline):** (a) below-water eye: `Render_MainScene` mirrors the camera block only while cam.z >= wh (`@ 0x5c1361..0x5c1370`, mirror `@ 0x5c1376..0x5c139c`) and copies it unchanged below (`@ 0x5c13f6..0x5c1414`); the reflected pass fogs with the dry `g_EnvFogBlock` (`Environment_ApplyFogAndAmbient(0, 0)`, `@ 0x5c1648..0x5c164c`, `@ 0x5c8515..0x5c8519`) and never swaps the terrain's stage 3 (ctx+0x74 = 0 `@ 0x5c153d`); ported as `env::build_water_mirror_view` + `EnvironmentState::build_water_mirror_fog` ("Port the water mirror's below-water eye, dry fog and clip planes"; the render-fixture contract expects the live reflected eye below the water); (b) the "the reimpl exposes no oblique clip plane and does not clip" and "wh − 0.1" statements are retracted: retail clips per pixel through `GSysClip` under AlphaRef 0x80 on both sides (objects keep y >= wh, terrain y >= wh − 0.05), armed per draw (sector models whose CMDL bbox floor lies below wh − 0.25, first-wave entities whose z − boundRadius lies below wh; the BySide person waves and the celestial draws never arm); ported as the mirror-pass discard with per-draw arming (`env::water_mirror_clip_armed`, "Arm the water mirror's CLIP technique per draw, as retail does"); (c) the RTT renders with the MAIN view's projection (`@ 0x5c1255`, `@ 0x5c163e`): the "square projection with preserved horizontal FOV" of the 2026-07-07 note and the 2026-07-15 (1, h/w) UV rescale are retracted; the mirror keeps the source projection, since 2026-09-26 over the 512 x 512 target through `TargetProjectionXrInterface` (the 2026-09-24 round(512 × aspect) × 512 target of "Render the water mirror over the main view's field at 512 rows" is retired); (d) the mirror's overlay pass closes with the coronas (`@ 0x5c85fd`), the dim (`@ 0x5c186c`) and, inside the far depth band (`@ 0x5c18f4`, gated `@ 0x5c18c6`), `Render_CelestialBodies(0)` (`@ 0x5c18fb`) and `Render_SkyboxSunGlow(0, 0)` (`@ 0x5c1904`), the glow at the no-occlusion alpha of the MIRROR camera's view dot (`env::mirror_glare_upl`), the discs at the beauty submit value (`renderer::kMirrorOverlayOrder`, "Close the water mirror with its dim and the far-band sky redraw"). **2026-09-26:** the mirror clear is ported: the mirror camera carries its own BG_COLOR environment fed by `EnvironmentState::water_mirror_clear_color` (skyfog, black under the indoors letter; `Render_MainScene @ 0x5c1474`, `@ 0x5c1597`), independent of the beauty clear's thermal, waterline and NVG legs. The reflection-sample brightness remeasure is #37's. |
| 31 | The water render LOOK was invented: sin/cos shader waves + Fresnel-style alpha with no witness; retail = per-frame animated 128×128 noise color + DuDv textures over `g_EnvWaterColorLit` per-vertex color, distance-alpha, murk term | **FIXED 2026-07-06 (minted-and-closed at the water binding slice)**: `water.gdshader` rewritten as a structural port of the witnessed detailed-path model over the engine/formats/env textures (`water_noise_color_pixels`/`water_noise_normal_pixels`/`water_uv_state`, ctest-pinned); the invented waves/fresnel are deleted |
| 32 | Celestial placement inventions: the reimpl placed bodies at `camera.xz + dir × 2000 × (sky_height/175.69)` with ZEROED camera height and a `dir.y > -0.1` visibility gate — none witnessed. The live renderer places at camera + dir × 64 (full height, identity rotation) with alpha folds; the 2000 belongs only to the dome VS proximity ref, the +64/+16 offsets to a dead variant | **FIXED 2026-07-06 (minted-and-closed at the celestial leg)**: placement + witnessed sun/moon alphas ported (`kCelestialBodyDistance`, `celestial_sun/moon_alpha_fixed`, EnvFile statics, vector-pinned); the depth-test flip replaces the invented gate (the world overdraws bodies like retail's draw order). **2026-09-24:** placement unchanged; the body alpha now rides UPL_INTENSITY into the authored SELFLUM material, each disc with its own value (§Celestial bodies), and the bodies are far-pinned between the no-depth dome passes |
| 33 | Star field: retail renders 256 camera-anchored billboard instances with per-star twinkle (rol4/rol11 PRNG) and a hide-near-the-light dot cull `[orig: Star_RenderField_Unused @ 0x5ad9c0]`; the reimpl renders the star 3DI as ONE body. The instance-table generator is unfound | **RE-GRADED 2026-09-24 (R9-1): retail never draws stars.** `Star_RenderField_Unused @ 0x5ad9c0` has no caller (no code xref, no E8/E9 rel32 call, no absolute pointer; §Celestial bodies, Dead variants). The StarField MultiMesh, the Celestial star legs, `env::generate_star_instances` / `star_twinkle_tick` / `star_visible_fixed`, the sky-stars ladder rung (`kRungSkyStars`) and their tests are deleted ("Delete the star field: retail never draws stars"); `star_3di` is still parsed and named, never drawn, like retail. History: the 2026-07-06 REN-6 port leg witnessed the generator and ported the renderer as live ("FIXED"): the generator witnessed (`Star_GenerateInstanceTable @ 0x5ac850`, ex `init_weather_particles` — per-star math in §Celestial bodies; sole caller `EffectWorld_LoadCelestialModels @ 0x5add40`) and PORTED — `env::generate_star_instances`/`star_twinkle_tick`/`star_visible_fixed` (ctest-pinned: the PRNG draws from seed 1, star[0] offsets, the 256-star invariants) hosted by `StarField` + a 256-instance camera-anchored billboard MultiMesh in `godot/src/env/celestial.cpp` (per-star twinkle via instance color, the 0.98 near-light cull against the active light, regenerate-per-celestial-load, the sky-stars ladder rung). The single-body stand-in and its `0x2000` dead-variant opacity are deleted. Note: the brightness accumulator's exact scale/color application inside `Matrix_BuildTransformFromParts` is Hex-Rays-mangled (x87 handoff) — brightness-as-color-modulation is the structural reading **2026-07-21 reflection correction:** instances are camera-local inside a conservative MultiMesh AABB and the additive shader billboards/reanchors them from each active pass view, preserving far-off mission cameras and the mirror RTT. |
| 34 | Water surface framebuffer blend + far cutoff: the reimpl used standard alpha blending (`blend_mix` — src·α + dst·(1−α), water opaque near / transparent far) and no alpha test; witnessed retail draws the above-water surface with **SrcBlend ONE + DestBlend SRCALPHA** (out = src + dst·α — transparent near, surface-dominant far) `[orig: Water_InitSurfaceShaders @ 0x5c19b0; RenderState_DecodeBlendModeToD3DStates @ 0x680f00 mode 11]` | **FIXED (minted-and-closed at REN-4; the alpha-test half RE-GRADED 2026-07-07)**: `water.gdshader` re-expresses the blend exactly via `blend_premul_alpha` with `ALPHA = 1 − a` and stays two-sided (pass flags 0x400000). The REN-4 "alpha-test ref 32 GREATER far-fade cutoff" half was a MISREAD: `CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419/@ 0x5c3484` latches ALPHAFUNC/ALPHAREF only `[orig: @ 0x6770a0]`; D3DRS_ALPHATESTENABLE rides pass-flag bit 0x40000 `[orig: CGfxShader_ApplyPass @ 0x68326b]`, which the water passes (0x30000/0x20000) never set — the misported `discard` amputated the far water (the user-reported short draw distance) and is deleted; the witnessed far fade is the fog convergence, not a cutoff. Residuals live in their own rows: tessellation/murk-angle chain #29, reflection RTT #30 (the underwater OPAQUE swap hosted at the 2026-07-07 facet, see #29) |
| 35 | Water sine LUT provenance: the reimpl built the 256-entry LUT at init from runtime `std::sin`; the original builds once from x87 fsin (`trunc(sin(i·2π/256)·−64)`, stored `0x80 − v` `[orig: Water_InitNoiseFieldAndSineLut @ 0x5c0308..0x5c0334]`) — ONE deterministic instance. Last-ulp libm variance (first seen on the GitHub `macos-26-arm64` runner image, 2026-07-10) flips the truncation at non-landmark indices; the flipped byte survives the LUT landmark+symmetry-sum checks but forks every downstream noise color/DuDv pixel, failing `env_render_unit`'s pinned checksums on that platform only | **FIXED (2026-07-10 — tracked decision)**: the LUT is a committed 256-byte constant in `water_init_noise_tables` — the deterministic instance every existing ctest/GUT pin was generated from (formula-identical on MSVC/UCRT x64; landmarks `0x80/0xAD/0xBF/0x80/0x41` and the 32768 symmetry sum unchanged). Runtime libm no longer participates, so all platforms render the same witnessed-faithful instance. Whether this instance byte-matches the retail x87 build at every index is unverified (needs a retail memory dump) — the same "pinned-current instance" caveat the noise FIELD already carries via the PRNG call-history quirk (§Water surface, Init tables). **2026-09-24 (R10-8, "Correct the water strip constants and the noise sine LUT"):** the committed LUT is now the SINGLE-precision instance. `Water_InitNoiseFieldAndSineLut` runs after `CGfxDevice_CreateDevice` created the device without `D3DCREATE_FPU_PRESERVE` (BehaviorFlags 0x80, then 0x20, `@ 0x67e9fd` / `@ 0x67ea35`), so the x87 precision control is 24 bits while the fild / fmul step / fsin / fmul −64 / `_ftol2_sse` loop runs (`@ 0x5c0308..0x5c0334`; step `flt_7DBD14`, amplitude `flt_7C9BD8`). sin × −64 rounds to exactly −64 / +64 at i = 64 / 192: bytes `0xC0` / `0x40` (the double-precision instance had `0xBF` / `0x41`); no other index lies within a float half-ulp of an integer (checked by emulation). Landmarks are now `0x80/0xAD/0xC0/0x80/0x40` (i = 0/32/64/128/192), the 32768 symmetry sum is unchanged, and the t=0 colour checksum is `0x5CA88BA4` (`env_render_unit`). |
| 36 | Cloud-map alpha: the reimpl's shared texture resolver decoded the sky-map PCX as opaque RGB (alpha = 1 everywhere), so the cloud pass's alpha chain saturated and the cloud layer fully covered the dome — the pass-1 sky gradient never showed through (found at the 2026-07-18 sniper/aircraft retail A/B: our 08:00/06:32 skies read as all-cloud-ramp) | **FIXED (2026-07-18)**: the sky maps load through the witnessed per-pixel palette-luminance alpha synthesis `A[i] = (85·(r+g+b)) >> 8` `[orig: Texture_LoadFromArchive @ 0x58b980 — table @ 0x58bc35..0x58bca9, per-pixel A @ 0x58bcee]` via `build_pcx_luminance_alpha_texture` + `EnvFile::_load_sky_map_texture*`; non-PCX cloud names keep the generic decode (retail's DDS-first path has no alpha synthesis). See §Sky dome, "Cloud texture load + the synthesized alpha" |
| 37 | Water reflection-sample brightness at matched pose/clock: the 2026-08-16 retail-matched CP01 side-by-side (scripted onHook-bridge pose, mission clock 15:30, identical 80°×53.45° frustum — [render/render-lighting-parity-2026-08-15.md](../render/render-lighting-parity-2026-08-15.md)) measured our open near/mid water at mean L≈175–193 vs retail L≈63–69 (≈2.7–2.8×), neutral sky-mirror vs murky green, with every cited surface-chain term verifying except the reflection sample — retail's t2 reads ≈0.25× what ours read | **FIXED (2026-08-16, same session — the witnessed mirror dim was unported)**: `Render_MainScene` does not hand the water shader the raw mirrored scene — at water detail ≥ 2 it multiplies the finished reflection RTT by vertex color `0x404040` (a fullscreen 4-vertex TRIANGLESTRIP drawn with `SetRenderState(D3DRS_SRCBLEND = D3DBLEND_DESTCOLOR, D3DRS_DESTBLEND = D3DBLEND_ZERO)` = out = dst × 64/255, then SRCALPHA/INVSRCALPHA restored) `[orig: detail gate @ 0x5c1727; blend states @ 0x5c1856..0x5c186a; quad + color @ 0x5c186c..0x5c189e; restore @ 0x5c18a3..0x5c18bf]`. The celestial bodies and sun glow draw AFTER the dim `[orig: @ 0x5c18fb/@ 0x5c1904]`. Ported as `env::kReflectionDimFactor` (engine/runtime/environment/water_mirror.h, the witness map) + a multiply ColorRect compositing over the mirror SubViewport (`water.cpp`), pinned by `water_test`'s dim test and the new `env_render_unit` texm3x2 row-constant pins (t1/t2 scales, vbase chain, 0.05 bump clamp — previously unpinned). Post-fix matched measurement at three pitches (−2°/−8.1°/−20°): waterline/mid-water luminance ratios 0.96–1.03 (were 2.5–2.7); committed-panel ratios 1.01/1.03/0.94. The pitch A/B also validated the 2026-07-15 `u_reflection_uv_scale` registration empirically (reflections stay registered at all three pitches in both engines) — candidate (a) retired. (That rescale itself was retired 2026-09-24 when the mirror took the main view's projection at the source aspect, #30.) Residuals in-row: (1) the former nearest-row D-RMAT-8 attribution is retired by the 2026-08-22 gamma-framebuffer cutover; the reflection-sample brightness, remeasured after the 2026-09-26 mirror projection and mirror clear ports, CLOSED 2026-09-27: at the cp01-water-* rows (catalog v5, fresh OpenNova captures vs the settled 2026-09-25 retail set) the open-water luma ratios read 0.96-1.03 (near, mid, waterline); the 2026-08-22 CP01 frames were fast captures (0.085 s after apply) and read 6-7% brighter over the whole frame, sky included; (2) CLOSED 2026-09-24 ("Close the water mirror with its dim and the far-band sky redraw"): the dim and the sky redraw now close the mirror's overlay pass (`renderer::kMirrorOverlayOrder` in `engine/runtime/renderer/scene_overlay.h`: coronas `@ 0x5c85fd`, dim `@ 0x5c186c`, then `Render_CelestialBodies(0)` `@ 0x5c18fb` and `Render_SkyboxSunGlow(0, 0)` `@ 0x5c1904` inside `Render_SetViewportFarDepth`'s band called `@ 0x5c18f4`, gated on `FrameFX_QualityAtLeast3` `@ 0x5c18c6`), so the mirrored bodies over the sky stay bright as in retail; the mirror glow's alpha is the no-occlusion (0, 0) form at the MIRROR camera's view dot (`env::mirror_glare_upl`), the discs keep the beauty submit value. The dim moved from the mirror SubViewport's canvas ColorRect into that overlay pass (`renderer::append_mirror_dim_overlay`, after the mirror's particle passes and coronas, before the decode terminal); (3) CLOSED 2026-09-14: the reimpl RTT is the shipped detail-3 512² (`kReflectionRttSize`, see the #30 allocator note; since 2026-09-26 the 512² RTT at the source frustum, #30); (4) CLOSED 2026-09-26: the second fullscreen quad in the same tail (`g_WaterShaderAdditiveFlat`, pass 0x700000, color 0xFF000000 @ 0x5c190c..0x5c1990) is walked, an unobserved alpha fill: ONE/ONE of 0xFF000000 leaves the RGB and saturates the RTT alpha, which no water program reads (§Reflection pipeline) |

**2026-08-29 static-batching correction to row 30.** The final historical
"per graphic" performance note is superseded: opaque and alpha-tested static
rows now use terrain-aligned 512-unit X/Z populations with exact custom AABBs.
Only blended rows remain global, intentionally, so spatial population centers
cannot perturb transparent ordering; inside one blended global population the
rows sit in swap-remove order (a crossing moves the last live row into the
hole), not slot order, so the instances of one alpha graphic blend within
that single draw in an order that depends on past crossings. Retail's
per-strip depth sort was never reproduced inside a population, so no
documented rule breaks, and this is the only other ordering caveat. Every
population (per-bin, blended global, shadow twin) hangs under the
container's one `StaticPopulations` child rather than beside the placed
models (2026-08-30): the shell's per-frame walks over the container's
children (the EffectWorld light select, the item-effect attach) then visit
the entity models and one holder instead of a population per graphic x
level x bin: `world_light` against master went from +0.51 / +0.15 / +0.10
ms to +0.17 / +0.02 / -0.02 ms (00TRa / CP01 / CP19, the closing A/B in
render-order-re.md). A graphic with more than one authored
RLOD stays in those populations: every level is emitted as its own population
over the same slot list and the retail selector picks the level per instance
each frame (render-order-re.md, "Authored object RLOD selection"), so
batching never decides which level an entity draws. Since 2026-08-30 a
population carries rows only for the slots at its level (packed dense, a
crossing moves the row between the level populations, an empty population is
hidden), so the draw-call and cull cost is one row per live instance rather
than one population per authored level per bin (1600x900, Ryzen 7735HS
iGPU, medians of per-run p50: 00TRa 696 root draws / 1.15M primitives /
15.10 ms frame before, 487 / 0.87M / 14.66 ms after; CP01 564 / 2.04M /
16.09 ms before, 416 / 1.11M / 14.84 ms after; CP19 360 / 2.76M / 17.33 ms
before, 296 / 1.52M / 17.12 ms after; master draws 487 / 376 / 284). The
final A/B against master (e17349529) on the merged head, 2026-08-30, same
machine and protocol, three interleaved runs per tree and mission, medians
of per-run p50, is the complete record for all three columns: root draws
487 -> 489 / 376 -> 416 / 284 -> 296, root primitives 1.053M -> 0.871M /
1.172M -> 1.110M / 1.944M -> 1.519M, wall frame 13.97 -> 13.19 /
14.14 -> 13.05 / 17.02 -> 14.60 ms, frame p95 15.11 -> 14.15 /
17.75 -> 14.09 / 18.12 -> 15.71 ms (00TRa / CP01 / CP19). The
bins were re-measured against one population per (graphic, policy, level,
submesh) on the same build, 2 runs each: 00TRa bins 487 draws / 0.87M /
14.66 ms vs no bins 485 / 1.05M / 14.73 ms; CP01 bins 416 / 1.11M /
14.84 ms vs no bins 376 / 1.17M / 14.89 ms. The bins keep their per-bin
frustum cull (fewer primitives, a lower root GPU time) for the same or a
slightly lower frame time, so they stay; the extra draws they cost are the
bins holding entities at two levels.

## 2026-09-24 rendering parity pass

The rendering parity pass (PR #678) re-read the sky, celestial, fog and water legs
against the IDB and ported every divergence it found; the sections above carry the
witnesses. By system:

- **Sky dome** (#20): no depth write and far-pinned; the cloud pass drawn after the
  bodies as its own Godot pass; the gradient, bodies and clouds on their own ladder
  rungs (`kRungSkyDome` / `kRungSkyBody` / `kRungSkyClouds`); the main and mirror sky
  gates; the NVG unpack and the thermal white dome (§Sky dome).
- **Celestial bodies** (#32, #33): stars re-graded as never drawn (the renderer has no
  caller) and deleted; the sun and moon draw through their authored FF_ST_AD_LUM
  material with UPL_INTENSITY per disc; the bloom copy takes `sat(SelfLumColor × gain) × 2`
  and tests the far band against the scene viewport's MaxZ; the glint and the glare draw
  at the end of the post-particle overlay stage (§Celestial bodies).
- **Fog** (§Fog policy): the overcast-folded start used as-is; type 0 keeps the 0.5 start;
  the object fog distance follows the retail pass path; the viewmodel takes the dry fog.
- **Colour blocks**: the exact `g_EnvTerrainLightCombined` word math; the blocks stay raw
  under NVG (§Color state blocks).
- **Frame clear** (#21): the beauty clear has no blink leg; the black clear is the water
  mirror's.
- **Water** (#29, #30, #35, #37): the strip constants, the single-precision sine LUT, the
  world x/32, z/32 texcoords in the render basis, the scene-curve depth, the strict side
  gates, the ungated nightvision redraw; the mirror's below-water eye, dry fog, per-pixel
  per-draw CLIP, main-view projection at 512 rows, and its closing dim + far-band sky
  redraw (§Water surface).
- **Precipitation and murk**: both draw in the post-particle overlay stage; the
  precipitation and corona shaders are deleted.

### Open after the 2026-09-24 pass

- The 03tr-sun-sky sun glow. The fixture's dome share closed 2026-09-27 (§Sky dome, "Cloud
  texture axes and dome fog saturation"): the open-sky band means sit within 0.4 level of the
  settled retail frame, and the mean absolute difference (0.59, 0.90, 0.72) is inside
  retail's own run-to-run spread (0.64, 0.96, 0.79). The saturated sun area (all channels
  255, raw x0.96) reads 4318 against retail's 4725, centroid (520.5, 381.5) vs (520.5,
  381.7). Over the modelled dome, the glow the disc and glare add is 0.6-1.0 level weaker in
  the port at every radius 20-150 px, identical before and after the dome fix. That glow is
  the remaining area gap: a celestial-glow measurement (`Render_SkyboxSunGlow @ 0x5acd00`,
  the disc's UPL), not the dome's.

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
- `dword_B763E8`, the debug page's `Loc: %i` row — unidentified; the F3 Environment window omits it.
- `Precipitation_FallTick`'s wind-origin writes (`0x2c059f8..0x2c05a00` from the view matrix) have no readers — not ported.
- **Closed at REN-4** — the pass-1 sky-gradient stage table: the effect is built
  by `GfxShader_Create1TexModeId(0, 0x20200)` (`[orig: Terrain_InitRenderingResources
  @ 0x578aa8]`) — no texture, mode word 0x200 + fog bit — and the mode decoder
  (`RenderState_DecodeModeColorStage @ 0x681080` case 0x200) emits stage 0
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
| Fog policy | **matching** (env_render port; overcast coupling included; 2026-09-24: the folded start used as-is by every consumer, type 0 keeps the caller's 0.5, the object fog distance follows the retail pass path, the viewmodel takes the dry pass fog; §Fog policy) |
| Weather tick / smoothing / lightning | **matching (2026-08-30)**: ONE weather home (`world::WeatherState`) on the simulation clock runs the clock, the oscillator, the hit blackout, the quake jitter, both flash sequencers with their thunder, the scalar springs under the recovered clamps, the cloud-scroll ramp, the two-stage iris modulator, all fourteen color blocks and the cloud-scroll tail in witnessed order; the WAC handlers, the 0x0A projection and the joiner's decoder read/write that home; the precipitation drops, the rain ambient, the overcast cross-fade and the F3 Environment page consume it. The `sunfade` channel is inert like retail (no max writer). |
| Sky dome render: scroll / VS constants / mesh / advanced_clouds=0 | **matching** (including the six-color `2/255` upload and vs.1.1 color-output saturation; verified against `Color_UnpackToFloat4 @ 0x578900`, `Render_Skybox @ 0x579080`, and `SkyDome_BuildMesh @ 0x578db0`) |
| Sky dome per-fragment combine | **matching** (C7): structural port of the recovered two-pass spec (§Sky dome) as two Godot passes (gradient; clouds after the bodies, since 2026-09-24), no depth write, far-pinned, NVG/thermal constants ported; D3D forward clip depth is reconstructed for the proximity dp3 while raster position remains native Godot reverse-Z |
| Celestial + glare | **matching for the ported scope (ENG-2 celestial leg; #33 star field re-graded 2026-09-24: never drawn; sun veil ported 2026-08-20; bodies through their authored SELFLUM material 2026-09-24)**: body placement (camera + dir × 64) + witnessed alphas (riding UPL_INTENSITY per disc into the authored FF_ST_AD_LUM material, the bloom copy at `sat(SelfLumColor × gain) × 2` and the far band tested against the scene viewport's MaxZ, 2026-09-24) + the #14 occlusion window/hysteresis/glow chain live in `engine/formats/env` behind EnvFile statics + `GlareOcclusion`; the former ENG-3 lo-res-DDA ray residual closed with #209 (2026-07-08) — the glare ray now marches the `engine/runtime/terrain_query` port via `TerrainData.raycast_terrain`. 2026-08-20: the live `@ 0x5ad8b0` consumer witnessed + ported — the fullscreen white sun veil (alpha = dot³² glare byte) and the modulator-2 exposure stop-down (see the #14 closure). Frozen captures previously starved BOTH glare accumulators (occlusion window at one advance, modulator at its reset identity) — the capture refresh now settles them at the fixture pose through the witnessed per-tick math (`Celestial.settle_glare_occlusion`, `Weather.settle_exposure` — capture-seam only, live cadence unchanged). The 2026-08-20 round also witnessed + ported the celestial AXIS map (`godot/src/util/axes.h` — the #14 closure carries the derivation), the water-reflected sun glint (`Environment_UpdateSunGlare @ 0x5ad130` — the glint leg, drawn in the post-particle overlay stage since 2026-09-24, + the veil secondary term), and the glare gate ray's start-height lift/coarse-then-fine cadence |
| BMS overrides | **matching** application semantics via EnvFile's non-persistent override layer (runtime apply on load / clear on unload; base file never mutated) |
| iris / terrain_rgb | iris **matching for the ported scope** after #17 and D-RLIT-2: the live modulator is fed by the marched three-point camera-ray average with per-sample indoor/outdoor classification and sun-occlusion rays; the smoothed ÷64 gain reaches shaders. D-RLIT-2 records the bounded entity/light-group geometry residuals. terrain_rgb **matching** after the 2026-07-13 correction to #19: tile overlay HALF×2X and the effects reciprocal are observable; the foliage FULL-tint sample is overwritten before emission, the bake is dead, and the terrain surface is faithfully untinted |
| Load pipeline overcast precedence | **matching** after C6 correction (additive-after-success; the reimpl two-table model is live since 2026-08-30 — `overcast.def` as the overcast table, the cross-fade by the weather's overcast blend) |

## What this effort shipped (historical, 2026-06-09 branch `environment-workspace`)

- `engine/formats/env`: engine-faithful parse + 16.16-hours TOD interpolation; new
  `env_render` module (fog policy, day phase, integer smoothing, lightning,
  glare, derived colors, BMS overrides) with `env_render_unit_test` and the
  `OPENNOVA_JO_DIR`-gated install sweep.
- `godot/src`: EnvFile fog/day-phase/glare/override/`to_bytes` surface +
  the color smoother (now engine-only `ColorChannelState`, ADR 0043 slice G9); engine-faithful `environment`/`sky`/
  `water`/`weather`; new `celestial` + two celestial shaders (deleted 2026-09-24, the bodies draw through their authored materials);
  BMS override fields through `MissionData`; runtime apply/clear in
  `game_world`.
- The former `godot/modtools` Environment workspace unified its preview onto
  `Water` + `EditorWeather` (PR #24 parity). ADR 0037 removed that workspace;
  only the runtime portions above remain current.
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
addresses (verified by body comparison; 18 recites applied across `engine/formats/env`, `engine/runtime/terrain`,
and the env nodes now under `godot/src/env`):

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

### Addresses verified stale (2026-08-29)

Terrain-side cites (`engine/runtime/terrain/lighting.h`) the audit could not confirm in the retail
image; the three VERIFY-pending rows were closed 2026-08-29 by decompiling each address in the kong
IDB (none is a function start; each successor is named in the row):

| Claimed cite | Status | Nearest known retail anchor |
|---|---|---|
| `Terrain_SetLightingColors @ 0x5C4B10` | RETIRED 2026-08-29 — not a function start in the kong IDB: the address sits inside `Render_VisibilityPortalTraversal @ 0x5c4ae0` (decompiled); cite the successor | no fn at that address; `Render_VisibilityPortalTraversal @ 0x5c4ae0` at −48. Entity/sector lighting was since anchored at `Terrain_SectorComputeLighting @ 0x5c7550` (§iris) |
| `Render_ConfigureFog @ 0x5F9890` | RETIRED 2026-08-29 — not a function start: the address sits inside `StdString_TrimWhitespace` (a CRT string helper, decompiled); cite the successor | unnamed `sub_5F98A0` at +16, body unconfirmed; the device fog path was since anchored at `CD3DDevice_SetFogParameters @ 0x677960` (§Fog policy) |
| `Terrain_GetModulatedColorAtPos @ 0x5C5FE0` | RETIRED 2026-08-29 — not a function start: the address sits inside `Terrain_RenderSectorModels` (the sector-model draw walk, decompiled), a different function | nearest `Entity_RenderNVGLaserBeam @ 0x5c6090` at +176 — likely a different function |
| `Foliage_BuildGeometry @ 0x5BF5F0` | stale / do not cite | address resolves inside `VMacros_BuildShaderPassName @ 0x5bf5d0`; foliage placement anchors are `Foliage_UpdateModelTiles @ 0x601F50`, `Foliage_GenerateModelTileInstances @ 0x600980`, and `Foliage_SampleFoliageMapMask @ 0x606620` |
| jodemo `sub_5D0A90` (terrain-init caller of the loader) | closed by the grill | `Terrain_LoadEnvironmentConfig @ 0x610940` (§BMS overrides) |

### Gap-walk dispositions

One row per §7 "Known Gaps" / §9 "Port Divergences" entry of the jodemo-era spec; all but one
were closed by the grill sections above:

| Gap | Disposition |
|---|---|
| §7.1 env CRC error strings ("bad sky/fog/water CRC", near `0x7480a0`) | Closed as intentional divergence: retail validates env state server-side (`server_handle_client_crc_validation @ 0x519110`, §BMS overrides); our parser does not gate load on CRC (assets are user-edited, not network-delivered) |
| §7.2 fog D3D state slots | Closed: `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` mapping witnessed (§Fog policy); the jodemo-era curve claims (exp `ln(64)/end` for type 0, 0.5/0.25-scaled linear starts) were behaviorally correct |
| §7.3 snapshot stride (`dword_FF375C`) | Closed: snapshot tables `g_EnvTrnSnapshotTable @ 0x26c7414` / `g_EnvEnvSnapshotTable @ 0x26c70d0`, keyframe count at +832 (§Load pipeline) |
| §7.4 advanced-clouds render path | Closed: `advanced_clouds 0` is a single fixed-function pass with the dome material set to the cloud block color (§Sky dome) |
| §7.5 unconsumed globals (`timeofday`, `iris_*`, `lightning/ceiling/floor/cloud_rgb`) | Closed: re-anchored to the `0x26c6xxx` state blocks with consumers identified — iris → entity/sector lighting, ceiling/floor → indoor ambient, cloud → `advanced_clouds 0` dome material, lightning → additive flash slots (§Color state blocks, §iris) |
| §9.1 `.trn`/`.env` pair load order | Closed: two-pass loader documented (§Load pipeline) via `Terrain_LoadEnvironmentConfig @ 0x610940` |
| §9.2 `terrain_rgb` reciprocal consumer | Closed: `EffectWorld_TickInstancesAndLightScale @ 0x5aa170` effect-brightness compensation (§iris / terrain_rgb) |

The audit's one open finding, that `Environment_UpdateWeatherTick @ 0x57e9b0` has signature
`void(int waterHeight, int isReflection)`, is closed (R9-15, 2026-09-24): the IDB type is
bogus. `Environment_UpdateWeatherTick @ 0x57e9b0` takes no arguments (it reads only
`[ebp-4]` / `[ebp-0Ch]`; its sole caller `@ 0x526774` pushes nothing). There is no
reflection-specific weather or sky delta; the mirror's sky differences are the gates in
[render/render-occlusion-re.md](../render/render-occlusion-re.md) §4 and `SkyDome_RenderWithSkyfog(0, 0)`
(§Sky dome).

### Visual-parity check dispositions (sky / fog / weather / celestial)

The planned A/B capture run (4 cameras × 4 curtimes via `scripts/ab_diff.py atmosphere`, a script that no longer exists) was
never executed — zero visual deltas were recorded. The five pre-flagged checks were
dispositioned analytically by the grill instead:

| Check | Disposition |
|---|---|
| Reflection-pass sky in water surfaces | Closed (2026-09-24): no weather-tick parameters exist (see above); the mirror draws its sky under its own gate (`Render_MainScene @ 0x5c1342..0x5c1353`) with `SkyDome_RenderWithSkyfog(0, 0)` |
| Fog-type curves (type 0 exponential, types 2/3 linear starts) | Closed numerically: device-layer mapping witnessed (§Fog policy) |
| Sun position within ~1° of retail | Closed: sun/moon direction verdict **matching** (float-vs-fixed quantization sub-1e-4, §Verdicts) |
| Sky horizon gradient (skybase → skybright → skyhighlight) | Closed: VS constant map verified and `sky` aligned (§Sky dome) |
| Cloud scroll rate | Closed: smoothed-rate accumulators × (1, 1, 2/3, 4/3) with `sky_speed << 10` parse scale (§Sky dome, §Format truths) |

---

## Appendix: C6 consumer/combine grill session record (2026-06-11)

Closed all six grill targets from [env-honored-matrix.md](env-honored-matrix.md); no
target ended inconclusive. Doc-only slice: no reimplementation changes (the fix wave is
roadmap slice C7).

### Per-target verdicts

| Target | Question | Verdict | Evidence | C7 action |
|---|---|---|---|---|
| G1 sky combine | how c11/c12/c15/c24-c27 combine per fragment; cloud_rgb scope | **recovered** — embedded vs_1_1 sources + TSS tables decoded; two-pass spec | §Sky dome | rewrite `sky.gdshader` fragment + the sky presenter's uniforms (now `godot/src/env/sky_dome.cpp`) from the spec; delete keyframed-path `u_cloud_tint`; skip dead c25 |
| G2 iris curve | exact x87 tail shaping | **recovered** — closed-form auto-exposure gain, 64 = identity | §Iris auto-exposure | shipped: `env::iris_gain` + the live `ModulatorChain` (REN-5; divergence #17 FIXED) |
| G3 terrain_rgb consumers | any consumer beyond the effects reciprocal? | **corrected 2026-07-13** — tile overlay is live; texture bake is dead; the foliage sample executes but is overwritten before emission | §iris/terrain_rgb + [foliage-re.md](../foliage/foliage-re.md) | keep tile tint and effects reciprocal; remove invented foliage tint consumer |
| G4 ApplyFogAndAmbient | full state walk; ceiling/floor points; 0x5c7a00 wiring | **complete** — 8-row walk table; exposure application point = `Render_LightScaleRGB` shader constant | §Environment_ApplyFogAndAmbient walk | informs C7 fog/exposure plumbing; ceiling/floor wire-or-delete now decidable (keep: they are live exposure inputs + effect ambient) |
| G5 overcast precedence | fallback or always-after? | **corrected** — never a fallback; additive after `.trn` success (no count reset); missing/failed `.trn` aborts all | §Load pipeline | comment-level in `engine/formats/env`; future overcast cross-fade uses the corrected order |
| G6 thunder/oscillators | epoch→sound mapping; .env tunables | **feeder complete** — the THUNDER set at bearing 0 / 1 m (A) and bearing 128 / 10 m (B) (the earlier "trigger ids 0/0x80" reading was the emitter's bearing word, corrected 2026-08-30), bank `dword_24E0914`, `SETFLASH1` net command; sequencer B reachable through the WAC `farflash` (2026-08-30 correction); no rain/wind `.env` keywords | §weather tick | consumed by the WAC weather wave, not C7 |

### IDB edits applied (sanctioned-grill policy; verify-before-edit; `idb_save` checkpoints)

| Addr | Old | New | Evidence |
|---|---|---|---|
| 0x605e20 | `sub_605E20` | `PolyTrn_SetTerrainTintColors` | writes both tint globals; three witnessed consumers |
| 0x57d4b0 | `sub_57D4B0` | `Env_GetTerrainColorPacked` | 2-insn getter |
| 0x31a1824 | `dword_31A1824` | `g_PolyTrnTerrainTintFull` | consumer sweep |
| 0x31a1828 | `flt_31A1828` | `g_PolyTrnTerrainTintHalf` | `(c>>1)&0x7f7f7f` packing (was mistyped float) |
| 0x57d940 | `sub_57D940` | `ColorBlock_SetStepDeltas` | per-channel `|target−current|/frames` body |
| 0x58db30 | `Render_UnpackFogColor` | `Render_UnpackModulatorToLightScale` | input = modulator block value; output consumed by `Material_ApplyShaderParameters` (old kong name wrong) |
| 0x5aaef0 | `CEffectWorld_UnpackAmbientLightColor` | `EffectWorld_UnpackModulatorToAmbientScale` | same shape, foliage/effects consumers |
| 0x610920 | `sub_610920` | `Terrain_PushSkyDomeHeightFloat` | 16.16→float → `SkyDome_SetHeightAndRebuild` |
| 0x8409f4-fc | `flt_8409F4..` | `g_RenderLightScaleR/G/B` | modulator÷64 shader constant |
| 0x840b24-2c | `flt_840B24..` | `g_EffectWorldAmbientScaleR/G/B` | effects/foliage gain |
| 0x7d7338 | `aVs11DclPositio` | `g_SkyVSGradientPassSource` | embedded vs_1_1 source, pass 1 |
| 0x7d6fc0 | `aVs11DclPositio_0` | `g_SkyVSCloudPassSource` | embedded vs_1_1 source, pass 2 |
| 0x4ed500 | `sub_4ED500` | `Env_TriggerLightningFlashA` | body sets timer A = 16 (old kong comment "font size" wrong) |
| 0x4ed510 | `TextResource_GetStringOrDefault` | `Env_TriggerLightningFlashB` | 2-insn body sets timer B = 32 — prior kong name provably wrong |
| 0x5c7550 | return type `void` → `int` | (type fix) | tail-calls `_ftol2_sse`; fix made Hex-Rays recover the whole iris tail |
| 0x60c670 | `sub_60C670` | `Terrain_RenderSectorBatchLit` | REN-7 dome-band check: `D3DRS_AMBIENT=0xFFFFFF` + `LIGHTING=1` bracket around `Terrain_RenderSectorBatch` — the sky-pass/lo-res terrain leg carries NO below-rim skirt |
| 0x5d7ea0 | `render_scene_with_water_reflection` | (context map, 2026-09-01) | the LIVE beauty scene's PolyTrn context floats after the 16-float view matrix: [3]=the camera FOV in degrees (`dword_A7839C/65536` `@ 0x5d8037`; `PolyTrn_RenderFrame` feeds it to the clip-cone builder `sub_603DA0` `@ 0x60eaf6` — corrected 2026-09-14 from "fog dist"), [4]=water plane −0.1, [5]=quadtree quality scale **1.0** `@ 0x5d8047`, [6]=view dist (`word_26C681E`, the high word of `g_EnvFogDistCurrent`, `@ 0x5d8052`; `> 0` overrides the traversal far slab `flt_8493E8` = 2000 `@ 0x60eb7e`), [9]=plane flag, [10]=foliage-collect **1** `@ 0x5d801e`, [11]=4 overlay layers, [13]=below-water |
| 0x5c1240 | `render_main_scene` | (context map, 2026-09-01) | the offscreen/mirror scene passes [5]=**1.0** `@ 0x5c154d` (same tessellation as beauty — the lo-res-mirror-terrain reading is refuted) and [10]=foliage-collect **0** `@ 0x5c1522` (no near-foliage patches, and the far key list stays empty) |
| 0x60eac0 | `PolyTrn_RenderFrame` | (field consumers, 2026-09-01) | `flt_319FB2C = ctx[5] × flt_8493D8` `@ 0x60eb4a` scales the node-size subdivide test; `dword_319FB34 = ctx[10]` `@ 0x60eb44` gates `Terrain_CollectNearFoliagePatches` at `Terrain_TraverseQuadtreeNode @ 0x609069` |
| 0x5d04c0 | `NVG_RenderSceneToTarget` | (context note, 2026-09-01) | the separate low-detail/no-water scene path passes ctx[5] = scoped ? 1.66 : 0.5 `@ 0x5d05dc..0x5d05f0` — NOT the beauty-vs-mirror comparison pair |

Plus explanatory comments at 0x57db30/0x57dbca/0x57dbf5/0x57dbf7/0x57dce0 (loader),
0x60fc66/0x57d4c0/0x606030 (terrain tint), 0x5c7550/0x5c7a00 (exposure), 0x57e440/0x57d940/
0x5f7163 (walk), 0x5789e0/0x579080/0x27219f0 (sky), 0x4ed500/0x4ed510/0x57ecfb/0x57edc4/
0x429ec9 (lightning). No speculative renames were left applied; everything above is
witnessed in the listed bodies.

---

## Appendix: C7 pre-port grill addendum (2026-06-11)

Targeted reads that closed the C6 record's three port-blocking open questions before the
`sky.gdshader` rewrite (all findings woven into §Sky dome above):

1. **Dome normals + scale** — full read of `SkyDome_BuildMesh @ 0x578db0`: normal =
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
| 0x5ac850 | `init_weather_particles` | `Star_GenerateInstanceTable` | writes 256×40 B from `g_StarInstances`+16 to `g_CelestialSunModel`; per-star PRNG math; sole caller `EffectWorld_LoadCelestialModels @ 0x5add40` |
| 0x5ac010 | (undefined bytes) | `Star_TwinklePrngNext_Unused` | the standalone rol4/rol11/xor-1 PRNG step on `g_StarTwinklePRNG`; zero callers (live sites inline it) |
| 0x26c6880 | `dword_26C6880` | `g_EnvRainPctCurrent` | the `(d+31)>>5` spring at `@ 0x57ef01`; debug label `"Rain: %i%%"` `[orig: @ 0x4efa91]`; >48 gates weather-particle fall decay `[@ 0x5de916]` |
| 0x26c6884 | `dword_26C6884` | `g_EnvRainPctTarget` | net-synced (`@ 0x430311`), serialized (`@ 0x4ffae8`), snap (`@ 0x57d2d3`) |
| 0x26c688c / 0x26c6890 | `dword_26C688C/90` | `g_EnvRainPctStep` / `g_EnvRainPctMax` | the spring's step/clamp slots |
| 0x5c8510 | `sub_5C8510` (5-byte decompile-fail) | `Water_RenderReflectedWorldScene` | the reflected-world subscene (header call + fall-through body; the "retn `@ 0x5c8af8`" extent recorded here was corrected 2026-09-24 to `0x5c8510..0x5c860b`, a tail `jmp sub_67CAA0`); builds the water clip-plane texture matrix + arms `g_WaterMirrorActive` |
| 0x5c2780 | `sub_5C2780` | `Water_ReflectionPrerender` | packs the live camera block; sole reflection caller of `Render_MainScene` |
| 0x5c90a0 | `sub_5C90A0` | `Terrain_CollectVisibleEntitiesForReflection` | the reflection pass's OWN visibility collection (sole caller `Render_MainScene @ 0x5c16fc`): above water every collector gets `filterMask = 0x400`; a below-water view collects unfiltered (`filterMask = 0`); also recomputes the LOD-scale globals `g_RlodFrameScale/g_RlodFrameScaleInv` |
| 0x40e20a | (in `Entity_InitFromModel`) | entity+36 `\|= 0x400` REFLECTABLE | the flag's SOLE writer: set iff `ItemDefType(+0x5C)==1` (vehicle); read by the reflection collectors' filterMask and the decal projectors `@ 0x5cc924`/`@ 0x5ce59b` |
| 0x60f150 | `sub_60F150` | `PolyTrn_ResetFrameStatsAndRender` | zeroes 7 frame counters -> `PolyTrn_RenderFrame` |
| 0x680080 / 0x6800a0 | `sub_680080/A0` | `GTexRT_SelectThunk` / `GTexRT_RestoreThunk` | the RTT begin/end pair (`GTexRT_Select @ 0x67fa90`) around `g_WaterReflectionTexture` |

Plus comments at 0x5c8510/0x5c1240 (the full reflection-pipeline walks),
0x27e2e38 (the star-entry layout), 0x5ac850/0x5ac010. The `Render_ResetFixedFunctionState`
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

## Ledger de-table transplants (2026-08-06)

Closed ledger rows whose full text previously lived only in the divergence
ledger, transplanted verbatim at the 2026-08-06 compaction (Standing rule 6).

- **env #35** [FIXED (2026-07-10): the LUT is a committed 256-byte constant — the deterministic instance every existing pin was generated from; landmarks + symmetry sum unchanged; the retail-instance byte check rides the pinned-current caveat (env-tod-re.md #35)] Water sine LUT provenance: the runtime `std::sin` build forked per libm at trunc boundaries (the GitHub `macos-26-arm64` image flipped non-landmark bytes and every downstream noise pixel — `env_render_unit` red on macOS only, 2026-07-10); the original builds ONE deterministic instance via x87 fsin `[orig: Water_InitNoiseFieldAndSineLut @ 0x5c0308..0x5c0334]`
