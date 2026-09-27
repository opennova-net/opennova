// The engine's mission-start weather seed (runtime/environment/weather_seed):
// the host seeds the weather home from the mission-selected resources (the
// ENV parse + BMS header overrides) and runs retail's 255-tick weather settle
// before a client can receive phase 2, through the same real wire projection.

#include "netsim/conn_fan_test_util.h"

#include <formats/env/env.h>
#include <runtime/inmatch/loopback_channel.h>
#include <net/npwire/ingame_decode.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace ns = opennova::replication;
namespace nw = opennova;
namespace env = opennova::env;
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

// The shell's order: parse the ENV, fold the mission header's override layer
// onto it, then seed from that config and the header clock
// [orig: Game_LoadTerrainDuringConnect @0x520710 mutates the parsed ENV before
// the Game_StartMission snapshot @0x525383/0x525393].
bool seed_from_env(const char *text, const opennova::bms::Header &header, w::World &world) {
	std::istringstream input(text);
	env::Config config;
	std::string error;
	if (!env::load_env(input, config, error)) return false;
	const int no_rgb[3] = {0, 0, 0};
	env::apply_bms_overrides(config, env::bms_env_overrides_from_header(
			static_cast<uint32_t>(header.attrib_flags), 0, header.fog_override, no_rgb, no_rgb, 0));
	world.weather.seed(env::weather_seed_from_config(config, header));
	return true;
}

nw::FrameUpdate emit_phase2(w::World &world) {
	world.registry.configure_pool(0, 1);
	w::Entity recipient;
	recipient.kind = w::EntityKind::Organic;
	recipient.health = 150;
	const w::EntityHandle recipient_h = world.registry.spawn(0, recipient);
	// The server environment projection rides a real deployed player's 0x0A.
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

void test_resource_values_reach_the_real_wire_projection() {
	w::World world;
    // A mission only advances while a human is in the world - retail holds the
    // WAC tick and the BMS event pump on `g_WacVarHumans || !g_WacVarTicks`
    // (World::script_may_advance). These harnesses model a mission IN PROGRESS,
    // so they stand a player up; the empty-server hold has its own test.
	world.cached.humans = 1;
	opennova::bms::Header header{};
	header.start_time = 0x0A80; // 10.5 hours in the BMS Q8.8 clock
	header.minutes_per_day = 123;
	CHECK(seed_from_env(
			"enviro_name \"Parity\"\n"
			"fog_level 733\n"
			"sky_speed 37\n",
			header, world));
	CHECK(world.weather.valid);

	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 733);
	CHECK(frame.env.fog_accel == 0xFF00);
	CHECK(frame.env.tod_fixed == 0x5400);
	CHECK(frame.env.cloud_scroll == 37);
	CHECK(frame.env.quake_ticks == 0);
	CHECK(frame.env.rain_pct == 0);
	CHECK(frame.env.overcast == 0);
	CHECK(frame.env.env_param == 0);

	const uint32_t before = world.weather.tod_fixed24;
	w::WeatherTickEvents events;
	world.weather.tick_sim(&world, events);
	const uint32_t expected_rate = (24u << 24) / (3720u * 123u);
	CHECK(world.weather.tod_fixed24 - before == expected_rate);
}

void test_bms_fog_override_precedes_the_environment_resource() {
	w::World world;
	world.cached.humans = 1;
	opennova::bms::Header header{};
	header.attrib_flags = opennova::bms::AttribFlags::FogDistanceOverrideEnable;
	header.fog_override = 811;
	header.minutes_per_day = 60;
	CHECK(seed_from_env("fog_level 733\nsky_speed 19\n", header, world));
	CHECK(emit_phase2(world).env.fog_dist == 811);
}

void test_mission_start_prewarms_255_environment_ticks() {
	w::World world;
	world.cached.humans = 1;
	opennova::bms::Header header{};
	header.start_time = 0x0540;
	header.minutes_per_day = 60;
	CHECK(seed_from_env("fog_level 733\nsky_speed 19\n", header, world));
	const uint32_t start = world.weather.tod_fixed24;
	const uint32_t rate = (24u << 24) / (3720u * 60u);
	w::WeatherTickEvents events;
	world.weather.mission_start_init();
	for (int tick = 0; tick < 255; ++tick) world.weather.tick_sim(&world, events);
	CHECK(world.weather.tod_fixed24 ==
			(start + 255u * rate) % (24u << 24));
}

void test_prewarm_runs_complete_weather_ticks_after_eager_scripts() {
	// A mission-start script command that seeds weather state (quake writes
	// 6 * 50 = 300 complete weather ticks) must be prewarmed through: the
	// embedders run the eager WAC execution first (the kernel boot's tail),
	// then the 255-tick prewarm, leaving exactly 45.
	w::World world;
	world.cached.humans = 1;
	w::WeatherSeed base;
	base.fog_level_q16 = 733 << 16;
	base.tod_fixed24 = 100;
	base.tod_advance_per_tick = 7;
	world.weather.seed(base);
	world.weather.command_quake(50);
	w::WeatherTickEvents events;
	world.weather.mission_start_init();
	for (int tick = 0; tick < 255; ++tick) world.weather.tick_sim(&world, events);
	CHECK(world.weather.quake_ticks == 45);
	CHECK(world.weather.tod_fixed24 == 100 + 255 * 7);
}

} // namespace

int main() {
	test_resource_values_reach_the_real_wire_projection();
	test_bms_fog_override_precedes_the_environment_resource();
	test_mission_start_prewarms_255_environment_ticks();
	test_prewarm_runs_complete_weather_ticks_after_eager_scripts();
	std::printf(failures ? "WEATHER SEED TEST FAILED (%d)\n"
	                     : "weather seed test passed\n",
	            failures);
	return failures ? 1 : 0;
}
