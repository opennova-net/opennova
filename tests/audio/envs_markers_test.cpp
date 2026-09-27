// S13 (ADR 0028): the envs-marker resolution — the items.def DISPATCH-TAG
// gate (ai_function/move_function == "envs", case-insensitive, ANY entity
// pool), the four authored soundloop slot names passed through UNFILTERED
// (bank presence is the embedder's stream concern; the original has no gate —
// a missing set is silent), the canonical walk order, and the BMS type-id ->
// items.def id offset.
// [orig: dispatch table @0x82ABD4 -> Entity_UpdateEnvSoundEmitter @0x4a8080;
//  soundloop parse ItemDef_ParseProperty @0x49fec4]

#include <runtime/audio/envs_markers.h>

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

const char kItemsDef[] = R"(begin "Env building"
  id 100001
  type decoration
  move_function envs
  soundloop_1 BUILD_AMB
  soundloop_2 DAY_AMB
  soundloop_4 NIGHT_AMB
end

begin "Env marker upper"
  id 100002
  type marker
  ai_function ENVS
  soundloop_1 MARKER_AMB
end

begin "Not env"
  id 100003
  type marker
  soundloop_1 IGNORED_AMB
end
)";

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
	def_free_items(&items);
	if (!ok) return 1;
	std::printf("envs_markers_test: OK\n");
	return 0;
}
