// Generator + guard for fixtures/bms/synth_logic.bms and fixtures/bms/synth_logic.bin: a small
// mission with everything the mission document and its picture read (ADR 0046 S14), authored through
// the bms_edit free functions and written by bms::write, with its string table through rtxt::write.
// On the minted terrain (Tmap) under the minted environment (synth_full), every record inside mission
// x, y of -512..512: three items and two buildings whose items.def graphics are models of
// fixtures/threedi/synth (pump, armory), four path markers and a location marker (def type 2044,
// name_index 1), two organics (shed), the first named (name_index 1), in group 1 and on waypoint
// path 1; path 1 over the four markers; two area triggers (zone 20, and zone 30 the mission's boundary); two events that name
// one another (a SingleIsWithinArea trigger naming an SSN and zone 1 with a ResetEvent action; an
// Event trigger with a KillGroup action); a four-entry loadout, each with its four strings; the first
// win and lose conditions naming directive 1. The table holds the keys those records form
// (LOCATION001, STRNAME001, STRWINDIRECTIVE001, STRWINCOND001, STRLOSEDIRECTIVE001). No retail byte is
// carried.
//
// Also fixtures/bms/synth_areas.bms: the area triggers of a shipped training mission's shape
// (build_areas_mission), which the .mis idempotency test round-trips by zone id.
//
// Default: rebuild in memory and byte-compare the committed files. `--write` (re)writes them.
#include "common/test_paths.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/def/reserved_items.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <formats/rtxt/rtxt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "fixtures/minimal_3di_builder.h"

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
	const size_t index = mission::add_entity(doc, kind, item_id, t);
	// The members this mission has always held beside a new record's (bms_edit's new_entity, the original
	// editor's initializer since D-MIS-10): the map symbol the properties dialog leaves with none chosen,
	// and a person's engagement and attack distances.
	bms::Entity &made = (*mission::entities(doc, kind))[index];
	made.map_symbol = 255;
	if (kind == EntityKind::Organic) {
		made.max_engagement_distance = 100;
		made.max_attack_distance = 150;
	}
	return index;
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
	// Path 1's four waypoint markers (a path's stops are its 6005 markers', D-MIS-6).
	const int waypoint_item = mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT;
	place(doc, EntityKind::Marker, waypoint_item, -200.0f, -200.0f, 0);
	place(doc, EntityKind::Marker, waypoint_item, 200.0f, -200.0f, 0);
	place(doc, EntityKind::Marker, waypoint_item, 200.0f, 200.0f, 0);
	place(doc, EntityKind::Marker, waypoint_item, -200.0f, 200.0f, 0);
	// The location marker names itself by its record's name_index, so 1 keys the table's
	// LOCATION001 [orig: Entity_SpawnFromBMSRecord @0x40F196..0x40F1B8] (D-NET-511).
	const size_t location = place(doc, EntityKind::Marker, mission::kItemIdOffset + 2044, 0.0f, 0.0f, 0);
	doc.markers[location].name_index = 1;
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

// --- the mission set's models (fixtures/mission_set) --------------------------------------------

using synth3di::Box;
using synth3di::Vec3;

Box box(double x0, double y0, double z0, double x1, double y1, double z1) {
	return Box{{x0, y0, z0}, {x1, y1, z1}};
}

