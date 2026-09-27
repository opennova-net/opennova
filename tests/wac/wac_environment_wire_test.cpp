// WAC-authored weather lands in the ONE weather home and is observable
// through the real S2C 0x0A phase-2 projection; the tick that advances it is
// the world's weather tick.

#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>

#include "netsim/conn_fan_test_util.h"

#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/loopback_channel.h>
#include <net/npwire/ingame_decode.h>
#include <runtime/world/world.h>
#include <runtime/world/player_view.h>

#include <cstdio>
#include <vector>

namespace {

namespace ns = opennova::replication;
namespace nw = opennova;
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

void run_one_wac_execution(w::World &world, opennova::wac::WacSystem &system) {
	world.add_system(&system);
	world.load_systems();
	for (int tick = 0; tick < opennova::wac::WacSystem::kTicksPerExecution; ++tick) {
		world.run_logic_tick(/*is_authority=*/true);
	}
}

nw::FrameUpdate emit_phase2(w::World &world) {
	world.registry.configure_pool(0, 1);
	w::Entity recipient;
	recipient.kind = w::EntityKind::Organic;
	recipient.health = 150;
	const w::EntityHandle recipient_h = world.registry.spawn(0, recipient);
	// The environment sub-block still rides a real deployed player's 0x0A.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate]
	ns::LoopbackChannel channel;
	std::vector<ns::Connection> connections;
	connections.push_back(ns::Connection{
			&channel, ns::TransportMode::Loopback, recipient_h, 0});
	connections.back().s2c_phase = 1;
	ns::test::emit_all(world, connections);

	ns::Datagram datagram;
	CHECK(channel.client_recv(datagram));
	nw::FrameUpdate frame;
	CHECK(nw::decode_frame_update(datagram.body.data(), datagram.body.size(),
			ns::class_for_type_id, frame));
	CHECK(frame.flags2 == 2);
	CHECK(frame.env.present);
	return frame;
}

void test_fov_uses_the_shared_weather_current_and_snapshot() {
    w::World world;
    world.weather.seed(w::WeatherSeed{});
    opennova::wac::CompileEnv compile_env;
    auto program = opennova::wac::compile_source("fov(40) store(v1)\n", compile_env);
    CHECK(program.ok());
    opennova::wac::WacVm vm;
    vm.load(program);
    vm.execute(world);
    auto &channels = world.weather.core.scalar_channels;
    CHECK(world.script.vars.get_mission(1) == 0);
    CHECK(channels.camera_fov_target_fp == (40 << 16));
    CHECK(channels.camera_fov_fp == (80 << 16));
    w::WeatherTickEvents events;
    world.weather.tick_sim(&world, events);
    CHECK(channels.camera_fov_fp == (75 << 16));
    CHECK(w::player_view_fov_h_deg(w::PlayerViewState{}, channels.camera_fov_fp,
                                   false, false, 1) == 75.0f);
    const auto baseline = world.snapshot();
    world.weather.command_fov(120);
    world.weather.tick_sim(&world, events);
    CHECK(channels.camera_fov_fp != (75 << 16));
    world.restore(baseline);
    CHECK(channels.camera_fov_fp == (75 << 16));
    CHECK(channels.camera_fov_target_fp == (40 << 16));
    // Whole-degree DWORD shift, including overflow; no camera-range clamp.
    world.weather.command_fov(65576);
    CHECK(channels.camera_fov_target_fp == (40 << 16));
    world.weather.command_fov(-1);
    CHECK(channels.camera_fov_target_fp == -65536);
    world.weather.command_fov(40);
    world.weather.mission_start_init();
    CHECK(channels.camera_fov_fp == (40 << 16));
    channels.camera_fov_target_fp = channels.camera_fov_fp - 1;
    world.weather.tick_sim(&world, events);
    CHECK(channels.camera_fov_fp == (40 << 16)); // negative <8-Q16 dead band
}

void test_scripted_sky_speed_reaches_the_wire() {
	w::World world;
	w::WeatherTickEvents events;
    // A mission only advances while a human is in the world - retail holds the
    // WAC tick and the BMS event pump on `g_WacVarHumans || !g_WacVarTicks`
    // (World::script_may_advance). These harnesses model a mission IN PROGRESS,
    // so they stand a player up; the empty-server hold has its own test.
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 733 << 16;
	base.tod_fixed24 = 9u << 24;
	base.cloud_scroll_rate_target = 13u << 10;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then skyspeed(47) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.cloud_scroll == 47);
}

