#include <runtime/world/weather_state.h>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>

namespace opennova::world {

namespace {

// WAC seconds -> ticks: 62 per second (a wrapping 32-bit imul), zero
// seconds rounds up to one tick; a negative argument stays negative — no
// handler takes an absolute value [orig: WacCmd_Rain @ 0x4edf84 `imul eax,
// 62` then the `test/jnz` zero guard; the same idiom in Overcast @ 0x4ee040,
// MoveFog @ 0x4ee0b4, SunFade @ 0x4edf22].
int32_t wac_ticks(int32_t seconds) noexcept {
    int32_t ticks = static_cast<int32_t>(static_cast<uint32_t>(seconds) *
            static_cast<uint32_t>(WeatherState::kWacTicksPerSecond));
    if (ticks == 0) ticks = 1;
    return ticks;
}

// The 32-bit wrapping negate abs32 uses (INT32_MIN stays INT32_MIN).
int32_t wrap_abs(int32_t v) noexcept {
    return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v;
}

// abs32(target + (ticks >> 1) - current) / ticks — the transition step every
// timed weather command installs: wrapping 32-bit adds, then the SIGNED
// divide, so a negative tick count installs a negative step exactly as the
// original idiv does [orig: WacCmd_Rain @ 0x4edfb6; MoveFog @ 0x4ee0ec;
// SunFade @ 0x4edf52].
int32_t transition_step(int32_t current, int32_t target, int32_t ticks) noexcept {
    const int32_t centered = static_cast<int32_t>(static_cast<uint32_t>(target) +
            static_cast<uint32_t>(ticks >> 1) - static_cast<uint32_t>(current));
    // The int64 divide keeps INT32_MIN / -1 defined (unreachable: the tick
    // count is 62 * s or 1, never -1).
    return static_cast<int32_t>(static_cast<int64_t>(wrap_abs(centered)) / ticks);
}

// min((percent << 16) / 100, 0x10000) — the rain/overcast percent target; the
// shift wraps at 32 bits and a negative percent lands negative
// [orig: WacCmd_Rain @ 0x4edf82..0x4edf95].
int32_t percent_target_q16(int32_t percent) noexcept {
    const int32_t fixed = static_cast<int32_t>(static_cast<uint32_t>(percent) << 16) / 100;
    return fixed > 0x10000 ? 0x10000 : fixed;
}

// min(metres << 16, Env_FogDistReference) raised to 2 m — the fog distance
// target. The << 16 is the script compiler's kind-6 operand shift (the
// handlers receive the 16.16 value), a 32-bit shift that wraps
// [orig: Script_Compile @ 0x4f4167 / @ 0x4f42c5; the clamps WacCmd_FogDist
//  @ 0x4ee10c..0x4ee117, WacCmd_MoveFog @ 0x4ee0b8..0x4ee0c4].
int32_t fog_command_target_q16(int32_t metres, int32_t reference_q16) noexcept {
    int32_t fixed = static_cast<int32_t>(static_cast<uint32_t>(metres) << 16);
    if (fixed > reference_q16) fixed = reference_q16;
    if (fixed < 0x20000) fixed = 0x20000;
    return fixed;
}

uint16_t precipitation_rand16(void *ctx) {
    return static_cast<WeatherState *>(ctx)->next_prng16_b();
}

uint32_t pack_rgb(uint32_t rgb) noexcept {
    return rgb & 0x00FFFFFFu;
}

} // namespace

uint16_t WeatherState::next_prng16_b() noexcept {
    // [orig: PRNG_Next16_B @ 0x6130f0] s = rol4(s + rol11(s)) ^ 1; low word.
    const uint32_t rol11 = (prng16_b_state << 11) | (prng16_b_state >> 21);
    uint32_t next = prng16_b_state + rol11;
    next = ((next << 4) | (next >> 28)) ^ 1u;
    prng16_b_state = next;
    return static_cast<uint16_t>(next);
}

double WeatherState::tod_hhmm() const {
    // 8.24 hours -> HHMM: the whole hour times 100 plus the minute fraction.
    const double hours = static_cast<double>(tod_fixed24) / 16777216.0;
    const double whole = static_cast<double>(static_cast<uint32_t>(hours));
    return whole * 100.0 + (hours - whole) * 60.0;
}

bool WeatherState::is_night_phase() const {
    return env::compute_day_phase(static_cast<float>(tod_hhmm())).is_night;
}

void WeatherState::seed(const WeatherSeed &seed) {
    // A fresh core: the PRNG seed 0x12333333, zeroed quake/lightning/hit-dim,
    // identity modulators, every accumulator at rest [orig:
    // Environment_SnapStateToTargets @ 0x57d1e0 — seed imm32 @ 0x57d2ff].
    core = env::WeatherCore{};
    core.set_wind_intensity(seed.wind_scale);
    core.scalar_channels.fog_dist_target_fp = seed.fog_level_q16;
    core.scalar_channels.fog_dist_fp = seed.fog_level_q16;
    core.scalar_channels.sky_height_target_fp = seed.sky_height_q16;
    core.scalar_channels.sky_height_fp = seed.sky_height_q16;
    // The snap refreshes only the cloud TARGET — the rate always ramps
    // [orig: @ 0x57d2da].
    cloud_scroll_rate_target = seed.cloud_scroll_rate_target;
    tod_fixed24 = seed.tod_fixed24 % kTodDayFixed24;
    tod_advance_per_tick = seed.tod_advance_per_tick;
    tod_minute_tickdown = kTodMinuteTicks;
    tod_minutes_elapsed = 0;
    quake_ticks = 0;
    precipitation_kind = static_cast<uint32_t>(PrecipitationKind::Rain);
    fog_type = seed.fog_type;
    // Env_FogDistReference: 1024.0 at init, re-set at terrain init to 768.0 or
    // 1024.0 by adapter-caps bit 0x40 and forced to 1024.0 on the session
    // authority [orig: Environment_InitDefaults @ 0x57c0b0; Terrain_Init
    //  @ 0x60fc9a/0x60fca3].
    fog_reference_q16 = static_cast<int32_t>(env::kFogDistReferenceDefault);
    lightning_color = pack_rgb(seed.lightning_color);
    // WacScript_InitAndLoad zeroes the color-fade seconds at every load.
    color_fade_ticks = 0;
    overcast_for_tod_q16 = 0;
    // Precipitation_Reset: the table cleared, the drops re-seeded from the
    // freshly seeded PRNG_B stream, the kind reset to rain [orig: @ 0x5df3a0;
    // the B seed @ 0x52460b].
    prng16_b_state = kPrng16BSeed;
    precipitation.reset(&precipitation_rand16, this);
    valid = true;
    bump_command();
}

void WeatherState::mission_start_init() {
    // [orig: Environment_MissionStartInit (ex sub_57F1E0) @ 0x57f1e0..0x57f873]
    // Every color block (the two modulators too): current <- active target,
    // per-channel step 0x2800000, the lightning additive [12] <- 0 (the
    // sixteen `mov dword_xxx, esi` stores with esi = 0 @ 0x57f2d0..0x57f836).
    const auto init_block = [](env::WeatherColorBlock &block) {
        block.snap(block.target);
        for (int32_t &rate : block.max_rate) rate = 0x02800000;
        block.additive = 0;
    };
    init_block(core.fill_block);
    init_block(core.sun_block);
    init_block(core.fog_block);
    init_block(core.sky_block);
    init_block(core.sky_color_blocks.skyfog);
    init_block(core.sky_color_blocks.ceiling);
    init_block(core.sky_color_blocks.cloud);
    init_block(core.sky_color_blocks.floor);
    init_block(core.sky_color_blocks.skybase);
    init_block(core.sky_color_blocks.skybright);
    init_block(core.sky_color_blocks.skyhighlight);
    init_block(core.sky_color_blocks.cloudbase);
    init_block(core.sky_color_blocks.cloudhighlight);
    init_block(core.sky_color_blocks.cloudedge);
    init_block(core.modulator_chain.modulator);
    init_block(core.modulator_chain.modulator2);
    // The scalar currents <- targets plus the recovered clamps
    // [orig: @ 0x57f7d8..0x57f873]; the cloud rate joins the snap.
    core.scalar_channels.mission_start_init();
    core.cloud_scroll.rate = static_cast<int32_t>(cloud_scroll_rate_target);
    bump_command();
}

void WeatherState::apply_wire_sample(const WeatherWireSample &sample) {
    // [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c] — every field
    // lands in a TARGET (or a countdown seed); the currents keep chasing.
    core.scalar_channels.apply_network_sample(
            static_cast<uint16_t>(std::clamp(sample.fog_dist, 0, 0xFFFF)),
            static_cast<uint16_t>(std::clamp(sample.fog_accel, 0, 0xFFFF)),
            static_cast<uint8_t>(std::clamp(sample.rain_pct, 0, 0xFF)),
            static_cast<uint8_t>(std::clamp(sample.overcast, 0, 0xFF)));
    tod_fixed24 = (static_cast<uint32_t>(std::clamp(sample.tod_fixed, 0, 0xFFFF)) << 13) %
            kTodDayFixed24;
    quake_ticks = static_cast<uint32_t>(std::clamp(sample.quake_ticks, 0, 0xFF));
    cloud_scroll_rate_target =
            static_cast<uint32_t>(std::clamp(sample.cloud_scroll, 0, 0xFF)) << 10;
    precipitation_kind = static_cast<uint32_t>(std::clamp(sample.precipitation_kind, 0, 0xFF));
    valid = true;
    bump_command();
}

// --- WAC handlers ------------------------------------------------------------

void WeatherState::command_rain(int32_t percent, int32_t seconds) {
    // [orig: WacCmd_Rain @ 0x4edf60] target = min(pct<<16/100, 0x10000),
    // step = |target + ticks/2 - current| / ticks, kind = rain.
    const int32_t ticks = wac_ticks(seconds);
    env::EnvScalarChannels &ch = core.scalar_channels;
    ch.rain_pct_target_fp = percent_target_q16(percent);
    ch.rain_step_fp = transition_step(ch.rain_pct_fp, ch.rain_pct_target_fp, ticks);
    precipitation_kind = static_cast<uint32_t>(PrecipitationKind::Rain);
    bump_command();
}

void WeatherState::command_snow(int32_t percent, int32_t seconds) {
    // [orig: WacCmd_Snow @ 0x4edfd0] — rain's body with kind = snow.
    const int32_t ticks = wac_ticks(seconds);
    env::EnvScalarChannels &ch = core.scalar_channels;
    ch.rain_pct_target_fp = percent_target_q16(percent);
    ch.rain_step_fp = transition_step(ch.rain_pct_fp, ch.rain_pct_target_fp, ticks);
    precipitation_kind = static_cast<uint32_t>(PrecipitationKind::Snow);
    bump_command();
}

void WeatherState::command_overcast(int32_t percent, int32_t seconds) {
    // [orig: WacCmd_Overcast @ 0x4ee040] — the overcast channel.
    const int32_t ticks = wac_ticks(seconds);
    env::EnvScalarChannels &ch = core.scalar_channels;
    ch.overcast_target_fp = percent_target_q16(percent);
    ch.overcast_step_fp = transition_step(ch.overcast_fp, ch.overcast_target_fp, ticks);
    bump_command();
}

void WeatherState::command_fog_distance(int32_t metres) {
    // [orig: WacCmd_FogDist @ 0x4ee100] target clamped [2 m, reference],
    // accel = |target - current| (an immediate arrival).
    env::EnvScalarChannels &ch = core.scalar_channels;
    ch.fog_dist_target_fp = fog_command_target_q16(metres, fog_reference_q16);
    ch.fog_step_fp = wrap_abs(static_cast<int32_t>(
            static_cast<uint32_t>(ch.fog_dist_target_fp) - static_cast<uint32_t>(ch.fog_dist_fp)));
    bump_command();
}

void WeatherState::command_move_fog(int32_t metres, int32_t seconds) {
    // [orig: WacCmd_MoveFog @ 0x4ee0a0] the same target over ticks.
    const int32_t ticks = wac_ticks(seconds);
    env::EnvScalarChannels &ch = core.scalar_channels;
    ch.fog_dist_target_fp = fog_command_target_q16(metres, fog_reference_q16);
    ch.fog_step_fp = transition_step(ch.fog_dist_fp, ch.fog_dist_target_fp, ticks);
    bump_command();
}

void WeatherState::command_sky_speed(int32_t rate) {
    // [orig: WacCmd_SkySpeed @ 0x4edeb0] Env_CloudScrollRateTarget = n << 10.
    cloud_scroll_rate_target = static_cast<uint32_t>(rate) << 10;
    bump_command();
}

void WeatherState::command_sky_height(int32_t height_raw) {
    // [orig: WacCmd_SkyHeight @ 0x4edec0] the raw parameter IS the 16.16
    // target (the parser's <<16 does not apply to the command).
    core.scalar_channels.sky_height_target_fp = height_raw;
    bump_command();
}

void WeatherState::command_quake(int32_t seconds) {
    // [orig: WacCmd_Quake @ 0x4ed4c0] Env_QuakeTicks = 6 * value, raw: a
    // negative argument wraps to a huge count the tick counts down (@ 0x57ec61).
    quake_ticks = static_cast<uint32_t>(seconds) * 6u;
    bump_command();
}

void WeatherState::command_time_of_day_minutes(int32_t minute_of_day) {
    // [orig: WacCmd_Tod @ 0x4edc70] minute-of-day * 0x44444 into the 8.24
    // accumulator.
    tod_fixed24 = (static_cast<uint32_t>(minute_of_day) * 0x44444u) % kTodDayFixed24;
    bump_command();
}

void WeatherState::debug_set_time_of_day_minutes(double minute_of_day) {
    const double units = std::max(0.0, minute_of_day) / 60.0 * 16777216.0;
    tod_fixed24 = static_cast<uint32_t>(std::llround(units)) % kTodDayFixed24;
    bump_command();
}

void WeatherState::command_fog_type(int32_t type) {
    // [orig: WacCmd_FogType @ 0x4eded0] Env_FogType = n (the immediate
    // Render_SetFogState refresh is the render owner's next fog push).
    fog_type = type;
    bump_command();
}

void WeatherState::command_sun_fade(int32_t percent, int32_t seconds) {
    // [orig: WacCmd_SunFade @ 0x4edf10] the sun-dim channel: target =
    // min(pct << 16, 0x640000) (16.16 percent), the timed step; the channel's
    // max clamp has no writer in the image, so the current never leaves 0.
    const int32_t ticks = wac_ticks(seconds);
    env::EnvScalarChannels &ch = core.scalar_channels;
    const int32_t shifted = static_cast<int32_t>(static_cast<uint32_t>(percent) << 16);
    ch.sun_dim_target_fp = shifted > 0x640000 ? 0x640000 : shifted;
    ch.sun_dim_step_fp = transition_step(ch.sun_dim_fp, ch.sun_dim_target_fp, ticks);
    bump_command();
}

void WeatherState::command_color_fade(int32_t seconds) {
    // [orig: WacCmd_ColorFade @ 0x4edcb0] Env_ColorFadeTicks = 62 * s, raw: a
    // negative count reaches ColorBlock_SetStepDeltas' idiv (@ 0x57d98f) as is.
    color_fade_ticks = static_cast<int32_t>(
            static_cast<uint32_t>(seconds) * static_cast<uint32_t>(kWacTicksPerSecond));
    bump_command();
}

void WeatherState::command_lightning_color(uint32_t rgb) {
    // [orig: Script_SetLightningColor @ 0x4ede20]
    lightning_color = pack_rgb(rgb);
    bump_command();
}

void WeatherState::command_flash() {
    // [orig: Env_TriggerLightningFlashA @ 0x4ed500] timer A = 16.
    core.lightning.trigger_short();
    bump_command();
}

void WeatherState::command_far_flash() {
    // [orig: Env_TriggerLightningFlashB @ 0x4ed510] timer B = 32.
    core.lightning.trigger_long();
    bump_command();
}

env::WeatherColorBlock &WeatherState::block_for(WeatherColorTarget target) {
    switch (target) {
        case WeatherColorTarget::Sun: return core.sun_block;
        case WeatherColorTarget::Sky: return core.sky_block;
        case WeatherColorTarget::Ground: return core.fill_block;
        case WeatherColorTarget::Floor: return core.sky_color_blocks.floor;
        case WeatherColorTarget::Ceiling: return core.sky_color_blocks.ceiling;
        case WeatherColorTarget::Cloud: return core.sky_color_blocks.cloud;
        case WeatherColorTarget::Fog: return core.fog_block;
        case WeatherColorTarget::SkyFog: return core.sky_color_blocks.skyfog;
        case WeatherColorTarget::Gain: return core.modulator_chain.modulator;
    }
    return core.sun_block;
}

void WeatherState::command_block_color(WeatherColorTarget target, uint32_t rgb) {
    // [orig: WacCmd_Sun @ 0x4edcd0 .. WacCmd_Gain @ 0x4edd30] the block's
    // ACTIVE target [11] takes the packed rgb, then ColorBlock_SetStepDeltas
    // (@ 0x57d940) installs the chase over Env_ColorFadeTicks. TOD-keyframed
    // blocks are overwritten by the next ComputeTimeOfDayColors; the statics
    // (ceiling/cloud/floor) and the modulator keep it.
    env::WeatherColorBlock &block = block_for(target);
    block.target = pack_rgb(rgb);
    block.set_step_deltas(color_fade_ticks);
    bump_command();
}

void WeatherState::set_wind_scale(int32_t value) {
    core.set_wind_intensity(value);
    bump_command();
}

// --- the tick ----------------------------------------------------------------

void WeatherState::tick_sim(World *world, WeatherTickEvents &events) {
    events = WeatherTickEvents{};
    // The clock: the TOD colors compute at curtime + advance, i.e. at the
    // advanced clock [orig: @ 0x57e9c7]; the 310-tick minute counter
    // [orig: @ 0x57e9da..0x57e9ef].
    tod_fixed24 = (tod_fixed24 + tod_advance_per_tick) % kTodDayFixed24;
    if (--tod_minute_tickdown < 0) {
        tod_minute_tickdown = kTodMinuteTicks;
        if (tod_advance_per_tick != 0) ++tod_minutes_elapsed;
    }
    // The TOD compute reads the overcast blend before the spring steps it.
    overcast_for_tod_q16 = core.scalar_channels.overcast_fp;
    core.tick_sim_head();
    if (quake_ticks != 0) {
        if (world != nullptr) apply_quake_jitter(*world, events);
        --quake_ticks; // [orig: @ 0x57ec61]
    }
    core.tick_sim_tail(lightning_color, static_cast<int32_t>(cloud_scroll_rate_target));
    events.thunder_a = core.lightning.thunder_a;
    events.thunder_b = core.lightning.thunder_b;
    bump();
}

void WeatherState::apply_quake_jitter(World &world, WeatherTickEvents &events) {
    // [orig: Environment_UpdateWeatherTick @ 0x57eb12..0x57ec61] — the tick's
    // oscillator draw (Env_WeatherPrng & 0xFFF) is the first jitter word; every
    // displaced entity re-rolls the PRNG for the next.
    uint32_t rand = core.oscillator.prng & 0xFFFu;
    const EntityHandle local_player = world.cached.local_player;
    const Entity *local = world.registry.get(local_player);
    const auto displace = [&](Entity &e, int32_t *heading_bam) {
        // X += (int16)r >> 6, Y += 4 * (int8)r, heading += (16 * r) >> 7
        // [orig: @ 0x57eb57..0x57eb6f / @ 0x57ec00..0x57ec18].
        const int32_t dx = static_cast<int32_t>(static_cast<int16_t>(rand)) >> 6;
        const int32_t dy = 4 * static_cast<int32_t>(static_cast<int8_t>(rand));
        const int32_t dh = static_cast<int32_t>((16u * rand) >> 7);
        e.position.x += static_cast<float>(dx) / 65536.0f;
        e.position.y += static_cast<float>(dy) / 65536.0f;
        if (world.ai != nullptr) {
            if (AiEntity *a = world.ai->for_handle(e.handle)) {
                a->pos[0] += dx;
                a->pos[1] += dy;
                if (heading_bam == nullptr) a->heading += dh;
            }
        }
        if (heading_bam != nullptr) *heading_bam += dh;
        rand = core.oscillator.reroll() & 0xFFFu;
    };
    // Pool 0: every placed organic not in the air and not carried; the local
    // player's displacement arms the camera shake [orig: @ 0x57eb48..0x57eb7d].
    world.registry.for_each_in_pool(0, [&](const Entity &row) {
        Entity *e = world.registry.get(row.handle);
        if (e == nullptr || e->item_id == 0) return;
        if ((e->flags & kEntityFlagInAir) != 0 || e->mounted) return;
        displace(*e, nullptr);
        if (e->handle == local_player) events.quake_shake_local = true;
    });
    // Pool 1: items whose def carries attrib 0x40; the local player's carrier
    // arms the shake [orig: @ 0x57ebde..0x57ec29].
    world.registry.for_each_in_pool(1, [&](const Entity &row) {
        Entity *e = world.registry.get(row.handle);
        if (e == nullptr || e->item_id == 0) return;
        if ((e->flags & kEntityFlagInAir) != 0) return;
        if (!e->has_item_def || (e->item_attrib & 0x40u) == 0) return;
        displace(*e, &e->veh.yaw_bam);
        if (local != nullptr && local->mounted && local->mount_target == e->handle)
            events.quake_shake_local = true;
    });
}

} // namespace opennova::world