// door.3di: a block house with a hinged door, in the shape of the shipped door building
// (IBlock01: an LOD of type "door" whose part 0 is the house and part 1 the door, its one PANM
// row a rotation on control register 0, DOOR_00, from 0 to -5688, rotation type 2; a collision
// object per part, the door's offset to its hinge). Mission axes: the house a 6 x 6 room, 3 tall,
// its doorway in the +x wall, the door hinged at (3, -0.5).
synth3di::Model make_door_building() {
	synth3di::Model m;
	m.name = "door";
	const int lod = m.add_lod(0, "door");
	const int walls = m.add_material("FF_ST_OP", "door_walls.tga");
	const int slab = m.add_material("FF_ST_OP", "door_slab.tga");
	const Box floor = box(-3.0, -3.0, 0.0, 3.0, 3.0, 0.1);
	const Box back = box(-3.0, -3.0, 0.1, -2.8, 3.0, 3.0);
	const Box left = box(-2.8, 2.8, 0.1, 3.0, 3.0, 3.0);
	const Box right = box(-2.8, -3.0, 0.1, 3.0, -2.8, 3.0);
	const Box front_a = box(2.8, 0.5, 0.1, 3.0, 2.8, 3.0);
	const Box front_b = box(2.8, -2.8, 0.1, 3.0, -0.5, 3.0);
	const Box lintel = box(2.8, -0.5, 2.2, 3.0, 0.5, 3.0);
	const Box roof = box(-3.0, -3.0, 3.0, 3.0, 3.0, 3.2);
	const Box door = box(2.85, -0.5, 0.1, 2.95, 0.5, 2.2);
	const int house = m.add_part(lod, 0, Vec3{});
	for (const Box &b : {floor, back, left, right, front_a, front_b, lintel, roof}) m.add_box(lod, house, walls, b);
	const Vec3 hinge{2.9, -0.5, 0.1};
	const int leaf = m.add_part(lod, 0, hinge);
	m.add_box(lod, leaf, slab, door);
	m.add_control_register("DOOR_00");
	m.add_panm(lod, house, 0);
	opennova::threedi::ThreediPartAnimation &swing =
			m.add_panm(lod, leaf, 0, opennova::threedi::threedi_panm_pack_flags(0, 2, 0, 0));
	swing.rotation_x = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, -5688);
	m.add_user_point("Ground", Vec3{}, Vec3{0.0, 0.0, 1.0}, 0, synth3di::kUserPointGameplay);
	const int shell = m.add_cobj(0);
	for (const Box &b : {floor, back, left, right, front_a, front_b, lintel, roof}) m.add_face_box(shell, b);
	const int door_cobj = m.add_cobj(0, hinge);
	m.add_face_box(door_cobj, door);
	return m;
}

constexpr Vec3 kForward{1.0, 0.0, 0.0};
constexpr Vec3 kUp{0.0, 0.0, 1.0};

