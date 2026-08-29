// The mission kernel (ADR 0042 d3) booted over a SYNTHETIC mission and an
// in-memory file source — ungated. Locks the promoted rig body's engine home:
// open_document + boot run the S9 order end to end (entities promoted, the
// layered WAC compiled and installed, the local player spawned at the origin
// when no start marker exists, the post-PreMission baseline captured), a
// tick_no_net advances the logic clock, the teleport/health seams round-trip
// through both stores, and the CanFire verdict answers over the spawned
// player. The retail-path legs stay in tests/common/retail_mission_files.
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

int main() {
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
	CHECK(kernel.by_net_id(21) != nullptr);
	CHECK(kernel.by_net_id(31) != nullptr);
	CHECK(kernel.events.events().size() == 1);
	CHECK(kernel.wac_loaded);
	CHECK(kernel.wac.vm().loaded());
	CHECK(kernel.has_local_player());
	CHECK(kernel.player() != nullptr);
	CHECK(kernel.player_ai() != nullptr);
	CHECK(near_equal(kernel.player_position().x, 0.0f, 0.001f));
	CHECK(near_equal(kernel.player_position().y, 0.0f, 0.001f));
	CHECK(kernel.have_baseline);
	CHECK(!kernel.has_terrain()); // no terrain documents were supplied
	CHECK(kernel.text_source == ms::MissionTextSource::kNone);

	// One no-net tick advances the authoritative logic clock.
	const uint32_t tick0 = kernel.world.logic_tick;
	kernel.tick_no_net();
	CHECK(kernel.world.logic_tick == tick0 + 1);
	kernel.tick_no_net();
	CHECK(kernel.world.logic_tick == tick0 + 2);

	// Teleport writes BOTH stores: the registry position and the AI 16.16
	// mirror, with the input-owned view seeded to the new facing.
	const w::EntityHandle player_h = kernel.player()->handle;
	kernel.teleport_local_player(w::Vec3{100.0f, 200.0f, 5.0f}, /*yaw_deg=*/90.0, /*pitch_deg=*/0.0);
	CHECK(near_equal(kernel.player_position().x, 100.0f, 0.001f));
	CHECK(near_equal(kernel.player_position().y, 200.0f, 0.001f));
	CHECK(near_equal(kernel.player_position().z, 5.0f, 0.001f));
	if (const w::AiEntity *body = kernel.player_ai()) {
		CHECK(body->pos[0] == 100 << 16);
		CHECK(body->pos[1] == 200 << 16);
		CHECK(body->pos[2] == 5 << 16);
		CHECK(kernel.input.look_heading == body->heading);
	}

	// set_entity_position / set_entity_health round-trip the same dual store.
	kernel.set_entity_position(player_h, w::Vec3{50.0f, 60.0f, 2.0f});
	CHECK(near_equal(kernel.player_position().x, 50.0f, 0.001f));
	if (const w::AiEntity *body = kernel.player_ai()) CHECK(body->pos[1] == 60 << 16);
	kernel.set_entity_health(player_h, 37);
	CHECK(kernel.player_health() == 37);
	CHECK(kernel.player()->alive);
	if (const w::AiEntity *body = kernel.player_ai()) CHECK(body->health == 37);
	kernel.set_entity_health(player_h, 0);
	CHECK(kernel.player_health() == 0);
	CHECK(!kernel.player()->alive);
	kernel.set_entity_health(player_h, 100);

	// The CanFire verdict on the spawned player: no weapon table was loaded
	// (no weapon.def in the source), so the local weapon is inactive and the
	// verdict is a hard no — the same gate the pre-tick stamps onto
	// inf.aimed_shot_available.
	CHECK(!kernel.weapon.active);
	CHECK(!kernel.local_player_can_fire(kernel.player_ai()));
	kernel.tick_no_net();
	if (const w::AiEntity *body = kernel.player_ai()) CHECK(!body->inf.aimed_shot_available);

	// The baseline restores the post-PreMission world.
	CHECK(kernel.restore_baseline());

	if (failures == 0) std::printf("mission_kernel: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
