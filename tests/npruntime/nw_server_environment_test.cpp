// nw_server must seed the same authoritative environment owner as Godot from
// the mission-selected resources before a client can receive phase 2.

#include "environment_startup.h"
#include "wac_startup.h"
#include "netsim/conn_fan_test_util.h"

#include <mission/event_runtime.h>
#include <netsim/loopback_channel.h>
#include <npwire/ingame_decode.h>
#include <wac/wac_system.h>
#include <world/ai.h>
#include <world/world.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace ns = opennova::netsim;
namespace nw = opennova;
namespace server = opennova::nw_server;
namespace w = opennova::world;
namespace wc = opennova::wac;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

class TempResourceRoot {
public:
	TempResourceRoot() {
		static uint64_t sequence = 0;
		const uint64_t stamp = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
		path = std::filesystem::temp_directory_path() /
				("opennova-nw-server-wac-" + std::to_string(stamp) + "-" +
				 std::to_string(++sequence));
		std::error_code error;
		std::filesystem::create_directories(path, error);
		CHECK(!error);
	}

	~TempResourceRoot() {
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	void write(const std::string &name, const std::string &source) const {
		std::ofstream output(path / name, std::ios::binary);
		output << source;
		CHECK(output.good());
	}

	std::filesystem::path path;
};

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
	opennova::bms::Header header{};
	header.start_time = 0x0A80; // 10.5 hours in the BMS Q8.8 clock
	header.minutes_per_day = 123;
	std::istringstream input(
			"enviro_name \"Parity\"\n"
			"fog_level 733\n"
			"sky_speed 37\n");
	std::string error;
	CHECK(server::publish_initial_environment(
			input, header, world.network_env, error));
	CHECK(error.empty());
	CHECK(world.network_env.valid);

	const nw::FrameUpdate frame = emit_phase2(world);
	CHECK(frame.env.fog_dist == 733);
	CHECK(frame.env.fog_accel == 0xFF00);
	CHECK(frame.env.tod_fixed == 0x5400);
	CHECK(frame.env.cloud_scroll == 37);
	CHECK(frame.env.quake_ticks == 0);
	CHECK(frame.env.rain_pct == 0);
	CHECK(frame.env.overcast == 0);
	CHECK(frame.env.env_param == 0);

	const uint32_t before = world.network_env.tod_fixed24;
	world.network_env.advance_tick();
	const uint32_t expected_rate = (24u << 24) / (3720u * 123u);
	CHECK(world.network_env.tod_fixed24 - before == expected_rate);
}

void test_bms_fog_override_precedes_the_environment_resource() {
	w::World world;
	opennova::bms::Header header{};
	header.attrib_flags = opennova::bms::AttribFlags::FogDistanceOverrideEnable;
	header.fog_override = 811;
	header.minutes_per_day = 60;
	std::istringstream input("fog_level 733\nsky_speed 19\n");
	std::string error;
	CHECK(server::publish_initial_environment(
			input, header, world.network_env, error));
	CHECK(emit_phase2(world).env.fog_dist == 811);
}

void test_headless_startup_prewarms_255_environment_ticks() {
	w::World world;
	opennova::bms::Header header{};
	header.start_time = 0x0540;
	header.minutes_per_day = 60;
	std::istringstream input("fog_level 733\nsky_speed 19\n");
	std::string error;
	CHECK(server::publish_initial_environment(
			input, header, world.network_env, error));
	const uint32_t start = world.network_env.tod_fixed24;
	const uint32_t rate = (24u << 24) / (3720u * 60u);
	server::prewarm_initial_environment(world.network_env);
	CHECK(world.network_env.tod_fixed24 ==
			(start + 255u * rate) % (24u << 24));
}

void test_wac_resource_root_is_mission_relative_unless_overridden() {
	const std::filesystem::path mission =
			std::filesystem::path("missions") / "coop" / "sample.bms";
	CHECK(server::resolve_resource_root(mission, {}) ==
			std::filesystem::path("missions") / "coop");
	CHECK(server::resolve_resource_root(
			mission, std::filesystem::path("retail") / "resources") ==
			std::filesystem::path("retail") / "resources");
}

void test_wac_layers_execute_in_retail_order() {
	TempResourceRoot root;
	root.write("game.wac", "if never then set(v1,1) endif\n");
	root.write("server.wac", "if never then set(v1,2) endif\n");
	root.write("sample.wac", "if never then set(v1,3) endif\n");

	w::World world;
	wc::WacSystem wac;
	std::string error;
	CHECK(server::load_wac_program(
			root.path, "sample", world, wac, error) ==
			server::WacLoadStatus::Loaded);
	CHECK(error.empty());
	world.add_system(&wac);
	world.load_systems();
	CHECK(wac.execute_initial(world));
	CHECK(wac.runs() == 1);
	CHECK(world.vars.get_mission(1) == 3);
}

