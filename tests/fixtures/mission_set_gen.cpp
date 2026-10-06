// Generator + guard for fixtures/bms/synth_logic.bms and fixtures/bms/synth_logic.bin: a small
// mission with everything the mission document and its picture read (ADR 0046 S14), authored through
// the bms_edit free functions and written by bms::write, with its string table through rtxt::write.
// On the minted terrain (Tmap) under the minted environment (synth_full), every record inside mission
// x, y of -512..512: three items and two buildings whose items.def graphics are models of
// fixtures/threedi/synth (pump, armory), four path markers and a location marker (def type 2044), two
// organics (shed), the first named (name_index 1), in group 1 and on waypoint path 1; path 1 over the
// four markers; two area triggers (zone 20, and zone 30 the mission's boundary); two events that name
// one another (a SingleIsWithinArea trigger naming an SSN and zone 1 with a ResetEvent action; an
// Event trigger with a KillGroup action); a four-entry loadout, each with its four strings; the first
// win and lose conditions naming directive 1. The table holds the keys those records form
// (LOCATION001, STRNAME001, STRWINDIRECTIVE001, STRWINCOND001, STRLOSEDIRECTIVE001). No retail byte is
// carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write` (re)writes them.
#include "common/test_paths.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <formats/rtxt/rtxt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

namespace bms = opennova::bms;
namespace mission = opennova::mission;
namespace rtxt = opennova::rtxt;
using mission::EntityKind;
using mission::EntityTransform;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

size_t place(bms::File &doc, EntityKind kind, int item_id, float x, float y, int yaw) {
	EntityTransform t;
	t.x = x;
	t.y = y;
	t.z = 0.0f;
	t.yaw = yaw;
	return mission::add_entity(doc, kind, item_id, t);
}