// mtruck.3di: a transport truck in the shape of the shipped DTruck1 (LOD type "veh0"): the hull
// on part 0 and six wheels on parts 1..6 at the shipped hubs (front 1.92, mid -3.355, rear -5.074,
// each side 1.25 off the centre line, 0.812 up, 0.8 in radius), each a PANM row on the shipped
// registers (the front pair steering on VEHICLE_STEERING, every wheel spinning on VEHICLE_WHEELS
// and travelling on its own VEHICLE_TIRE0n); the shipped user points (the controller ctrlx10, the
// seven bed and cab passengers sitex00a..g, the exhaust FX01); and a collision object per part,
// the wheels' bottoms 0.01 over the origin, the floor of the shipped CMDL bounds (D-VEH-1: the
// settled origin lands 0.01 under the ground).
synth3di::Model make_truck() {
	synth3di::Model m;
	m.name = "mtruck";
	const int lod = m.add_lod(96, "veh0");
	const int paint = m.add_material("FF_ST_OP", "mtruck.tga");
	const int rubber = m.add_material("FF_ST_OP", "mtruck_tire.tga");
	const Box cab = box(0.4, -1.2, 1.0, 3.0, 1.2, 3.4);
	const Box bed = box(-6.5, -1.3, 1.0, 0.4, 1.3, 2.4);
	const int hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, hull, paint, cab);
	m.add_box(lod, hull, paint, bed);
	m.add_control_register("VEHICLE_STEERING");
	m.add_control_register("VEHICLE_WHEELS");
	for (int t = 0; t < 6; ++t) {
		char name[24];
		std::snprintf(name, sizeof(name), "VEHICLE_TIRE%02d", t);
		m.add_control_register(name);
	}
	m.add_panm(lod, hull, 0);
	const double hubs_x[3] = {1.92, -3.355, -5.074};
	constexpr double kHubZ = 0.812, kRadius = 0.802;
	const auto wheel_box = [&](double x, double y) {
		return box(x - kRadius, y - 0.25, kHubZ - kRadius, x + kRadius, y + 0.25, kHubZ + kRadius);
	};
	std::vector<std::pair<Vec3, Box>> wheels;
	for (int axle = 0; axle < 3; ++axle) {
		for (int side = 0; side < 2; ++side) {
			const double y = side == 0 ? 1.25 : -1.25;
			const Vec3 hub{hubs_x[axle], y, kHubZ};
			const int part = m.add_part(lod, 0, hub);
			m.add_box(lod, part, rubber, wheel_box(hubs_x[axle], y));
			const bool front = axle == 0;
			opennova::threedi::ThreediPartAnimation &row =
					m.add_panm(lod, part, 0, front ? 0x02010200u : 0x02000200u);
			if (front)
				row.rotation_x = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16338);
			row.rotation_y = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16338);
			row.translation = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER,
					static_cast<uint8_t>(2 + axle * 2 + side), 0, 0, 256);
			wheels.push_back({hub, wheel_box(hubs_x[axle], y)});
		}
	}
	m.add_user_point("ctrlx10", Vec3{-0.146, 0.715, 2.111}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("FX01", Vec3{-6.275, -0.026, 0.023}, Vec3{-1.0, 0.0, 0.0}, 0, synth3di::kUserPointEffect);
	const Vec3 seats[7] = {{-2.039, 1.068, 2.539}, {-5.124, -1.054, 2.539}, {-3.569, 1.068, 2.539},
		{-3.595, -1.054, 2.539}, {-5.138, 1.068, 2.539}, {-2.026, -1.054, 2.539}, {-0.059, -0.693, 2.086}};
	const char *const seat_names[7] = {"sitex00a", "sitex00b", "sitex00c", "sitex00d", "sitex00e", "sitex00f",
		"sitex00g"};
	for (int s = 0; s < 7; ++s) {
		const Vec3 facing = s == 6 ? kForward : (seats[s].y > 0.0 ? Vec3{0.0, -1.0, 0.0} : Vec3{0.0, 1.0, 0.0});
		m.add_user_point(seat_names[s], seats[s], facing, 0, synth3di::kUserPointGameplay);
	}
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, cab);
	m.add_face_box(c_hull, bed);
	m.add_volume(c_hull, 1, 0, cab);
	m.add_volume(c_hull, 1, 0, bed);
	for (const auto &wheel : wheels) {
		const int c = m.add_cobj(0, wheel.first);
		m.add_face_box(c, wheel.second);
	}
	return m;
}

