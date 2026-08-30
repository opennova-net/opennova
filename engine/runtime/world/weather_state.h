#pragma once

// The retail weather globals, ONE home (docs/env/env-tod-re.md): the
// Env_* state cluster every WAC weather handler writes, every render frame
// reads, the S2C 0x0A phase-2 projection serializes, and a joiner's decoder
// writes back. Owned by the World (snapshotted/restored with it), ticked once
// per 62.5 Hz logic tick by the weather tick (the sim legs here, the color
// legs in env::WeatherRuntime — one tick, one clock
// [orig: Environment_UpdateWeatherTick @ 0x57e9b0 runs from
//  Game_ProcessMainFrame @ 0x526774, after Entity_UpdateAllEntities
//  @ 0x52674b, once per drained 16 ms quantum]).
//
// The math cluster (springs, sequencers, oscillator, color blocks, modulator
// chain, cloud scroll) is env::WeatherCore (engine/formats/env); this struct
// adds the mission clock, quake, precipitation kind, fog type/reference,
// lightning color, the WAC color-fade ticks, and the precipitation pool, plus
// the witnessed handler math of every weather command.

#include <formats/env/env_weather_core.h>
#include <runtime/environment/precipitation.h>

#include <cstdint>

namespace opennova::world {

class World;
struct WeatherState;

// The mission-start seed: the parsed .env (after the BMS header overrides) and
// the BMS clock, in native units. Built by env::weather_seed_from_config (the
// ONE derivation every embedder runs), applied by WeatherState::seed.
struct WeatherSeed {
    int32_t fog_level_q16 = 1024 << 16;      // Env_FogDistTarget/Current @ 0x26c6820/0x26c681c
    int32_t sky_height_q16 = 175 << 16;      // Env_SkyHeightTarget @ 0x26c685c
    uint32_t cloud_scroll_rate_target = 0;   // Env_CloudScrollRateTarget (sky_speed << 10)
    uint32_t tod_fixed24 = 12u << 24;        // Env_CurTimeFixed24 (BMS start_time << 16)
    uint32_t tod_advance_per_tick = 0;       // Env_TodAdvancePerTick
    int32_t fog_type = 1;                    // Env_FogType @ 0x26c6808
    uint32_t lightning_color = 0x00FFFFFFu;  // Env_LightningColor @ 0x26c646c (.env lightning_rgb)
    int32_t wind_scale = 256;                // Env_WindScale @ 0x26c68c0 (Environment_InitDefaults @ 0x57c1d1)
};

// One decoded S2C 0x0A phase-2 ENV sub-block in WIRE units (the client
// writes these into the TARGET globals; the local currents keep chasing)
// [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c].
struct WeatherWireSample {
    int fog_dist = 0;           // u16, whole metres -> Env_FogDistTarget << 16
    int fog_accel = 0;          // u16 8.8 -> Env_FogDistAccelClamp << 8
    int tod_fixed = 0;          // u16 -> Env_CurTimeFixed24 << 13
    int quake_ticks = 0;        // u8
    int cloud_scroll = 0;       // u8 -> Env_CloudScrollRateTarget << 10
    int rain_pct = 0;           // u8 8.8 -> Env_RainPctTarget << 8
    int overcast = 0;           // u8 8.8 -> Env_OvercastBlendTarget << 8
    int precipitation_kind = 0; // u8 (0 rain, 1 snow)
};

enum class PrecipitationKind : uint8_t {
    Rain = 0,
    Snow = 1,
};

// The WAC color commands' block selector [orig: WacCmd_Sun @ 0x4edcd0, Sky
// @ 0x4edd00, Ground @ 0x4edd60, Floor @ 0x4eddf0, Ceiling @ 0x4eddc0, Cloud
// @ 0x4edd90, FogColor @ 0x4ede40, SkyFogColor @ 0x4ede70, Gain @ 0x4edd30].
enum class WeatherColorTarget : uint8_t {
    Sun,
    Sky,
    Ground,
    Floor,
    Ceiling,
    Cloud,
    Fog,
    SkyFog,
    Gain,
};

// The thunder one-shot the audio owner plays: the THUNDER trigger set at a
// distance from the listener along a bearing [orig: Sound_PlayTriggerSetScaled
// @ 0x527b90 — the 24-byte emitter {0x10000, bearing, g_SoundVolumeOption, 0,
// distance, 0} into SoundBank_PlayTriggerEntries @ 0x75ccd0 on the THUNDER
// bank dword_24E0914; A = 1 m centred (@ 0x57ecfb), B = 10 m from behind,
// bearing 128 (@ 0x57edc4)].
struct WeatherSoundEvent {
    int32_t distance_q16 = 0x10000;
    uint8_t bearing = 0;
};

// The render owner's per-tick hook: the kernel runs the sim legs, then the
// installed render owner ticks the color legs against the same core — the
// witnessed single Environment_UpdateWeatherTick, split across two owners.
class IWeatherRenderTick {
public:
    virtual ~IWeatherRenderTick() = default;
    virtual void weather_render_tick(WeatherState &weather) = 0;
};

// Sim-tick outputs the tick's caller consumes (the world has no audio or
// camera): the thunder one-shots and the local-player quake shake arm.
struct WeatherTickEvents {
    bool thunder_a = false;     // sequencer A epoch 0 [orig: @ 0x57ecfb]
    bool thunder_b = false;     // sequencer B epoch 0 [orig: @ 0x57edc4]
    bool quake_shake_local = false; // the local player (or its carrier) was displaced
};

struct WeatherState {
    env::WeatherCore core;

