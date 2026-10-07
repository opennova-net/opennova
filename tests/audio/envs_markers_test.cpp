// S13 (ADR 0028): the envs-marker resolution — the items.def DISPATCH-TAG
// gate (ai_function/move_function == "envs", case-insensitive, ANY entity
// pool), the four authored soundloop slot names passed through UNFILTERED
// (bank presence is the embedder's stream concern; the original has no gate —
// a missing set is silent), the canonical walk order, and the BMS type-id ->
// items.def id offset.
// [orig: dispatch table @0x82ABD4 -> Entity_UpdateEnvSoundEmitter @0x4a8080;
//  soundloop parse ItemDef_ParseProperty @0x49fec4]

#include <runtime/audio/envs_markers.h>
#include <runtime/world/world.h>

#include <array>
#include <memory>

#include <cstdio>
#include <cstring>
#include <string>

using namespace opennova::def;

namespace {

using namespace opennova;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const char kItemsDef[] = "begin \"Env building\"\r\n"
    "  id 100001\r\n"
    "  type decoration\r\n"
    "  move_function envs\r\n"
    "  soundloop_1 BUILD_AMB\r\n"
    "  soundloop_2 DAY_AMB\r\n"
    "  soundloop_4 NIGHT_AMB\r\n"
    "end\r\n"
    "\r\n"
    "begin \"Env marker upper\"\r\n"
    "  id 100002\r\n"
    "  type marker\r\n"
    "  ai_function ENVS\r\n"
    "  soundloop_1 MARKER_AMB\r\n"
    "end\r\n"
    "\r\n"
    "begin \"Not env\"\r\n"
    "  id 100003\r\n"
    "  type marker\r\n"
    "  soundloop_1 IGNORED_AMB\r\n"
    "end\r\n";

bms::Entity placed(int32_t type_id, int32_t id, int32_t x_fixed) {
	bms::Entity e{};
	e.type_id = type_id;
	e.id = id;
	e.x = x_fixed;
	return e;
}

// The fixture entry with this items.def id, or null.
const DefItemDef *entry(const DefItemsFile &items, int32_t id) {
	for (size_t i = 0; i < items.count; ++i) {
		if (items.entries[i].id == id) return &items.entries[i];
	}
	return nullptr;
}

} // namespace

int main() {
	DefItemsFile items{};
	if (!expect(def_parse_items_memory(
					reinterpret_cast<const uint8_t *>(kItemsDef),
					sizeof(kItemsDef) - 1, &items) == 0,
			"fixture items.def parses"))
		return 1;

	// item_is_envs: move_function OR ai_function, case-insensitive; untagged
	// items are not envs (an unknown id has no def: the walk below skips it).
	const auto is_envs = [&](int32_t id) {
		const DefItemDef *def = entry(items, id);
		return def != nullptr && audio::item_is_envs(*def);
	};
	bool ok = true;
	ok &= expect(is_envs(100001), "move_function envs");
	ok &= expect(is_envs(100002), "ai_function ENVS (case)");
	ok &= expect(entry(items, 100003) != nullptr && !is_envs(100003), "untagged item");
	if (!ok) {
		def_free_items(&items);
		return 1;
	}

	// The walk: buildings-pool envs decoration + marker-pool envs marker both
	// participate; the untagged marker does not. BMS type ids carry the
	// items.def offset (100000). Walk order is markers, items, buildings,
	// organics — the marker row lands first even though the building was
	// authored "first".
	bms::File mission{};
	mission.buildings.push_back(placed(1, 7, 10 << 16)); // 100001 envs building
	mission.markers.push_back(placed(2, 9, 20 << 16));   // 100002 envs marker
	mission.markers.push_back(placed(3, 11, 30 << 16));  // 100003 not envs
	mission.organics.push_back(placed(4242, 13, 0));     // unknown def id

	const std::vector<audio::EnvsMarker> rows =
			audio::resolve_envs_markers(mission, items);
	ok &= expect(rows.size() == 2, "two envs rows");
	if (rows.size() == 2) {
		ok &= expect(rows[0].bms_id == 9 && rows[0].x == 20.0f,
				"marker pool walks first");
		ok &= expect(rows[0].slot_sets[0] == "MARKER_AMB" &&
						rows[0].slot_sets[1].empty() &&
						rows[0].slot_sets[3].empty(),
				"marker slots: authored slot 1 only");
		ok &= expect(rows[1].bms_id == 7 && rows[1].x == 10.0f,
				"building row follows");
		ok &= expect(rows[1].slot_sets[0] == "BUILD_AMB" &&
						rows[1].slot_sets[1] == "DAY_AMB" &&
						rows[1].slot_sets[2].empty() &&
						rows[1].slot_sets[3] == "NIGHT_AMB",
				"building slots pass through unfiltered, gaps stay silent");
	}
	// A header-only join: the same rows from the streamed registry, in the
	// same pool walk, at the registry's mission-frame positions.
	// [orig: Entity_UpdateEnvSoundEmitter @0x4a8080 on streamed entities]
	auto world_heap = std::make_unique<world::World>();
	world::World &world = *world_heap;
	for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 8);
	const auto spawn = [&](int pool, int32_t type, int32_t bms_id, float x) {
		world::Entity seed;
		seed.item_id = type;
		seed.bms_id = bms_id;
		seed.position = world::Vec3{x, 5.0f, 2.0f};
		world.registry.spawn(pool, seed);
	};
	spawn(2, 1, 7, 10.0f);  // envs building
	spawn(3, 2, 9, 20.0f);  // envs marker
	spawn(3, 3, 11, 30.0f); // not envs
	spawn(0, 4242, 13, 0.0f);
	const std::vector<audio::EnvsMarker> streamed = audio::resolve_envs_markers(world, items);
	ok &= expect(streamed.size() == 2, "streamed: two envs rows");
	if (streamed.size() == 2) {
		ok &= expect(streamed[0].bms_id == 9 && streamed[0].x == 20.0f &&
						streamed[0].y == 5.0f && streamed[0].z == 2.0f &&
						streamed[0].slot_sets[0] == "MARKER_AMB",
				"streamed: the marker pool walks first, at the registry position");
		ok &= expect(streamed[1].bms_id == 7 && streamed[1].slot_sets[3] == "NIGHT_AMB",
				"streamed: the building row follows with its slots");
	}
	// The per-entity read every walk and the editor's Listen share (envs_slot_sets): an envs def's four slots, none
	// for another; and the stagger a resolved marker registers with (its place's low nibble).
	{
		std::array<std::string, 4> slots;
		ok &= expect(audio::envs_slot_sets(items.entries[0], slots) && slots[0] == "BUILD_AMB" &&
						slots[1] == "DAY_AMB" && slots[2].empty() && slots[3] == "NIGHT_AMB",
				"slots: the envs building's four slots");
		ok &= expect(!audio::envs_slot_sets(items.entries[2], slots), "slots: no envs, no slots");
		ok &= expect(audio::envs_stagger_slot(0) == 0 && audio::envs_stagger_slot(5) == 5 &&
						audio::envs_stagger_slot(17) == 1,
				"stagger: the place's low nibble");
	}
	def_free_items(&items);
	if (!ok) return 1;
	std::printf("envs_markers_test: OK\n");
	return 0;
}
