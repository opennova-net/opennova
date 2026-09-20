// Generator + guard for fixtures/bms/synth_dense.bms: a synthetic mission
// authored through the bms_edit free functions (the same seam ONED writes with) and
// serialized by bms::write, dense the way the shipped missions are — every
// entity pool populated (items, buildings, markers, organics; a few hundred
// records on a grid, one item id per pool so the .mis pool classification
// stays 1:1), three waypoint paths, the full waypoint/group/layer record
// tables, and zero-valued optional fields on every record (the D-MIS-5 shape:
// an authoring default the parser seeds must not mutate a zero it read). The
// header names the minted terrain (Tmap) and environment (synth_full)
// fixtures. No retail mission is carried: the shipped ash_i5b is
// mission_mis_idempotency's reference-tree leg, and the whole shipped corpus
// is the gated mission_corpus sweep.
//
// Default: rebuild in memory and byte-compare the committed file. `--write`
// (re)writes it.
#include "common/test_paths.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

using opennova::mission::EntityKind;
using opennova::mission::EntityTransform;

// One pool: `count` entities of `item_id` (an id the authored
// fixtures/def/items.def carries) laid out on a grid from `origin`, yaw
// stepping around the compass.
void add_pool(opennova::bms::File &doc, EntityKind kind, int item_id, int count, float origin_x, float origin_y,
              float spacing) {
	for (int i = 0; i < count; ++i) {
		EntityTransform t;
		t.x = origin_x + static_cast<float>(i % 12) * spacing;
		t.y = origin_y + static_cast<float>(i / 12) * spacing;
		t.z = 0.0f;
		t.yaw = (i * 45) % 360;
		t.pitch = 0;
		t.roll = 0;
		(void)opennova::mission::add_entity(doc, kind, item_id, t);
	}
}