bool build_mission(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	mission::make_default(doc);
	std::string edit_error;
	if (!mission::set_header_string(doc, "mission_name", "Synthetic Logic", edit_error) ||
	    !mission::set_header_string(doc, "designer", "OpenNova", edit_error) ||
	    !mission::set_header_string(doc, "terrain", "Tmap", edit_error) ||
	    !mission::set_header_string(doc, "environment", "synth_full", edit_error) ||
	    !mission::set_header_int(doc, "minutes_per_day", 1440, edit_error)) {
		err = "header authoring failed: " + edit_error;
		return false;
	}
	// A co-op mission: one of no game mode bit is single player, whose player needs a start marker
	// (mission.no_start), which the minted items.def has no row of.
	bms::set_game_mode(doc.header.attrib_flags, uint32_t(bms::AttribFlags::Coop));
	// The first win and lose conditions name directive 1; the others name none (255).
	std::memset(doc.header.win_conditions, 0xFF, sizeof(doc.header.win_conditions));
	std::memset(doc.header.lose_conditions, 0xFF, sizeof(doc.header.lose_conditions));
	doc.header.win_conditions[0] = 1;
	doc.header.lose_conditions[0] = 1;

	// Items (pump, an object) and buildings (armory), by ids the authored fixtures/def/items.def holds.
	place(doc, EntityKind::Item, 106100, -100.0f, -100.0f, 0);
	place(doc, EntityKind::Item, 106100, 0.0f, 50.0f, 90);
	place(doc, EntityKind::Item, 106100, 120.0f, -40.0f, 180);
	place(doc, EntityKind::Building, 106101, -300.0f, 200.0f, 0);
	place(doc, EntityKind::Building, 106101, 250.0f, 300.0f, 270);
	// Four path markers (marker alpha) round a square, then a location marker (def type 2044).
	place(doc, EntityKind::Marker, 100001, -200.0f, -200.0f, 0);
	place(doc, EntityKind::Marker, 100001, 200.0f, -200.0f, 0);
	place(doc, EntityKind::Marker, 100001, 200.0f, 200.0f, 0);
	place(doc, EntityKind::Marker, 100001, -200.0f, 200.0f, 0);
	place(doc, EntityKind::Marker, mission::kItemIdOffset + 2044, 0.0f, 0.0f, 0);
	// Two organics (shed, a person): the first named, in group 1 and walking path 1 from its first
	// stop; the second in group 2.
	const size_t walker = place(doc, EntityKind::Organic, 106102, 30.0f, -200.0f, 45);
	const size_t guard = place(doc, EntityKind::Organic, 106102, -60.0f, 220.0f, 225);
	doc.organics[walker].name_index = 1;
	doc.organics[walker].group_id = 1;
	doc.organics[walker].waypoint_id = 1;
	doc.organics[walker].wp_number = 0;
	doc.organics[guard].group_id = 2;
	const int walker_ssn = doc.organics[walker].id;

	if (!mission::set_waypoint_path(doc, 1, {0, 1, 2, 3}, 0, edit_error)) {
		err = "set_waypoint_path failed: " + edit_error;
		return false;
	}

	// Zone 20 round the middle; zone 30 the mission's boundary (ids no SSN shares, so a name finds
	// one record of the mission's scope).
	const auto fixed = [](double units) { return bms::to_fixed_16_16(units); };
	doc.area_triggers.push_back(bms::AreaTrigger{20, fixed(-150), fixed(150), fixed(-150), fixed(150), 0, 0, 0});
	doc.area_triggers.push_back(bms::AreaTrigger{30, fixed(-512), fixed(512), fixed(-512), fixed(512), 0, 0,
	                                             bms::AreaTrigger::kFlagMissionArea});
	mission::sync_counts(doc);

	// Event 1: the walker inside zone 20 resets event 2. Event 2: event 1 having fired kills group 2.
	{
		const size_t event = mission::add_event(doc, mission::MissionEventRecord());
		mission::MissionTriggerRecord trigger;
		trigger.main_type = static_cast<int>(bms::TriggerMainType::Single);
		trigger.sub_type = 10; // SingleIsWithinArea
		trigger.param1 = walker_ssn;
		trigger.param2 = 20;
		mission::MissionActionRecord action;
		action.action_type = static_cast<int>(bms::ActionType::ResetEvent);
		action.param1 = 1;
		if (!mission::insert_event_trigger(doc, event, 0, trigger, edit_error) ||
		    !mission::insert_event_action(doc, event, 0, action, edit_error)) {
			err = "event 1 authoring failed: " + edit_error;
			return false;
		}
	}
	{
		mission::MissionEventRecord record;
		record.delay = 5;
		const size_t event = mission::add_event(doc, record);
		mission::MissionTriggerRecord trigger;
		trigger.main_type = static_cast<int>(bms::TriggerMainType::Event);
		trigger.param1 = 0;
		mission::MissionActionRecord action;
		action.action_type = static_cast<int>(bms::ActionType::KillGroup);
		action.param1 = 2;
		if (!mission::insert_event_trigger(doc, event, 0, trigger, edit_error) ||
		    !mission::insert_event_action(doc, event, 0, action, edit_error)) {
			err = "event 2 authoring failed: " + edit_error;
			return false;
		}
	}

	// Each parameter a record's type does not read holds -1, as every shipped record holds it
	// (mission::kUnreadParam, the mission_logic ctest's retail leg).
	for (bms::Trigger &trigger : doc.triggers) {
		int32_t *params[4] = {&trigger.param1, &trigger.param2, &trigger.param3, &trigger.param4};
		for (int slot = 0; slot < 4; ++slot)
			if (mission::trigger_param_kind(trigger, slot) == mission::ParamKind::Unused) *params[slot] = mission::kUnreadParam;
	}
	for (bms::Action &action : doc.actions) {
		int32_t *params[4] = {&action.param1, &action.param2, &action.param3, &action.param4};
		for (int slot = 0; slot < 4; ++slot)
			if (mission::action_param_kind(action, slot) == mission::ParamKind::Unused) *params[slot] = mission::kUnreadParam;
	}

	// The loadout: each record with its four strings.
	{
		std::vector<mission::WeaponLoadoutEntry> kit;
		const char *const names[] = {"WPN_M4AUTO", "WPN_M9Beretta", "WPN_SATCHEL_CHARGE", "WPN_KNIFE"};
		for (int i = 0; i < 4; ++i) {
			mission::WeaponLoadoutEntry entry;
			entry.name = names[i];
			entry.ammo_primary = i == 0 ? "6" : "-1";
			entry.ammo_secondary = "-1";
			entry.flags = i == 2 ? "1" : "-1";
			kit.push_back(entry);
		}
		if (!mission::set_weapon_loadout(doc, kit, edit_error)) {
			err = "set_weapon_loadout failed: " + edit_error;
			return false;
		}
	}

	if (!bms::write(doc, bytes, err)) return false;
	// The bytes parse back to the same document, write again identically and keep the pinned shape.
	bms::File back;
	if (!bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (!bms::equal(doc, back)) {
		err = "parse(write(file)) is not value-equal to file";
		return false;
	}
	std::vector<uint8_t> again;
	if (!bms::write(back, again, err)) return false;
	if (again != bytes) {
		err = "write(parse(write(file))) differs from write(file)";
		return false;
	}
	const bool shape_ok = back.items.size() == 3 && back.buildings.size() == 2 && back.markers.size() == 5 &&
	                      back.organics.size() == 2 && back.area_triggers.size() == 2 && back.events.size() == 2 &&
	                      back.triggers.size() == 2 && back.actions.size() == 2 && back.loadout.entries.size() == 4 &&
	                      back.waypoint_records[1].waypoint_numbers.size() == 4 && back.get_terrain() == "Tmap";
	if (!shape_ok) {
		err = "the re-parsed mission lost a pinned record";
		return false;
	}
	return true;
}

bool build_table(std::vector<uint8_t> &bytes, std::string &err) {
	rtxt::File table;
	const auto section = [&table](const char *name, std::initializer_list<std::pair<const char *, const char *>> rows) {
		const uint32_t index = static_cast<uint32_t>(table.sections.size());
		table.sections.push_back({name, static_cast<uint32_t>(rows.size())});
		for (const auto &row : rows) table.entries.push_back({row.first, row.second, {}, index});
	};
	section("Info", {{"TITLE", "Synthetic Logic"}, {"BRIEFING", "Walk the square and reach the pump house."}});
	section("Locations", {{"LOCATION001", "Pump House"}});
	section("PeopleNames", {{"STRNAME001", "Sgt. Walker"}});
	section("WinConditions", {{"STRWINDIRECTIVE001", "Reach the pump house"}, {"STRWINCOND001", "Pump house reached"}});
	section("LoseConditions", {{"STRLOSEDIRECTIVE001", "The walker must survive"}});
	if (!rtxt::write(table, bytes, err)) return false;
	rtxt::File back;
	if (!rtxt::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (back.entries.size() != table.entries.size() || back.sections.size() != table.sections.size()) {
		err = "the re-parsed table lost a row";
		return false;
	}
	return true;
}

int emit(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return 1;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: %s byte-reproducible (%zu bytes)\n", path.c_str(), bytes.size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string root = std::string(test_paths_repo_root(__FILE__)) + "/";

	std::vector<uint8_t> mission_bytes, table_bytes;
	std::string err;
	if (!expect(build_mission(mission_bytes, err), ("synth_logic.bms: " + err).c_str())) return 1;
	if (!expect(build_table(table_bytes, err), ("synth_logic.bin: " + err).c_str())) return 1;
	if (emit(root + "fixtures/bms/synth_logic.bms", mission_bytes, write_mode) != 0) return 1;
	return emit(root + "fixtures/bms/synth_logic.bin", table_bytes, write_mode);
}
