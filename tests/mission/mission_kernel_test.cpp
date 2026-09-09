// The mission kernel (ADR 0042 d3) booted over a SYNTHETIC mission and an
// in-memory file source — ungated. Locks the promoted rig body's engine home:
// open_document + boot run the S9 order end to end (entities promoted, the
// layered WAC compiled and installed, the local player spawned at the origin
// when no start marker exists, the post-PreMission baseline captured), a
// the local role's tick advances the logic clock, the teleport/health seams round-trip
// through both stores, and the CanFire verdict answers over the spawned
// player. The retail-path legs stay in tests/common/retail_mission_files.
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

bms::Entity organic(int32_t x, int32_t y, int32_t z, uint8_t team) {
	bms::Entity e{};
	e.type = bms::ItemType::Organic;
	e.x = x;
	e.y = y;
	e.z = z;
	e.yaw = 90;
	e.team = team;
	return e;
}

bms::Entity item(int32_t type_id, int32_t x, int32_t y, int32_t z) {
	bms::Entity e{};
	e.type = bms::ItemType::Item;
	e.type_id = type_id;
	e.x = x;
	e.y = y;
	e.z = z;
	return e;
}

ms::BootFileSource source_over(const std::map<std::string, std::string> *files) {
	ms::BootFileSource s;
	s.has_file = [files](const std::string &name) {
		return files->find(name) != files->end();
	};
	s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto it = files->find(name);
		if (it == files->end()) return false;
		out.assign(it->second.begin(), it->second.end());
		return true;
	};
	return s;
}

bool near_equal(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

// The boot gates through the trace: a joiner never spawns its own player
// here (L spawns on the name-match inside the joiner frame); a rootless boot
// keeps only the root-free steps.
static void run_boot_trace_gates() {
	std::map<std::string, std::string> files;
	{
		bms::File m{};
		m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1));
		ms::MissionKernel kernel;
		kernel.open_document(std::move(m), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.joiner = true;
		std::string error;
		CHECK(kernel.boot(options, error));
		const std::vector<std::string> expected = {
				"ai_profiles", "mission_text", "load_mission", "infantry_anim", "wac",
				"weapon_table", "ammo_table", "organic_init", "premission"};
		CHECK(kernel.boot_trace == expected);
		CHECK(!kernel.local.has_local_player());
	}
	{
		bms::File m{};
		m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1));
		ms::MissionKernel kernel;
		kernel.open_document(std::move(m), "synth", ms::BootFileSource{});
		ms::KernelBootOptions options;
		std::string error;
		CHECK(kernel.boot(options, error));
		const std::vector<std::string> expected = {
				"mission_text", "load_mission", "spawn_local_player", "organic_init", "premission"};
		CHECK(kernel.boot_trace == expected);
		CHECK(kernel.local.has_local_player());
	}
}

// The bare no-net tick: the local role over the kernel (ADR 0043 d3; the
// kernel itself owns no tick).
static void tick_no_net(opennova::mission::MissionKernel &kernel) {
	opennova::inmatch::LocalRole role;
	role.bind(kernel);
	role.run_tick(opennova::inmatch::TickInput{});
}

static void test_initial_wac_waits_for_the_weather_owner_once() {
    std::map<std::string, std::string> files;
    files["synth.wac"] = "if never then inc(v1) fov(40) endif\n";
    ms::MissionKernel kernel;
    kernel.open_document(bms::File{}, "synth", source_over(&files));
    ms::KernelBootOptions options;
    options.playable = false;
    options.defer_initial_wac = true;
    std::string error;
    CHECK(kernel.boot(options, error));
    CHECK(kernel.wac.runs() == 0);
    CHECK(kernel.world.script.vars.get_mission(1) == 0);
    kernel.world.weather.seed(w::WeatherSeed{});
    CHECK(kernel.wac.execute_initial(kernel.world));
    CHECK(!kernel.wac.execute_initial(kernel.world));
    kernel.settle_weather_mission_start();
    CHECK(kernel.world.script.vars.get_mission(1) == 1);
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_fp == (40 << 16));
    kernel.capture_baseline();
    kernel.world.weather.command_fov(120);
    kernel.tick_weather();
    CHECK(kernel.restore_baseline());
    CHECK(kernel.world.script.vars.get_mission(1) == 1);
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_fp == (40 << 16));
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_target_fp == (40 << 16));
    CHECK(!kernel.wac.execute_initial(kernel.world));
}