    bool valid = false;               // an owner seeded it (mission T0)
    uint32_t generation = 0;          // bumped on every mutation (commands and ticks)
    uint32_t command_generation = 0;  // bumped by commands only

    // The mission clock [orig: Env_CurTimeFixed24 @ 0x26c6448 (8.24 hours),
    // Env_TodAdvancePerTick @ 0x26c644c, Env_TodMinuteTickdown @ 0x26c6064,
    // Env_TodMinutesElapsed @ 0x26c6450].
    static constexpr uint32_t kTodDayFixed24 = 24u << 24;
    static constexpr int32_t kTodMinuteTicks = 310;
    // The 62 ticks-per-second scale every WAC seconds argument multiplies by
    // [orig: WacCmd_Rain @ 0x4edf60 `imul 62`; WacCmd_ColorFade @ 0x4edcb0].
    static constexpr int32_t kWacTicksPerSecond = 62;
    uint32_t tod_fixed24 = 12u << 24;
    uint32_t tod_advance_per_tick = 0;
    int32_t tod_minute_tickdown = kTodMinuteTicks;
    uint32_t tod_minutes_elapsed = 0;

    uint32_t quake_ticks = 0;              // Env_QuakeTicks @ 0x26c68ac
    uint32_t cloud_scroll_rate_target = 0; // Env_CloudScrollRateTarget @ 0x26c6870
    uint32_t precipitation_kind = 0;       // Env_PrecipitationKind (ex dword_2C059D0)
    int32_t fog_type = 1;                  // Env_FogType @ 0x26c6808
    int32_t fog_reference_q16 = 1024 << 16; // Env_FogDistReference @ 0x26c68a8
    uint32_t lightning_color = 0x00FFFFFFu; // Env_LightningColor @ 0x26c646c
    int32_t color_fade_ticks = 0;          // Env_ColorFadeTicks (ex frameCount @ 0xc60dd0)
    // The overcast blend the TOD color compute reads THIS tick — retail runs
    // Environment_ComputeTimeOfDayColors before the overcast spring steps
    // [orig: @ 0x57e9c7 vs @ 0x57ef62], so the cross-fade lags the spring by
    // one tick.
    int32_t overcast_for_tod_q16 = 0;

    // The precipitation pool and the PRNG_B stream its seed draws from
    // [orig: PRNG_Next16_B @ 0x6130f0 over dword_31BFBBC, seeded 0x5ADEADA5
    //  by Game_StartMission @ 0x52460b].
    static constexpr uint32_t kPrng16BSeed = 0x5ADEADA5u;
    uint32_t prng16_b_state = kPrng16BSeed;
    env::PrecipitationField precipitation;
    uint16_t next_prng16_b() noexcept;

    // --- the wire projection's reads (native units) -----------------------
    int32_t fog_target_q16() const { return core.scalar_channels.fog_dist_target_fp; }
    int32_t fog_current_q16() const { return core.scalar_channels.fog_dist_fp; }
    uint32_t fog_accel_clamp() const { return static_cast<uint32_t>(core.scalar_channels.fog_step_fp); }
    uint32_t rain_pct_current_q16() const { return static_cast<uint32_t>(core.scalar_channels.rain_pct_fp); }
    uint32_t rain_pct_target_q16() const { return static_cast<uint32_t>(core.scalar_channels.rain_pct_target_fp); }
    uint32_t overcast_blend_q16() const { return static_cast<uint32_t>(core.scalar_channels.overcast_fp); }
    uint32_t overcast_target_q16() const { return static_cast<uint32_t>(core.scalar_channels.overcast_target_fp); }
    int32_t sun_dim_pct_q16() const { return core.scalar_channels.sun_dim_fp; }
    int32_t sky_height_q16() const { return core.scalar_channels.sky_height_fp; }
    int32_t sky_height_target_q16() const { return core.scalar_channels.sky_height_target_fp; }
    int32_t cloud_scroll_rate() const { return core.cloud_scroll.rate; }
    int32_t wind_scale() const { return core.oscillator.intensity; }
    // Env_RainPctCurrent > 48 (the drop gate) [orig: @ 0x5de92e; @ 0x5dee48].
    bool raining() const { return core.scalar_channels.rain_pct_fp > env::PrecipitationField::kRainGateQ16; }
    // The clock as HHMM (the TOD compute's parameter space).
    double tod_hhmm() const;
    // Env_IsNightPhase — the 06:00/18:45 sun-vs-moon select over the clock
    // [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 ->
    //  Environment_GetLightDirectionFloat @ 0x57d870]; the `night` WAC value.
    bool is_night_phase() const;

