// Generator + guard for fixtures/bms/synth_dense.bms and fixtures/bms/synth_patrol.bms.
//
// synth_dense.bms: a synthetic mission
// authored through the bms_edit free functions (the same seam ONED writes with) and
// serialized by bms::write, dense the way the shipped missions are — every
// entity pool populated (items, buildings, markers, organics; a few hundred
// records on a grid, one item id per pool so the .mis pool classification
// stays 1:1), three waypoint paths, the full waypoint/group/layer record
// tables, and zero-valued optional fields on every record (the D-MIS-5 shape:
// an authoring default the parser seeds must not mutate a zero it read). The
// header names the minted terrain (Tmap) and environment (synth_full)
// fixtures. No retail mission is carried: the minted mission sets
// (fixtures/bms, fixtures/mission_set) stand for the shipped missions the
// mission tests once read.
//
// synth_patrol.bms: the small patrol mission the GUT world tests load
// (WorldFixture.load_patrol_mission; the C++ promote_test authors the same one):
// three waypoint markers (6005) on one looping waypoint record (slot 1) and a BLUE-flagged
// player route over them (slot 2, marker 0 a wide radius and a name id), two
// organics on teams 1 and 2 walking the loop, one building; no terrain or
// environment named.
//
// Default: rebuild each in memory and byte-compare the committed file. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/def/reserved_items.h>
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

	// Three waypoint paths of 4, 5 and 6 waypoint markers (15 more markers), the last one flagged as a
	// non-looping blue-team patrol: paths 1 to 3 (path 0 holds none: a marker carrying 0 is on no path).
	for (size_t path = 1; path <= 3; ++path) {
		opennova::mission::WaypointPath authored;
		for (int i = 0; i < 3 + static_cast<int>(path); ++i) {
			EntityTransform t;
			t.x = 600.0f + static_cast<float>(i) * 20.0f;
			t.y = 100.0f + static_cast<float>(path - 1) * 40.0f;
			t.z = 0.0f;
			t.yaw = i * 30;
			if (!mission::add_waypoint_marker(doc, path, mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT, t, -1,
			                                  edit_error)) {
				err = "add_waypoint_marker failed";
				return false;
			}
		}
		(void)mission::waypoint_path(doc, path, authored);
		const int flags = path == 3 ? static_cast<int>(opennova::bms::WaypointFlags::DoesNotLoop) |
		                                  static_cast<int>(opennova::bms::WaypointFlags::PlayerRoute)
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

	// Every record's authored members as this fixture has always held them (a new record's values are
	// the original editor's initializer's, bms_edit's new_entity, which this dense mission predates):
	// the ranges and accuracies below, every other optional member zero.
	opennova::bms::File file = doc;
	std::vector<opennova::bms::Entity> *pools[4] = {&file.items, &file.buildings, &file.markers, &file.organics};
	uint8_t team = 0;
	for (std::vector<opennova::bms::Entity> *pool : pools) {
		for (opennova::bms::Entity &e : *pool) {
			e.perception2 = 100;
			e.perfectionist2 = 100;
			e.min_engagement_distance = 20;
			e.max_engagement_distance = 200;
			e.w_accuracy1 = 50;
			e.w_accuracy2 = 50;
			e.max_attack_distance = 100;
			e.wp_distance = 0;
			e.crouch_timer = 0;
			e.shoot_timer = 0;
			e.wp_adv_trigger = 0;
			e.obliqueness = 0;
			e.map_symbol = 0;
			e.advancetimer = 0;
			std::memset(e.gen_string, 0, sizeof(e.gen_string));
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

// The patrol mission's records, raw (no editor defaults), as the promote tests author them.
bool build_patrol(std::vector<uint8_t> &bytes, std::string &err) {
	opennova::bms::File m;
	opennova::mission::make_default(m);
	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		// A waypoint marker (6005), as an authored route's are: the spawn writes the route
		// words (radius, name, trigger, chain-back) on that type alone (D-EVT-1)
		// [orig: Entity_SpawnFromBMSRecord - `cmp dword ptr [edi],1775h` @0x40F05A].
		e.type_id = opennova::def::DEF_TYPE_WAYPOINT;
		e.x = x;
		e.y = y;
		e.z = z;
		// No advance trigger (the record's 0xFFFF), so the route advances by proximity:
		// a trigger of 0 holds the marker (D-HUD-60).
		e.wp_adv_trigger = -1;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x;
		e.y = y;
		e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};
	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));
	// The waypoint table is positional: slot index == authored list id, slot 0 the
	// reserved no-route id. The looping patrol the organics author as wp_id 1 at slot 1;
	// the BLUE-flagged HUD route at slot 2, its marker 0 a wide radius and a name id.
	m.waypoint_records[1].flags = opennova::bms::WaypointFlags::None; // loops
	m.waypoint_records[1].marker_count = 3;
	m.waypoint_records[1].waypoint_numbers = {0, 1, 2};
	m.markers[0].wp_distance = 25;
	m.markers[0].ttool_index = 1;
	m.waypoint_records[2].flags = opennova::bms::WaypointFlags::PlayerRoute;
	m.waypoint_records[2].marker_count = 3;
	m.waypoint_records[2].waypoint_numbers = {0, 1, 2};
	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));
	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);
	// Authored SSNs (promotion copies record ids verbatim, like the original).
	m.organics[0].id = 1;
	m.organics[1].id = 2;
	m.buildings[0].id = 3;
	m.markers[0].id = 10;
	m.markers[1].id = 11;
	m.markers[2].id = 12;
	opennova::mission::sync_counts(m);
	if (!opennova::bms::write(m, bytes, err)) return false;
	opennova::bms::File back;
	if (!opennova::bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (!opennova::bms::equal(m, back)) {
		err = "parse(write(file)) is not value-equal to file";
		return false;
	}
	std::vector<uint8_t> again;
	if (!opennova::bms::write(back, again, err)) return false;
	if (again != bytes) {
		err = "write(parse(write(file))) differs from write(file)";
		return false;
	}
	const bool shape_ok = back.markers.size() == 3 && back.organics.size() == 2 && back.buildings.size() == 1 &&
	                      back.items.empty() && back.waypoint_records[1].marker_count == 3 &&
	                      back.waypoint_records[2].flags == opennova::bms::WaypointFlags::PlayerRoute &&
	                      back.organics[1].team == 2 && back.markers[0].wp_distance == 25;
	if (!shape_ok) {
		err = "the re-parsed patrol mission lost a pinned field";
		return false;
	}
	return true;
}

using test_io::read_file;

// One minted file: rebuilt and written (--write) or byte-compared with the committed one.
bool mint_or_check(const std::string &name, bool (*build_fn)(std::vector<uint8_t> &, std::string &),
                   bool write_mode) {
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/" + name;
	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(build_fn(bytes, err), (name + ": " + err).c_str())) return false;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return false;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return true;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return false;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return true;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return false;
	std::printf("OK: fixtures/bms/%s byte-reproducible (%zu bytes)\n", name.c_str(), bytes.size());
	return true;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	bool ok = mint_or_check("synth_dense.bms", build, write_mode);
	ok = mint_or_check("synth_patrol.bms", build_patrol, write_mode) && ok;
	return ok ? 0 : 1;
}
