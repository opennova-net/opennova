// The weather home (runtime/world/weather_state): the WAC handler math, the
// mission-start initializer, the wire-sample apply, and the sim tick against
// a scripted world [orig: Environment_UpdateWeatherTick @ 0x57e9b0; the WAC
// handlers @ 0x4edc70..0x4ee100; Environment_MissionStartInit @ 0x57f1e0;
// NapiNPClientMsg_0x00A case 2 @ 0x430244].

#include <runtime/world/ai.h>
#include <runtime/world/weather_state.h>
#include <runtime/world/world.h>

#include <cstdio>

namespace {
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

w::WeatherSeed seed_800() {
	w::WeatherSeed seed;
	seed.fog_level_q16 = 800 << 16;
	seed.sky_height_q16 = 175 << 16;
	seed.cloud_scroll_rate_target = 15u << 10;
	seed.tod_fixed24 = 9u << 24;
	seed.tod_advance_per_tick = 0x1234u;
	seed.fog_type = 2;
	seed.lightning_color = 0x383B27u;
	return seed;
}

void test_seed_installs_the_snapshot_and_zeroes_the_transients() {
	w::WeatherState ws;
	ws.command_quake(3);
	ws.command_flash();
	ws.core.hit_dim.intensity = 0xA000;
	const uint32_t commands_before = ws.command_generation;
	ws.seed(seed_800());
	CHECK(ws.valid);
	CHECK(ws.fog_target_q16() == (800 << 16) && ws.fog_current_q16() == (800 << 16));
	CHECK(ws.sky_height_target_q16() == (175 << 16));
	CHECK(ws.cloud_scroll_rate_target == (15u << 10));
	CHECK(ws.cloud_scroll_rate() == 0); // the rate ramps in from 0 [orig: @ 0x57d2da]
	CHECK(ws.tod_fixed24 == (9u << 24) && ws.tod_advance_per_tick == 0x1234u);
	CHECK(ws.quake_ticks == 0 && ws.core.lightning.timer_a == 0 && ws.core.hit_dim.intensity == 0);
	CHECK(ws.core.oscillator.prng == 0x12333333u);
	CHECK(ws.fog_type == 2 && ws.lightning_color == 0x383B27u);
	CHECK(ws.precipitation_kind == 0);
	// The seed re-seeded the B stream, then the pool drew 3 x 3072 words.
	uint32_t b = 0x5ADEADA5u;
	for (int i = 0; i < 3 * 3072; ++i) {
		const uint32_t rol11 = (b << 11) | (b >> 21);
		const uint32_t sum = b + rol11;
		b = ((sum << 4) | (sum >> 28)) ^ 1u;
	}
	CHECK(ws.prng16_b_state == b);
	// The pool re-seeded from the B stream (the same draws the test in
	// tests/environment pins); every slot inside the seed volume.
	CHECK(ws.precipitation.slots[0].x != 0 || ws.precipitation.slots[0].y != 0);
	CHECK(ws.wind_scale() == 256);
	CHECK(ws.command_generation == commands_before + 1);
}

void test_rain_and_overcast_handlers() {
	w::WeatherState ws;
	ws.seed(seed_800());
	// rain(100, 1): target 0x10000, 62 ticks -> step (0x10000 + 31) / 62 = 1057.
	ws.command_rain(100, 1);
	CHECK(ws.rain_pct_target_q16() == 0x10000u);
	CHECK(ws.core.scalar_channels.rain_step_fp == 1057);
	CHECK(ws.precipitation_kind == 0);
	// snow(150, 0): the percent clamps at 0x10000, zero seconds -> one tick.
	ws.command_snow(150, 0);
	CHECK(ws.rain_pct_target_q16() == 0x10000u && ws.core.scalar_channels.rain_step_fp == 0x10000);
	CHECK(ws.precipitation_kind == 1);
	// overcast(50, 2): 0x8000 over 124 ticks -> (0x8000 + 62) / 124 = 264.
	ws.command_overcast(50, 2);
	CHECK(ws.overcast_target_q16() == 0x8000u && ws.core.scalar_channels.overcast_step_fp == 264);
}

void test_fog_handlers_clamp_to_the_reference() {
	w::WeatherState ws;
	ws.seed(seed_800());
	// fogdist(200): target 200 m, an immediate |target - current| step.
	ws.command_fog_distance(200);
	CHECK(ws.fog_target_q16() == (200 << 16));
	CHECK(ws.fog_accel_clamp() == static_cast<uint32_t>(600 << 16));
	// fogdist(5000) clamps at the 1024 m reference; fogdist(0) at 2 m.
	ws.command_fog_distance(5000);
	CHECK(ws.fog_target_q16() == (1024 << 16));
	ws.command_fog_distance(0);
	CHECK(ws.fog_target_q16() == (2 << 16));
	// movefog(200, 2) from a current of 800: |200 - 800 << 16 + 62| / 124 = 317109.
	ws.core.scalar_channels.fog_dist_fp = 800 << 16;
	ws.command_move_fog(200, 2);
	CHECK(ws.fog_target_q16() == (200 << 16));
	CHECK(ws.fog_accel_clamp() == 317109u);
}

void test_scalar_handlers() {
	w::WeatherState ws;
	ws.seed(seed_800());
	ws.command_sky_speed(47);
	CHECK(ws.cloud_scroll_rate_target == (47u << 10));
	ws.command_sky_height(175);
	CHECK(ws.sky_height_target_q16() == 175); // the raw parameter
	ws.command_quake(7);
	CHECK(ws.quake_ticks == 42);
	ws.command_time_of_day_minutes(330);
	CHECK(ws.tod_fixed24 == 330u * 0x44444u);
	ws.command_fog_type(3);
	CHECK(ws.fog_type == 3);
	ws.command_color_fade(2);
	CHECK(ws.color_fade_ticks == 124);
	ws.command_lightning_color(0xFF112233u);
	CHECK(ws.lightning_color == 0x112233u);
	ws.set_wind_scale(128);
	CHECK(ws.wind_scale() == 128);
	// sunfade(50, 1): target 0x320000 (16.16 percent), 62 ticks; the max
	// clamp has no writer so the current never moves.
	ws.command_sun_fade(50, 1);
	CHECK(ws.core.scalar_channels.sun_dim_target_fp == 0x320000);
	CHECK(ws.core.scalar_channels.sun_dim_step_fp == (0x320000 + 31) / 62);
	w::WeatherTickEvents events;
	for (int i = 0; i < 100; ++i) ws.tick_sim(nullptr, events);
	CHECK(ws.sun_dim_pct_q16() == 0);
}

void test_block_color_command_sets_the_active_target_and_step() {
	w::WeatherState ws;
	ws.seed(seed_800());
	ws.command_color_fade(1);
	ws.core.sky_color_blocks.ceiling.snap(0x00101010u);
	ws.command_block_color(w::WeatherColorTarget::Ceiling, 0x00404040u);
	CHECK(ws.core.sky_color_blocks.ceiling.target == 0x00404040u);
	// ColorBlock_SetStepDeltas over 62 ticks: (0x30 << 20 + 31) / 62 per channel.
	CHECK(ws.core.sky_color_blocks.ceiling.max_rate[0] == ((0x30 << 20) + 31) / 62);
	CHECK(ws.core.sky_color_blocks.ceiling.max_rate[3] == 0);
	ws.command_block_color(w::WeatherColorTarget::Gain, 0x00202020u);
	CHECK(ws.core.modulator_chain.modulator.target == 0x00202020u);
}

void test_mission_start_init_snaps_currents_and_installs_the_clamps() {
	w::WeatherState ws;
	ws.seed(seed_800());
	ws.command_rain(100, 1);
	ws.command_overcast(50, 2);
	ws.command_move_fog(200, 2);
	ws.command_sky_speed(47);
	ws.core.sun_block.target = 0x00A0B0C0u;
	ws.mission_start_init();
	CHECK(ws.rain_pct_current_q16() == 0x10000u && ws.overcast_blend_q16() == 0x8000u);
	CHECK(ws.fog_current_q16() == (200 << 16));
	CHECK(ws.core.scalar_channels.rain_max_fp == 0xFFFF && ws.core.scalar_channels.overcast_max_fp == 0xFFFF);
	CHECK(ws.core.scalar_channels.rain_step_fp == 0x1000 && ws.core.scalar_channels.overcast_step_fp == 0x1000);
	CHECK(ws.core.scalar_channels.fog_step_fp == 0x00FF0000 && ws.core.scalar_channels.fog_max_fp == (1000 << 16));
	CHECK(ws.cloud_scroll_rate() == (47 << 10));
	CHECK(ws.core.sun_block.render_color == 0x00A0B0C0u && ws.core.sun_block.max_rate[0] == 0x02800000);
}

void test_tick_advances_the_clock_and_fires_thunder() {
	w::WeatherState ws;
	ws.seed(seed_800());
	w::WeatherTickEvents events;
	const uint32_t tod = ws.tod_fixed24;
	ws.tick_sim(nullptr, events);
	CHECK(ws.tod_fixed24 == tod + 0x1234u);
	CHECK(ws.tod_minute_tickdown == w::WeatherState::kTodMinuteTicks - 1);
	CHECK(!events.thunder_a && !events.thunder_b && !events.quake_shake_local);
	// 310 ticks later the minute counter wraps and counts one elapsed minute.
	for (int i = 0; i < 310; ++i) ws.tick_sim(nullptr, events);
	CHECK(ws.tod_minutes_elapsed == 1);
	// flash: timer A 16 -> thunder on the 16th tick; farflash: B 32.
	ws.command_flash();
	int thunder_tick = -1;
	for (int i = 1; i <= 20; ++i) {
		ws.tick_sim(nullptr, events);
		if (events.thunder_a) thunder_tick = i;
	}
	CHECK(thunder_tick == 16);
	ws.command_far_flash();
	thunder_tick = -1;
	for (int i = 1; i <= 40; ++i) {
		ws.tick_sim(nullptr, events);
		if (events.thunder_b) thunder_tick = i;
	}
	CHECK(thunder_tick == 32);
	// The springs chase: rain(100, 1) climbs by its 1057 step per tick.
	ws.command_rain(100, 1);
	ws.tick_sim(nullptr, events);
	CHECK(ws.rain_pct_current_q16() == 1057u);
	// The drops fall only once the current passes 48.
	CHECK(ws.raining());
	const int32_t z_before = ws.precipitation.slots[0].z;
	ws.tick_sim(nullptr, events);
	CHECK(ws.precipitation.slots[0].z == z_before - 12288);
	// The overcast the TOD compute reads lags the spring by one tick.
	ws.command_overcast(100, 0);
	ws.tick_sim(nullptr, events);
	CHECK(ws.overcast_for_tod_q16 == 0 && ws.overcast_blend_q16() != 0u);
}

void test_quake_displaces_pool_entities_and_arms_the_local_shake() {
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::Entity soldier;
	soldier.kind = w::EntityKind::Organic;
	soldier.item_id = 100;
	soldier.position = {10.0f, 20.0f, 0.0f};
	const w::EntityHandle local = world.registry.spawn(0, soldier);
	world.cached.local_player = local;
	w::Entity airborne = soldier;
	airborne.flags |= w::kEntityFlagInAir;
	const w::EntityHandle flyer = world.registry.spawn(0, airborne);
	w::Entity truck;
	truck.kind = w::EntityKind::Item;
	truck.item_id = 200;
	truck.has_item_def = true;
	truck.item_attrib = 0x40u;
	truck.position = {50.0f, 60.0f, 0.0f};
	const w::EntityHandle vehicle = world.registry.spawn(1, truck);
	w::Entity crate = truck;
	crate.item_attrib = 0;
	const w::EntityHandle box = world.registry.spawn(1, crate);

	world.weather.seed(seed_800());
	world.weather.command_quake(1); // 6 ticks
	w::WeatherTickEvents events;
	const uint32_t prng_before = world.weather.core.oscillator.prng;
	world.weather.tick_sim(&world, events);
	CHECK(events.quake_shake_local);
	CHECK(world.weather.quake_ticks == 5);
	// The tick's oscillator draw is the first jitter word; two displaced
	// entities re-roll twice more.
	uint32_t p = prng_before;
	const auto roll = [&p]() {
		const uint32_t rotated = (p << 9) | (p >> 23);
		p = rotated + ((static_cast<int32_t>(rotated) >> 31) & 0x1ABB09);
		return p;
	};
	const uint32_t tick_word = roll();
	roll();
	roll();
	CHECK(world.weather.core.oscillator.prng == p);
	const uint32_t rand = tick_word & 0xFFFu;
	const int32_t dx = static_cast<int32_t>(static_cast<int16_t>(rand)) >> 6;
	const int32_t dy = 4 * static_cast<int32_t>(static_cast<int8_t>(rand));
	const w::Entity *moved = world.registry.get(local);
	CHECK(moved != nullptr && moved->position.x == 10.0f + static_cast<float>(dx) / 65536.0f);
	CHECK(moved != nullptr && moved->position.y == 20.0f + static_cast<float>(dy) / 65536.0f);
	const w::Entity *still = world.registry.get(flyer);
	CHECK(still != nullptr && still->position.x == 10.0f && still->position.y == 20.0f);
	const w::Entity *shaken = world.registry.get(vehicle);
	CHECK(shaken != nullptr && (shaken->position.x != 50.0f || shaken->position.y != 60.0f || shaken->veh.yaw_bam != 0 || rand == 0));
	const w::Entity *inert = world.registry.get(box);
	CHECK(inert != nullptr && inert->position.x == 50.0f && inert->position.y == 60.0f);
	// Five more ticks exhaust the quake; the sixth displaces nothing.
	for (int i = 0; i < 5; ++i) world.weather.tick_sim(&world, events);
	CHECK(world.weather.quake_ticks == 0);
	world.weather.tick_sim(&world, events);
	CHECK(!events.quake_shake_local);
}

void test_wire_sample_writes_targets_only() {
	w::WeatherState ws;
	ws.seed(seed_800());
	ws.core.scalar_channels.fog_dist_fp = 1000 << 16;
	w::WeatherWireSample sample;
	sample.fog_dist = 380;
	sample.fog_accel = 0xFF00;
	sample.tod_fixed = 0x3088;
	sample.quake_ticks = 17;
	sample.cloud_scroll = 15;
	sample.rain_pct = 0x56;
	sample.overcast = 0x78;
	sample.precipitation_kind = 1;
	ws.apply_wire_sample(sample);
	CHECK(ws.fog_target_q16() == (380 << 16) && ws.fog_current_q16() == (1000 << 16));
	CHECK(ws.fog_accel_clamp() == 0x00FF0000u);
	CHECK(ws.tod_fixed24 == (0x3088u << 13));
	CHECK(ws.quake_ticks == 17 && ws.cloud_scroll_rate_target == (15u << 10));
	CHECK(ws.rain_pct_target_q16() == 0x5600u && ws.rain_pct_current_q16() == 0u);
	CHECK(ws.overcast_target_q16() == 0x7800u && ws.overcast_blend_q16() == 0u);
	CHECK(ws.precipitation_kind == 1);
	w::WeatherTickEvents events;
	ws.tick_sim(nullptr, events);
	CHECK(ws.fog_current_q16() == 0x03D4A000);
	CHECK(ws.rain_pct_current_q16() == 0x2B0u && ws.overcast_blend_q16() == 0x3C0u);
	CHECK(ws.quake_ticks == 16);
}

void test_night_phase_follows_the_clock() {
	w::WeatherState ws;
	w::WeatherSeed seed = seed_800();
	seed.tod_fixed24 = 12u << 24;
	ws.seed(seed);
	CHECK(!ws.is_night_phase());
	ws.command_time_of_day_minutes(2 * 60);
	CHECK(ws.is_night_phase());
	ws.command_time_of_day_minutes(18 * 60 + 50);
	CHECK(ws.is_night_phase());
	ws.command_time_of_day_minutes(6 * 60 + 5);
	CHECK(!ws.is_night_phase());
}

} // namespace

int main() {
	test_seed_installs_the_snapshot_and_zeroes_the_transients();
	test_rain_and_overcast_handlers();
	test_fog_handlers_clamp_to_the_reference();
	test_scalar_handlers();
	test_block_color_command_sets_the_active_target_and_step();
	test_mission_start_init_snaps_currents_and_installs_the_clamps();
	test_tick_advances_the_clock_and_fires_thunder();
	test_quake_displaces_pool_entities_and_arms_the_local_shake();
	test_wire_sample_writes_targets_only();
	test_night_phase_follows_the_clock();
	std::printf(failures ? "WEATHER STATE TEST FAILED (%d)\n" : "weather state test passed\n",
	            failures);
	return failures ? 1 : 0;
}