// The mission: header strings naming the minted terrain and environment, four
// populated pools (one item id each, so the id -> pool map is a function),
// three waypoint paths, then every seeded per-entity default zeroed so the
// records carry the zero-valued fields the shipped missions do.
bool build(std::vector<uint8_t> &bytes, std::string &err) {
	namespace mission = opennova::mission;
	opennova::bms::File doc;
	mission::make_default(doc);
	std::string edit_error;
	if (!mission::set_header_string(doc, "mission_name", "Synthetic Dense", edit_error) ||
	    !mission::set_header_string(doc, "designer", "OpenNova", edit_error) ||
	    !mission::set_header_string(doc, "terrain", "Tmap", edit_error) ||
	    !mission::set_header_string(doc, "environment", "synth_full", edit_error) ||
	    !mission::set_header_int(doc, "minutes_per_day", 1440, edit_error)) {
		err = "header authoring failed";
		return false;
	}
	add_pool(doc, EntityKind::Item, 104001, 96, 100.0f, 100.0f, 8.0f);      // ammo crates
	add_pool(doc, EntityKind::Item, 105002, 48, 300.0f, 100.0f, 6.0f);      // barrels
	add_pool(doc, EntityKind::Building, 102001, 60, 100.0f, 400.0f, 24.0f); // guard towers
	add_pool(doc, EntityKind::Building, 105003, 36, 500.0f, 400.0f, 12.0f); // sandbag walls
	add_pool(doc, EntityKind::Marker, 100001, 24, 100.0f, 700.0f, 16.0f);   // marker alpha
	add_pool(doc, EntityKind::Organic, 105311, 40, 300.0f, 700.0f, 10.0f);  // generic soldiers

	// Three waypoint paths of 4, 5 and 6 markers (15 more markers), the last
	// one flagged as a non-looping blue-team patrol.
	for (size_t path = 0; path < 3; ++path) {
		opennova::mission::WaypointPath authored;
		for (int i = 0; i < 4 + static_cast<int>(path); ++i) {
			EntityTransform t;
			t.x = 600.0f + static_cast<float>(i) * 20.0f;
			t.y = 100.0f + static_cast<float>(path) * 40.0f;
			t.z = 0.0f;
			t.yaw = i * 30;
			if (!mission::add_waypoint_marker(doc, path, 100001, t, -1, edit_error)) {
				err = "add_waypoint_marker failed";
				return false;
			}
		}
		(void)mission::waypoint_path(doc, path, authored);
		const int flags = path == 2 ? static_cast<int>(opennova::bms::WaypointFlags::DoesNotLoop) |
		                                  static_cast<int>(opennova::bms::WaypointFlags::BlueTeam)
		                            : 0;
		if (!mission::set_waypoint_path(doc, path, authored.marker_indices, flags, edit_error)) {
			err = "set_waypoint_path failed";
			return false;
		}
	}

	// A weapon loadout (the mission-global kit tuples: name, requested clip
	// counts, the damage-class flag) so the loadout chunk carries records and
	// its empty-name terminator.
	{
		std::vector<opennova::mission::WeaponLoadoutEntry> kit;
		const char *const names[] = {"WPN_M4AUTO", "WPN_M9Beretta", "WPN_SATCHEL_CHARGE", "WPN_KNIFE"};
		for (int i = 0; i < 4; ++i) {
			opennova::mission::WeaponLoadoutEntry entry;
			entry.name = names[i];
			entry.ammo_primary = i == 0 ? "6" : "-1";
			entry.ammo_secondary = "-1";
			entry.flags = i == 2 ? "1" : "-1";
			kit.push_back(entry);
		}
		if (!mission::set_weapon_loadout(doc, kit, edit_error)) {
			err = "set_weapon_loadout failed";
			return false;
		}
	}

	// Three scripted events, each gated on a mission variable and resetting an
	// event: the trigger/action ranges the loader indexes per event.
	for (int i = 0; i < 3; ++i) {
		opennova::mission::MissionEventRecord event;
		event.flags = 0;
		event.delay = i * 5;
		const size_t added = mission::add_event(doc, event);
		for (int t = 0; t <= i; ++t) {
			opennova::mission::MissionTriggerRecord trigger;
			trigger.condition_flags = 0;
			trigger.main_type = static_cast<int>(opennova::bms::TriggerMainType::MissionVariable);
			trigger.sub_type =
			    static_cast<int>(opennova::bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
			trigger.param1 = i + 1;
			trigger.param2 = t * 10;
			if (!mission::insert_event_trigger(doc, added, static_cast<size_t>(t), trigger, edit_error)) {
				err = "insert_event_trigger failed";
				return false;
			}
		}
		opennova::mission::MissionActionRecord action;
		action.action_type = static_cast<int>(opennova::bms::ActionType::ResetEvent);
		action.param1 = (i + 1) % 3;
		if (!mission::insert_event_action(doc, added, 0, action, edit_error)) {
			err = "insert_event_action failed";
			return false;
		}
	}

	opennova::bms::File file = doc;
	std::vector<opennova::bms::Entity> *pools[4] = {&file.items, &file.buildings, &file.markers, &file.organics};
	uint8_t team = 0;
	for (std::vector<opennova::bms::Entity> *pool : pools) {
		for (opennova::bms::Entity &e : *pool) {
			e.spawns = 0;
			e.no_more_than = 0;
			e.no_less_than = 0;
			e.attention = 0;
			e.alert_state = 0;
			e.team = team;
			team = static_cast<uint8_t>((team + 1) % 3);
		}
	}
	file.header.num_items = static_cast<uint32_t>(file.items.size());
	file.header.num_buildings = static_cast<uint32_t>(file.buildings.size());
	file.header.num_markers = static_cast<uint32_t>(file.markers.size());
	file.header.num_people = static_cast<uint32_t>(file.organics.size());
	if (!opennova::bms::write(file, bytes, err)) return false;

	// The bytes parse back to the same document, re-write identically and keep
	// the pinned shape.
	opennova::bms::File back;
	if (!opennova::bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (!opennova::bms::equal(file, back)) {
		err = "parse(write(file)) is not value-equal to file";
		return false;
	}
	std::vector<uint8_t> again;
	if (!opennova::bms::write(back, again, err)) return false;
	if (again != bytes) {
		err = "write(parse(write(file))) differs from write(file)";
		return false;
	}
	const bool shape_ok = back.items.size() == 144 && back.buildings.size() == 96 && back.markers.size() == 39 &&
	                      back.organics.size() == 40 && back.get_terrain() == "Tmap" &&
	                      back.get_environment() == "synth_full" &&
	                      back.waypoint_records.size() == opennova::bms::kWaypointRecordCount;
	if (!shape_ok) {
		err = "the re-parsed mission lost a pinned field";
		return false;
	}
	return true;
}

using test_io::read_file;

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_dense.bms";

	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(build(bytes, err), ("synth_dense.bms: " + err).c_str())) return 1;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return 1;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: fixtures/bms/synth_dense.bms byte-reproducible (%zu bytes)\n", bytes.size());
	return 0;
}