int main() {
	test_initial_wac_waits_for_the_weather_owner_once();
	// The synthetic mission: two placed entities plus one (empty) BMS event,
	// and a mission-named WAC layer in the in-memory source.
	std::map<std::string, std::string> files;
	files["synth.wac"] = "if never() then set(v1,1) endif\n";

	bms::File m{};
	m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 3 << 16));
	m.items[0].id = 21;
	m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1));
	m.organics[0].id = 31;
	m.events.push_back(bms::Event{});

	ms::MissionKernel kernel;
	kernel.open_document(std::move(m), "synth", source_over(&files));

	ms::KernelBootOptions options; // playable, wac, collision, seat_specs on; game_type 0 (SP)
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(error.empty());

	// Boot side effects, in the S9 order's observable residue: the promote,
	// the BMS event registration, the layered WAC install, the origin spawn
	// (no start marker in the synthetic mission), the baseline capture.
	CHECK(kernel.promo.spawned == 2);
	CHECK(kernel.promo.dropped == 0);
	CHECK(kernel.world.registry.by_net_id(21) != nullptr);
	CHECK(kernel.world.registry.by_net_id(31) != nullptr);
	CHECK(kernel.events.events().size() == 1);
	CHECK(kernel.wac_loaded);
	CHECK(kernel.wac.vm().loaded());
	CHECK(kernel.local.has_local_player());
	CHECK(kernel.local.player() != nullptr);
	CHECK(kernel.local.player_ai() != nullptr);
	CHECK(near_equal(kernel.local.player_position().x, 0.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().y, 0.0f, 0.001f));
	CHECK(kernel.have_baseline);
	CHECK(!kernel.has_terrain()); // no terrain documents were supplied
	CHECK(kernel.text_source == ms::MissionTextSource::kNone);
	// The boot ORDER (ADR 0043 slice E9): the file source is present, no item
	// db (no items.def in the source), no terrain, a playable non-joiner --
	// the trace is the literal step sequence with those gates applied.
	{
		const std::vector<std::string> expected = {
				"ai_profiles", "mission_text", "load_mission", "infantry_anim", "wac",
				"spawn_local_player", "weapon_table", "ammo_table", "organic_init", "premission"};
		CHECK(kernel.boot_trace == expected);
		if (kernel.boot_trace != expected)
			for (const std::string &s : kernel.boot_trace) std::printf("  trace: %s\n", s.c_str());
	}
	run_boot_trace_gates();

	// One no-net tick advances the authoritative logic clock.
	const uint32_t tick0 = kernel.world.logic_tick;
	tick_no_net(kernel);
	CHECK(kernel.world.logic_tick == tick0 + 1);
	tick_no_net(kernel);
	CHECK(kernel.world.logic_tick == tick0 + 2);

	// The camera shake: the counter decays ONCE per tick in the pre-tick pass
	// (never per frame), while every composed frame advances the IIR filters
	// again from the same PRNG word — two frames between ticks differ
	// [orig: @ 0x4de590; Camera_ComputeThirdPersonView @ 0x526781 / @ 0x5ca34d].
	kernel.local.view.shake.counter = 10;
	tick_no_net(kernel);
	CHECK(kernel.local.view.shake.counter == 8);
	{
		const w::LocalPlayerViewFrame frame_a = kernel.local.view_frame();
		const w::LocalPlayerViewFrame frame_b = kernel.local.view_frame();
		CHECK(kernel.local.view.shake.counter == 8);
		CHECK(frame_a.camera.yaw_deg != frame_b.camera.yaw_deg ||
		      frame_a.camera.pitch_deg != frame_b.camera.pitch_deg ||
		      frame_a.camera.roll_deg != frame_b.camera.roll_deg);
	}
	tick_no_net(kernel);
	CHECK(kernel.local.view.shake.counter == 6);
	kernel.local.view.shake.counter = 0;

	// Teleport writes BOTH stores: the registry position and the AI 16.16
	// mirror, with the input-owned view seeded to the new facing.
	const w::EntityHandle player_h = kernel.local.player()->handle;
	kernel.local.teleport_local_player(w::Vec3{100.0f, 200.0f, 5.0f}, /*yaw_deg=*/90.0, /*pitch_deg=*/0.0);
	CHECK(near_equal(kernel.local.player_position().x, 100.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().y, 200.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().z, 5.0f, 0.001f));
	if (const w::AiEntity *body = kernel.local.player_ai()) {
		CHECK(body->pos[0] == 100 << 16);
		CHECK(body->pos[1] == 200 << 16);
		CHECK(body->pos[2] == 5 << 16);
		CHECK(kernel.local.input.look_heading == body->heading);
	}

	// set_entity_position / set_entity_health round-trip the same dual store.
	kernel.world.commands.set_entity_position(player_h, w::Vec3{50.0f, 60.0f, 2.0f});
	CHECK(near_equal(kernel.local.player_position().x, 50.0f, 0.001f));
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(body->pos[1] == 60 << 16);
	kernel.world.commands.set_entity_health(player_h, 37);
	CHECK(kernel.local.player_health() == 37);
	CHECK(kernel.local.player()->alive);
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(body->health == 37);
	kernel.world.commands.set_entity_health(player_h, 0);
	CHECK(kernel.local.player_health() == 0);
	CHECK(!kernel.local.player()->alive);
	kernel.world.commands.set_entity_health(player_h, 100);

	// The CanFire verdict on the spawned player: no weapon table was loaded
	// (no weapon.def in the source), so the local weapon is inactive and the
	// verdict is a hard no — the same gate the pre-tick stamps onto
	// inf.aimed_shot_available.
	CHECK(!kernel.local.weapon.active);
	CHECK(!kernel.local.local_player_can_fire());
	tick_no_net(kernel);
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(!body->inf.aimed_shot_available);

	// The baseline restores the post-PreMission world.
	CHECK(kernel.restore_baseline());

	if (failures == 0) std::printf("mission_kernel: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