// mbike.3di: a motorcycle in the shape of the shipped Dcycle2: the body on part 0 and the two
// wheels on parts 1 (front, hub 0.82 forward) and 2 (rear, 0.82 back), each 0.385 up, spinning on
// VEHICLE_WHEELS and travelling on VEHICLE_TIRE00 / 01; the rider's controller ctrlx24 and the
// pillion sitex00; one collision object, its faces the body's and its one type-1 volume
// (-1.23..1.23, -0.25..0.25, 0..1.13), the collision floor at the origin.
synth3di::Model make_bike() {
	synth3di::Model m;
	m.name = "mbike";
	const int lod = m.add_lod(96);
	const int paint = m.add_material("FF_ST_OP", "mbike.tga");
	const Box frame = box(-1.0, -0.2, 0.4, 1.0, 0.2, 1.1);
	const int body = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, body, paint, frame);
	m.add_control_register("VEHICLE_WHEELS");
	m.add_control_register("VEHICLE_TIRE00");
	m.add_control_register("VEHICLE_TIRE01");
	m.add_panm(lod, body, 0);
	const double hubs[2] = {0.8195, -0.8195};
	for (int w = 0; w < 2; ++w) {
		const int part = m.add_part(lod, 0, Vec3{hubs[w], 0.0, 0.385});
		m.add_box(lod, part, paint, box(hubs[w] - 0.385, -0.08, 0.0, hubs[w] + 0.385, 0.08, 0.77));
		opennova::threedi::ThreediPartAnimation &row = m.add_panm(lod, part, 0, 0x02000200u);
		row.rotation_y = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, -16384);
		row.translation = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER,
				static_cast<uint8_t>(1 + w), 0, 0, w == 0 ? 384 : 204);
	}
	m.add_user_point("ctrlx24", Vec3{-0.445, -0.004, 1.042}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("sitex00", Vec3{-0.911, -0.002, 1.206}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("FX00", Vec3{-1.168, -0.003, 0.167}, Vec3{-0.737, 0.0, 0.676}, 0, synth3di::kUserPointEffect);
	m.add_user_point("FX01", Vec3{-1.168, -0.003, 0.167}, Vec3{-0.737, 0.0, 0.676}, 0, synth3di::kUserPointEffect);
	const int c = m.add_cobj(0);
	m.add_face_box(c, frame);
	m.add_face_box(c, box(0.4345, -0.08, 0.0, 1.2045, 0.08, 0.77));
	m.add_face_box(c, box(-1.2045, -0.08, 0.0, -0.4345, 0.08, 0.77));
	m.add_volume(c, 1, 0, box(-1.23, -0.25, 0.0, 1.23, 0.25, 1.13));
	return m;
}

// --- the mission set's missions --------------------------------------------------------------------

// The ground of the Tmap terrain (tests/fixtures/minimal_terrain_gen.cpp) west of its east rises,
// where every mission set mission stands: 36 units at the world origin, falling a sixteenth of a
// unit per unit of mission y.
double tmap_ground(double x, double y) {
	(void)x;
	return 36.0 - y / 16.0;
}

size_t place_on_ground(bms::File &doc, EntityKind kind, int item_id, double x, double y, int yaw, int32_t id,
		double lift = 0.0) {
	const size_t index = place(doc, kind, item_id, static_cast<float>(x), static_cast<float>(y), yaw);
	bms::Entity &made = (*mission::entities(doc, kind))[index];
	made.z = bms::to_fixed_16_16(tmap_ground(x, y) + lift);
	made.id = id;
	return index;
}

bool begin_mission(bms::File &doc, const char *name, std::string &err) {
	mission::make_default(doc);
	std::string edit_error;
	if (!mission::set_header_string(doc, "mission_name", name, edit_error) ||
	    !mission::set_header_string(doc, "designer", "OpenNova", edit_error) ||
	    !mission::set_header_string(doc, "terrain", "Tmap", edit_error)) {
		err = "header authoring failed: " + edit_error;
		return false;
	}
	return true;
}

bool finish_mission(bms::File &doc, std::vector<uint8_t> &bytes, std::string &err) {
	mission::sync_counts(doc);
	if (!bms::write(doc, bytes, err)) return false;
	bms::File back;
	if (!bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (!bms::equal(doc, back)) {
		err = "parse(write(file)) is not value-equal to file";
		return false;
	}
	return true;
}

constexpr int kStartMarker = 106001;   // "start, player"
constexpr int kTransportTruck = 101294; // the shipped "Drivable Transport Truck" row's id
constexpr int kArmoryTruck = 101420;    // and its "with armory" twin's
constexpr int kFriendlySoldier = 102072; // the shipped "Friendly Soldier #3 with M16" row's id

// A record's two name cells (iai_name, ai_textfile: the AI profile its brain reads).
void name_record(bms::Entity &e, const char *iai_name, const char *ai_textfile) {
	std::snprintf(e.name1, sizeof(e.name1), "%s", iai_name);
	std::snprintf(e.name2, sizeof(e.name2), "%s", ai_textfile);
}

// A crewman who command-mounts the vehicle of SSN `vehicle` at spawn (waypoint_id 125 rides the
// SSN its wp_number names, as the shipped training missions crew their trucks).
size_t place_crewman(bms::File &doc, double x, double y, int yaw, int32_t id, int32_t vehicle, uint8_t team) {
	const size_t index = place_on_ground(doc, EntityKind::Organic, kFriendlySoldier, x, y, yaw, id);
	bms::Entity &e = doc.organics[index];
	e.team = team;
	e.waypoint_id = 125;
	e.wp_number = vehicle;
	e.max_engagement_distance = 50;
	e.max_attack_distance = 50;
	return index;
}

// truck_rest.bms: the shipped 00TRa's parked transport trucks around the single-player start:
// SSN 11 (team 1, group 3) a truck right of the start and SSN 1714 (team 1, group 4) the armory
// truck, each crewed by a friendly soldier command-mounted at spawn (SSNs 1 and 1715), and SSN 6
// (team 2) an empty truck further out, each placed on the ground with the trucks' AI profile.
bool build_truck_rest(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Truck Rest", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -100.0, 0.0, 0, 1654);
	const size_t right = place_on_ground(doc, EntityKind::Item, kTransportTruck, -88.0, -12.0, -150, 11);
	doc.items[right].team = 1;
	doc.items[right].group_id = 3;
	name_record(doc.items[right], "mtruck", "d_5ton");
	const size_t armory = place_on_ground(doc, EntityKind::Item, kArmoryTruck, -86.0, 14.0, -90, 1714);
	doc.items[armory].team = 1;
	doc.items[armory].group_id = 4;
	name_record(doc.items[armory], "mtruck", "d_5ton");
	const size_t far = place_on_ground(doc, EntityKind::Item, kTransportTruck, -40.0, -60.0, -6, 6);
	doc.items[far].team = 2;
	name_record(doc.items[far], "mtruck", "d_5ton");
	place_crewman(doc, -86.0, -14.0, -60, 1, 11, 1);
	place_crewman(doc, -84.0, 12.0, 0, 1715, 1714, 1);
	return finish_mission(doc, bytes, err);
}

// One trigger or action row as a shipped mission's .mis lists it: the main (or action) type, the
// sub type and the four parameters.
struct Row {
	int type, sub, p1, p2, p3, p4;
};

// An event of `delay` (in 64-tick quanta) whose triggers and actions are `triggers` and `actions`.
bool add_event_rows(bms::File &doc, int delay, std::initializer_list<Row> triggers, std::initializer_list<Row> actions,
		std::string &err, int flags = 0) {
	mission::MissionEventRecord record;
	record.delay = delay;
	record.flags = flags;
	const size_t event = mission::add_event(doc, record);
	std::string edit_error;
	size_t at = 0;
	for (const Row &row : triggers) {
		mission::MissionTriggerRecord trigger;
		trigger.main_type = row.type;
		trigger.sub_type = row.sub;
		trigger.param1 = row.p1;
		trigger.param2 = row.p2;
		trigger.param3 = row.p3;
		trigger.param4 = row.p4;
		if (!mission::insert_event_trigger(doc, event, at++, trigger, edit_error)) {
			err = "trigger authoring failed: " + edit_error;
			return false;
		}
	}
	at = 0;
	for (const Row &row : actions) {
		mission::MissionActionRecord action;
		action.action_type = row.type;
		action.action_sub_type = row.sub;
		action.param1 = row.p1;
		action.param2 = row.p2;
		action.param3 = row.p3;
		action.param4 = row.p4;
		if (!mission::insert_event_action(doc, event, at++, action, edit_error)) {
			err = "action authoring failed: " + edit_error;
			return false;
		}
	}
	return true;
}

// Waypoint path `path` over new waypoint markers at `stops`, flagged `flags`.
bool add_path(bms::File &doc, size_t path, std::initializer_list<std::pair<double, double>> stops, int flags,
		std::string &err) {
	std::vector<int> markers;
	for (const auto &stop : stops) {
		const size_t index = place_on_ground(doc, EntityKind::Marker,
				mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT, stop.first, stop.second, 0, 0);
		doc.markers[index].id = 0;
		markers.push_back(static_cast<int>(index));
	}
	std::string edit_error;
	if (!mission::set_waypoint_path(doc, path, markers, flags, edit_error)) {
		err = "set_waypoint_path failed: " + edit_error;
		return false;
	}
	return true;
}

constexpr int kMotorcycle = 100074;       // the shipped "Drivable Motorcycle" row's id
constexpr int kFriendlyMp5 = 102073;      // the shipped "Friendly Soldier #4 with MP5" row's id

// motorcycle.bms: the shipped 06TR's front-left instructor ride. Motorcycle SSN 80 (group 4, the
// buggy AI profile) with its instructor SSN 1715 command-mounted on its controller at spawn; the
// player's start beside it; path 6 (DoesNotLoop) ahead of it; and the event the player's boarding
// fires (a Player trigger, sub 39, on SSN 80, two quanta's delay): RedirectGroupTo group 4 onto
// path 6 and ChangeGroupAI group 4 (92, sub 30), as 06TR's event 13 lists them.
bool build_motorcycle(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Motorcycle", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -204.0, 6.0, 0, 1654);
	const size_t bike = place_on_ground(doc, EntityKind::Item, kMotorcycle, -200.0, 0.0, 90, 80);
	doc.items[bike].group_id = 4;
	name_record(doc.items[bike], "mbike", "d_buggy");
	const size_t instructor = place_on_ground(doc, EntityKind::Organic, kFriendlyMp5, -199.4, 2.0, -150, 1715);
	doc.organics[instructor].team = 1;
	doc.organics[instructor].waypoint_id = 125;
	doc.organics[instructor].wp_number = 80;
	doc.organics[instructor].max_engagement_distance = 200;
	doc.organics[instructor].max_attack_distance = 200;
	if (!add_path(doc, 6, {{-170.0, 0.0}, {-120.0, 0.0}, {-70.0, 0.0}, {-20.0, 0.0}}, 1, err)) return false;
	if (!add_event_rows(doc, 2, {{7, 39, 80, -1, -1, -1}},
			{{1, 0, 4, 6, 0, -1}, {3, 30, 4, 92, -1, -1}}, err))
		return false;
	return finish_mission(doc, bytes, err);
}

// fixtures/bms/synth_areas.bms: a mission whose area triggers take the shape of a shipped
// training mission's (03TR: thirteen zones over ids 1..15 with gaps, most constrain-Z, flag 2),
// with the mission boundary (flag 1) beside them and a record in each of the four pools, so its
// .mis export carries every area by zone id with its flags and bounds (D-MIS-14).
bool build_areas_mission(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	mission::make_default(doc);
	std::string edit_error;
	if (!mission::set_header_string(doc, "mission_name", "Synthetic Areas", edit_error) ||
	    !mission::set_header_string(doc, "designer", "OpenNova", edit_error) ||
	    !mission::set_header_string(doc, "terrain", "Tmap", edit_error)) {
		err = "header authoring failed: " + edit_error;
		return false;
	}
	bms::set_game_mode(doc.header.attrib_flags, uint32_t(bms::AttribFlags::Coop));
	place(doc, EntityKind::Item, 106100, -40.0f, -40.0f, 0);
	place(doc, EntityKind::Building, 106101, 80.0f, 60.0f, 90);
	place(doc, EntityKind::Marker, mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT, 10.0f, 10.0f, 0);
	place(doc, EntityKind::Organic, 106102, -20.0f, 30.0f, 180);
	const auto fixed = [](double units) { return bms::to_fixed_16_16(units); };
	// Zone ids with the shipped gaps (6 and 13 unused); the constrain-Z ones carry a Z band.
	const int32_t ids[] = {1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 14, 15};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
		const double x = -400.0 + 60.0 * static_cast<double>(i);
		const bool constrain_z = ids[i] >= 4 && ids[i] != 14;
		doc.area_triggers.push_back(bms::AreaTrigger{ids[i], fixed(x), fixed(x + 40.0), fixed(-30.0 - i),
		                                             fixed(30.0 + i), constrain_z ? fixed(10.0) : 0,
		                                             constrain_z ? fixed(40.0 + i) : 0,
		                                             constrain_z ? bms::AreaTrigger::kFlagConstrainZ : 0u});
	}
	doc.area_triggers.push_back(bms::AreaTrigger{30, fixed(-512), fixed(512), fixed(-512), fixed(512), 0, 0,
	                                             bms::AreaTrigger::kFlagMissionArea});
	mission::sync_counts(doc);
	if (!bms::write(doc, bytes, err)) return false;
	bms::File back;
	if (!bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	if (!bms::equal(doc, back) || back.area_triggers.size() != 14) {
		err = "the re-parsed mission lost a record";
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

	std::vector<uint8_t> mission_bytes, table_bytes, areas_bytes;
	std::string err;
	if (!expect(build_mission(mission_bytes, err), ("synth_logic.bms: " + err).c_str())) return 1;
	if (!expect(build_table(table_bytes, err), ("synth_logic.bin: " + err).c_str())) return 1;
	if (!expect(build_areas_mission(areas_bytes, err), ("synth_areas.bms: " + err).c_str())) return 1;
	if (emit(root + "fixtures/bms/synth_logic.bms", mission_bytes, write_mode) != 0) return 1;
	if (emit(root + "fixtures/bms/synth_logic.bin", table_bytes, write_mode) != 0) return 1;
	if (emit(root + "fixtures/bms/synth_areas.bms", areas_bytes, write_mode) != 0) return 1;

	// The mission set (fixtures/mission_set): its models.
	const std::string set = root + "fixtures/mission_set/";
	if (write_mode) std::filesystem::create_directories(set);
	struct ModelRecipe {
		const char *file;
		synth3di::Model (*make)();
	};
	const ModelRecipe models[] = {
		{"door.3di", make_door_building},
		{"mtruck.3di", make_truck},
		{"mbike.3di", make_bike},
	};
	for (const ModelRecipe &recipe : models) {
		std::vector<uint8_t> bytes;
		if (!expect(synth3di::mint(recipe.make(), bytes), (std::string(recipe.file) + ": the writer refused it").c_str()))
			return 1;
		opennova::threedi::Threedi3di3 back{};
		const bool reads = opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &back) == 0;
		opennova::threedi::threedi_3di3_free(&back);
		if (!expect(reads, (std::string(recipe.file) + ": does not read back").c_str())) return 1;
		if (emit(set + recipe.file, bytes, write_mode) != 0) return 1;
	}
	// Its missions.
	struct MissionRecipe {
		const char *file;
		bool (*build)(std::vector<uint8_t> &, std::string &);
	};
	const MissionRecipe missions[] = {
		{"truck_rest.bms", build_truck_rest},
		{"motorcycle.bms", build_motorcycle},
	};
	for (const MissionRecipe &recipe : missions) {
		std::vector<uint8_t> bytes;
		if (!expect(recipe.build(bytes, err), (std::string(recipe.file) + ": " + err).c_str())) return 1;
		if (emit(set + recipe.file, bytes, write_mode) != 0) return 1;
	}
	return 0;
}