void test_movefog_publishes_retail_target_and_duration_step() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 800 << 16;
	base.cloud_scroll_rate_target = 15 << 10;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then movefog(200,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 200);
	// IDA @0x4EE0A0: seconds*62 ticks; abs(target-current+ticks/2)/ticks,
	// then the phase-2 (+0xFF)>>8 projection. 800 -> 200 over 2 s = 0x04D7.
	CHECK(frame.env.fog_accel == 0x04D7);
}

void test_movefog_uses_live_fog_current() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 800 << 16;
	base.cloud_scroll_rate_target = 15 << 10;
	world.weather.seed(base);
	// The live spring current is 600 (an earlier fogdist landed it there)
	// while the authored .env target was 800.
	world.weather.core.scalar_channels.fog_dist_fp = 600 << 16;

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then movefog(200,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 200);
	// Retail derives the two-second transition from 600 -> 200, yielding 0x033A.
	CHECK(frame.env.fog_accel == 0x033A);
}

void test_rain_advances_on_the_explicit_weather_tick() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 500 << 16;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then rain(100,1) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.weather.tick_sim(&world, events);
	const nw::FrameUpdate frame = emit_phase2(world);
	// 100% -> 0x10000; duration 62 ticks gives step 1057. One tick's
	// current narrows from 1057 Q16 to unsigned 8.8 byte 4.
	CHECK(frame.env.rain_pct == 4);
	CHECK(frame.env.env_param == 0); // rain precipitation kind
}

void test_snow_kind_reaches_the_wire() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 500 << 16;
		world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then snow(100,1) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.weather.tick_sim(&world, events);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.rain_pct == 4);
	CHECK(frame.env.env_param == 1); // snow precipitation kind
}

void test_overcast_advances_on_the_explicit_weather_tick() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 500 << 16;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then overcast(50,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.weather.tick_sim(&world, events);
	const nw::FrameUpdate frame = emit_phase2(world);
	// 50% -> 0x8000; two seconds gives a 264-Q16 step, narrowed to 1.
	CHECK(frame.env.overcast == 1);
}

void test_quake_uses_retail_six_tick_units_and_countdown() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 500 << 16;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then quake(7) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.weather.tick_sim(&world, events);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.quake_ticks == 41); // 7*6 authored ticks, then one weather tick
}

void test_tod_uses_retail_minute_to_fixed24_multiply() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 500 << 16;
	base.tod_fixed24 = 9u << 24;
	world.weather.seed(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then TOD(05:30) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	const nw::FrameUpdate frame = emit_phase2(world);
	// Handler @0x4EDC70 stores minute-of-day * 0x44444. For 05:30 the
	// phase-2 (+0x1000)>>13 projection is exactly 0x2C00.
	CHECK(frame.env.tod_fixed == 0x2C00);
}