void test_complete_startup_executes_wac_before_255_weather_ticks() {
	TempResourceRoot root;
	root.write("sample.wac",
			"if never then set(v4,1) endif\n"
			"if never then quake(50) endif\n");

	w::World world;
	w::EnvNetworkSample base;
	base.fog_target_q16 = 733 << 16;
	base.fog_current_q16 = base.fog_target_q16;
	base.tod_fixed24 = 100;
	base.tod_advance_per_tick = 7;
	world.network_env.publish_complete(base);
	w::AiSystem ai;
	world.ai = &ai;
	wc::WacSystem wac;
	opennova::mission::BmsEventSystem bms;
	opennova::bms::File mission;
	bool wac_loaded = false;
	std::string error;
	CHECK(server::initialize_mission_startup(
			root.path, "sample", mission, world, wac, bms, ai,
			wac_loaded, error));
	CHECK(error.empty());
	CHECK(wac_loaded);
	CHECK(wac.runs() == 1);
	CHECK(world.vars.get_mission(4) == 1);
	// quake(50) writes 300 complete weather ticks. The startup prewarm must
	// happen after the eager WAC execution, leaving exactly 45.
	CHECK(world.network_env.quake_ticks == 45);
	CHECK(world.network_env.tod_fixed24 == 100 + 255 * 7);

	for (int tick = 0; tick < wc::WacSystem::kTicksPerExecution - 1; ++tick)
		world.run_logic_tick(/*is_authority=*/true);
	CHECK(wac.runs() == 1);
	world.run_logic_tick(/*is_authority=*/true);
	CHECK(wac.runs() == 2);
}

void test_absent_wac_is_valid_bms_only_startup() {
	TempResourceRoot root;
	w::World world;
	w::EnvNetworkSample base;
	base.tod_fixed24 = 200;
	base.tod_advance_per_tick = 3;
	world.network_env.publish_complete(base);
	w::AiSystem ai;
	world.ai = &ai;
	wc::WacSystem wac;
	opennova::mission::BmsEventSystem bms;
	opennova::bms::File mission;
	bool wac_loaded = true;
	std::string error;
	CHECK(server::initialize_mission_startup(
			root.path, "sample", mission, world, wac, bms, ai,
			wac_loaded, error));
	CHECK(!wac_loaded);
	CHECK(wac.runs() == 0);
	CHECK(world.network_env.tod_fixed24 == 200 + 255 * 3);
}

void test_invalid_wac_fails_before_system_or_environment_startup() {
	TempResourceRoot root;
	root.write("server.wac", "if never then bogus_command_xyz(1) endif\n");
	w::World world;
	w::EnvNetworkSample base;
	base.tod_fixed24 = 321;
	base.tod_advance_per_tick = 9;
	world.network_env.publish_complete(base);
	w::AiSystem ai;
	world.ai = &ai;
	wc::WacSystem wac;
	opennova::mission::BmsEventSystem bms;
	opennova::bms::File mission;
	bool wac_loaded = false;
	std::string error;
	CHECK(!server::initialize_mission_startup(
			root.path, "sample", mission, world, wac, bms, ai,
			wac_loaded, error));
	CHECK(error.find("failed to compile") != std::string::npos);
	CHECK(world.logic_tick == 0);
	CHECK(world.network_env.tod_fixed24 == 321);
}

void test_resource_path_is_mission_relative_unless_explicitly_overridden() {
	const std::filesystem::path mission =
			std::filesystem::path("missions") / "coop" / "sample.bms";
	CHECK(server::resolve_environment_path(mission, "FULL_03", {}) ==
			std::filesystem::path("missions") / "coop" / "FULL_03.env");
	CHECK(server::resolve_environment_path(
			mission, "FULL_03", std::filesystem::path("retail") / "snow.env") ==
			std::filesystem::path("retail") / "snow.env");
}

void test_missing_resource_fails_before_session_launch() {
	w::EnvNetworkState state;
	opennova::bms::Header header{};
	std::string error;
	const std::filesystem::path missing =
			std::filesystem::path("definitely-missing") / "parity.env";
	CHECK(!server::publish_initial_environment_file(
			missing, header, state, error));
	CHECK(!state.valid);
	CHECK(error.find("definitely-missing") != std::string::npos);
}

} // namespace

int main() {
	test_resource_values_reach_the_real_wire_projection();
	test_bms_fog_override_precedes_the_environment_resource();
	test_headless_startup_prewarms_255_environment_ticks();
	test_wac_resource_root_is_mission_relative_unless_overridden();
	test_wac_layers_execute_in_retail_order();
	test_complete_startup_executes_wac_before_255_weather_ticks();
	test_absent_wac_is_valid_bms_only_startup();
	test_invalid_wac_fails_before_system_or_environment_startup();
	test_resource_path_is_mission_relative_unless_explicitly_overridden();
	test_missing_resource_fails_before_session_launch();
	std::printf(failures ? "NW SERVER ENVIRONMENT TEST FAILED (%d)\n"
	                     : "nw-server environment test passed\n",
	            failures);
	return failures ? 1 : 0;
}