    // --- seeding / mission start ------------------------------------------
    // The load-time seed [orig: TimeOfDay_ParseProperty targets;
    //  Environment_SnapStateToTargets @ 0x57d1e0 (targets, PRNG seed, zeroed
    //  quake/lightning/hit-dim); Game_StartMission clock @ 0x525371;
    //  Precipitation_Reset @ 0x5df3a0 from Game_StartMission @ 0x5249d4].
    void seed(const WeatherSeed &seed);
    // The mission-start initializer [orig: Environment_MissionStartInit (ex
    // sub_57F1E0) @ 0x57f1e0..0x57f873]: every block current <- target with
    // the 0x2800000 step, the scalar + cloud-rate currents <- targets, the
    // recovered clamps (rain/overcast max 0xFFFF, step 0x1000; fog accel
    // 0xFF0000, max 1000 m). The 255 settle ticks that follow are the
    // embedder's loop of full ticks.
    void mission_start_init();
    // A joiner's decoded phase-2 sample -> the TARGET globals
    // [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c].
    void apply_wire_sample(const WeatherWireSample &sample);

    // --- the WAC weather handlers (byte-faithful) -------------------------
    void command_rain(int32_t percent, int32_t seconds);        // [orig: WacCmd_Rain @ 0x4edf60]
    void command_snow(int32_t percent, int32_t seconds);        // [orig: WacCmd_Snow @ 0x4edfd0]
    void command_overcast(int32_t percent, int32_t seconds);    // [orig: WacCmd_Overcast @ 0x4ee040]
    void command_fog_distance(int32_t metres);                  // [orig: WacCmd_FogDist @ 0x4ee100]
    void command_move_fog(int32_t metres, int32_t seconds);     // [orig: WacCmd_MoveFog @ 0x4ee0a0]
    void command_sky_speed(int32_t rate);                       // [orig: WacCmd_SkySpeed @ 0x4edeb0]
    void command_sky_height(int32_t height_raw);                // [orig: WacCmd_SkyHeight @ 0x4edec0]
    void command_quake(int32_t seconds);                        // [orig: WacCmd_Quake @ 0x4ed4c0]
    void command_time_of_day_minutes(int32_t minute_of_day);    // [orig: WacCmd_Tod @ 0x4edc70]
    // The dev-tool scrub (MCP rows, GameWorld's debug seam): the exact
    // minute on the 8.24 clock. Not a witnessed handler — the WAC `tod`
    // math above keeps its 0x44444-per-minute truncation.
    void debug_set_time_of_day_minutes(double minute_of_day);
    void command_fog_type(int32_t type);                        // [orig: WacCmd_FogType @ 0x4eded0]
    void command_sun_fade(int32_t percent, int32_t seconds);    // [orig: WacCmd_SunFade @ 0x4edf10]
    void command_color_fade(int32_t seconds);                   // [orig: WacCmd_ColorFade @ 0x4edcb0]
    void command_lightning_color(uint32_t rgb);                 // [orig: Script_SetLightningColor @ 0x4ede20]
    void command_flash();                                       // [orig: Env_TriggerLightningFlashA @ 0x4ed500]
    void command_far_flash();                                   // [orig: Env_TriggerLightningFlashB @ 0x4ed510]
    void command_block_color(WeatherColorTarget target, uint32_t rgb); // [orig: WacCmd_Sun.. @ 0x4edcd0..]
    // Env_WindScale (the `wind` named value; retail's only writer is
    // Environment_InitDefaults @ 0x57c1d1).
    void set_wind_scale(int32_t value);

    // --- the tick's sim legs -----------------------------------------------
    // Everything of Environment_UpdateWeatherTick that is not a color block:
    // the clock advance + minute counter (@ 0x57e9c7..0x57e9ef), the wind
    // oscillator (@ 0x57e9fc..0x57eaed), the hit-dim fade (@ 0x57eaf9), the
    // quake jitter over pools 0/1 (@ 0x57eb12..0x57ec61), both lightning
    // sequencers with their thunder epochs (@ 0x57ec6f..0x57edc4), the
    // scalar springs (@ 0x57ede2..0x57ef92) and the cloud-scroll rate ramp
    // (@ 0x57eecc); the precipitation fall of the entity update that precedes
    // it in the frame [orig: Precipitation_FallTick (ex sub_5DE8F0) @ 0x5de8f0
    // from Entity_UpdateAllEntities @ 0x4c2214]. `world` may be null (a
    // headless owner without entities). The TOD color compute sits between the
    // clock advance and the oscillator in retail; the render half runs it from
    // overcast_for_tod_q16.
    void tick_sim(World *world, WeatherTickEvents &events);

private:
    void bump() { ++generation; }
    void bump_command() { ++generation; ++command_generation; }
    void apply_quake_jitter(World &world, WeatherTickEvents &events);
    env::WeatherColorBlock &block_for(WeatherColorTarget target);
};

} // namespace opennova::world