void test_rgb_operands_and_skyfog_alias_reach_color_channels() {
    w::World world;
    auto &core = world.weather.core;
    struct ColorCase {
        const char *command;
        opennova::env::WeatherColorBlock *block;
    };
    const ColorCase cases[] = {
        {"sun", &core.sun_block}, {"sky", &core.sky_block},
        {"ground", &core.fill_block}, {"floor", &core.sky_color_blocks.floor},
        {"ceiling", &core.sky_color_blocks.ceiling}, {"cloud", &core.sky_color_blocks.cloud},
        {"fog", &core.fog_block}, {"fogcolor", &core.fog_block},
        {"skyfog", &core.sky_color_blocks.skyfog},
        {"skyfogcolor", &core.sky_color_blocks.skyfog},
        {"crash", &core.sky_color_blocks.skyfog},
        {"gain", &core.modulator_chain.modulator},
    };
    for (const ColorCase &test : cases) {
        test.block->snap(0);
        const std::string source = std::string("colorfade(1)\nv1 = 62\nv2 = 124\nv3 = 186\n") +
                test.command + "(v1,v2,v3" + (std::string(test.command) == "crash" ? ",999" : "") +
                ") store(v4)\n";
        auto program = opennova::wac::compile_source(source, {});
        CHECK(program.ok());
        opennova::wac::WacVm vm; vm.load(program); vm.execute(world);
        CHECK(world.script.vars.get_mission(4) == 0);
        CHECK(test.block->target == 0x003E7CBAu);
        CHECK(test.block->max_rate[0] == 3 * 1048576);
        CHECK(test.block->max_rate[1] == 2 * 1048576);
        CHECK(test.block->max_rate[2] == 1048576);
        test.block->tick(opennova::env::kModulatorIdentityPacked, 0);
        CHECK(test.block->render_color == 0x00010203u);
    }
    // The fourth crash operand is ignored; overflowing RGB components carry
    // exactly as the retail shifts/adds do, including the packed alpha byte.
    auto program = opennova::wac::compile_source(
            "crash(257,258,259,123)\nlightning(-1,256,1)\n", {});
    CHECK(program.ok());
    opennova::wac::WacVm vm; vm.load(program); vm.execute(world);
    CHECK(core.sky_color_blocks.skyfog.target == 0x01020303u);
    CHECK(world.weather.lightning_color == 1u);
}

void test_eager_wac_initializer_and_255_tick_boundary() {
	w::World world;
	w::WeatherTickEvents events;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 800 << 16;
	base.tod_fixed24 = 9u << 24;
	base.tod_advance_per_tick = 0x1234u;
	base.cloud_scroll_rate_target = 15u << 10;
	world.weather.seed(base);
	world.weather.core.scalar_channels.fog_dist_fp = 600 << 16;

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then movefog(200,2) rain(100,1) overcast(50,2) "
			"quake(50) skyspeed(47) TOD(05:30) endif\n",
			compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	world.add_system(&system);
	world.load_systems();
	CHECK(system.execute_initial(world));
	CHECK(system.runs() == 1);
	CHECK(world.logic_tick == 0);

	world.weather.mission_start_init();
	for (int tick = 0; tick < 255; ++tick) world.weather.tick_sim(&world, events);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 200);
	CHECK(frame.env.fog_accel == 0xFF00);
	CHECK(frame.env.cloud_scroll == 47);
	CHECK(frame.env.rain_pct == 0xFF);
	CHECK(frame.env.overcast == 0x80);
	CHECK(frame.env.quake_ticks == 45); // 50*6, then exactly 255 ticks
	const uint32_t expected_tod =
			(330u * 0x44444u + 255u * base.tod_advance_per_tick) % (24u << 24);
	CHECK(frame.env.tod_fixed == static_cast<uint16_t>((expected_tod + 0x1000u) >> 13));

	// The live divider was untouched by eager execution and environment settling.
	for (int tick = 0; tick < opennova::wac::WacSystem::kTicksPerExecution - 1; ++tick)
		world.run_logic_tick(/*is_authority=*/true);
	CHECK(system.runs() == 1);
	world.run_logic_tick(/*is_authority=*/true);
	CHECK(system.runs() == 2);
}

} // namespace

int main() {
	test_fov_uses_the_shared_weather_current_and_snapshot();
	test_scripted_sky_speed_reaches_the_wire();
	test_movefog_publishes_retail_target_and_duration_step();
	test_movefog_uses_live_fog_current();
	test_rain_advances_on_the_explicit_weather_tick();
	test_snow_kind_reaches_the_wire();
	test_overcast_advances_on_the_explicit_weather_tick();
	test_quake_uses_retail_six_tick_units_and_countdown();
	test_tod_uses_retail_minute_to_fixed24_multiply();
	test_rgb_operands_and_skyfog_alias_reach_color_channels();
	test_eager_wac_initializer_and_255_tick_boundary();
	std::printf(failures ? "WAC ENVIRONMENT WIRE TEST FAILED (%d)\n"
	                     : "WAC environment wire test passed\n",
	            failures);
	return failures ? 1 : 0;
}
