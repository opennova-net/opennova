// WAC-authored weather must survive an external base-sample refresh and be
// observable through the real S2C 0x0A phase-2 projection.

#include "wac/compiler.h"
#include "wac/wac_system.h"

#include "netsim/conn_fan_test_util.h"

#include <netsim/entity_wire_bridge.h>
#include <netsim/loopback_channel.h>
#include <npwire/ingame_decode.h>
#include <world/world.h>

#include <cstdio>
#include <vector>

namespace {

namespace ns = opennova::netsim;
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

nw::FrameUpdate emit_phase2(const w::World &world) {
	ns::LoopbackChannel channel;
	std::vector<ns::Connection> connections;
	connections.push_back(ns::Connection{
			&channel, ns::TransportMode::Loopback, w::EntityHandle{}, 0});
	connections.back().s2c_phase = 1;
	nw::PlayerReplicationState fallback;
	ns::test::emit_all(world, connections, fallback);

	ns::Datagram datagram;
	CHECK(channel.client_recv(datagram));
	nw::FrameUpdate frame;
	CHECK(nw::decode_frame_update(datagram.body.data(), datagram.body.size(),
			ns::class_for_type_id, frame));
	CHECK(frame.flags2 == 2);
	CHECK(frame.env.present);
	return frame;
}

void test_scripted_sky_speed_survives_external_refresh() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 733 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	base.tod_fixed24 = 9u << 24;
	base.cloud_scroll_rate_target = 13u << 10;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then skyspeed(47) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	// GameWorld republishes its resource/weather sample on the following 62 Hz
	// quantum. The WAC-owned cloud target remains authoritative.
	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.cloud_scroll == 47);
}

void test_movefog_publishes_retail_target_and_duration_step() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 800 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	base.cloud_scroll_rate_target = 15 << 10;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then movefog(200,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 200);
	// IDA @0x4EE0A0: seconds*62 ticks; abs(target-current+ticks/2)/ticks,
	// then the phase-2 (+0xFF)>>8 projection. 800 -> 200 over 2 s = 0x04D7.
	CHECK(frame.env.fog_accel == 0x04D7);
}

void test_movefog_uses_live_external_fog_current() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 800 << 16;
	base.fog_current_q16 = 600 << 16;
	base.cloud_scroll_rate_target = 15 << 10;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then movefog(200,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 200);
	// The live spring current is 600 even though its authored target is 800.
	// Retail derives the two-second transition from 600 -> 200, yielding 0x033A.
	CHECK(frame.env.fog_accel == 0x033A);
}

void test_rain_advances_on_the_explicit_weather_tick() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 500 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then rain(100,1) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.advance_tick();
	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	// 100% -> 0x10000; duration 62 ticks gives step 1057. One tick's
	// current narrows from 1057 Q16 to unsigned 8.8 byte 4.
	CHECK(frame.env.rain_pct == 4);
	CHECK(frame.env.env_param == 0); // rain precipitation kind
}

void test_snow_kind_survives_external_refresh() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 500 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	base.precipitation_kind = static_cast<uint32_t>(w::PrecipitationKind::Rain);
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then snow(100,1) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.advance_tick();
	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.rain_pct == 4);
	CHECK(frame.env.env_param == 1); // snow precipitation kind
}

void test_overcast_advances_on_the_explicit_weather_tick() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 500 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then overcast(50,2) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.advance_tick();
	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	// 50% -> 0x8000; two seconds gives a 264-Q16 step, narrowed to 1.
	CHECK(frame.env.overcast == 1);
}

void test_quake_uses_retail_six_tick_units_and_countdown() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 500 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then quake(7) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.advance_tick();
	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.quake_ticks == 41); // 7*6 authored ticks, then one weather tick
}

void test_tod_uses_retail_minute_to_fixed24_multiply() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 500 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	base.tod_fixed24 = 9u << 24;
	world.network_env.publish_complete(base);

	opennova::wac::CompileEnv compile_env;
	opennova::wac::WacSystem system;
	auto program = opennova::wac::compile_source(
			"if never then TOD(05:30) endif\n", compile_env);
	CHECK(program.ok());
	system.set_program(std::move(program));
	run_one_wac_execution(world, system);

	world.network_env.publish_complete(base);
	const nw::FrameUpdate frame = emit_phase2(world);
	// Handler @0x4EDC70 stores minute-of-day * 0x44444. For 05:30 the
	// phase-2 (+0x1000)>>13 projection is exactly 0x2C00.
	CHECK(frame.env.tod_fixed == 0x2C00);
}

void test_eager_wac_initializer_and_255_tick_boundary() {
	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 800 << 16;
	base.fog_current_q16 = 600 << 16;
	base.tod_fixed24 = 9u << 24;
	base.tod_advance_per_tick = 0x1234u;
	base.cloud_scroll_rate_target = 15u << 10;
	world.network_env.publish_complete(base);

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

	world.network_env.initialize_mission_start();
	for (int tick = 0; tick < 255; ++tick) world.network_env.advance_tick();
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
	test_scripted_sky_speed_survives_external_refresh();
	test_movefog_publishes_retail_target_and_duration_step();
	test_movefog_uses_live_external_fog_current();
	test_rain_advances_on_the_explicit_weather_tick();
	test_snow_kind_survives_external_refresh();
	test_overcast_advances_on_the_explicit_weather_tick();
	test_quake_uses_retail_six_tick_units_and_countdown();
	test_tod_uses_retail_minute_to_fixed24_multiply();
	test_eager_wac_initializer_and_255_tick_boundary();
	std::printf(failures ? "WAC ENVIRONMENT WIRE TEST FAILED (%d)\n"
	                     : "WAC environment wire test passed\n",
	            failures);
	return failures ? 1 : 0;
}
