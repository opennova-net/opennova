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
#include <formats/bad/bad_build.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/anim/anim_slot_names.h>

#include <array>
#include <cmath>
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
	// The chassis, the cab ahead of the controller and the bed's low walls: every seat point
	// stands in the open over them, as the shipped truck's do (the seat scan's line of sight).
	const Box chassis = box(-6.5, -1.2, 0.9, 3.0, 1.2, 1.3);
	const Box cab = box(1.0, -1.2, 1.3, 3.0, 1.2, 3.4);
	const Box wall_l = box(-6.5, 1.2, 1.3, 0.9, 1.3, 2.0);
	const Box wall_r = box(-6.5, -1.3, 1.3, 0.9, -1.2, 2.0);
	const Box tail = box(-6.5, -1.2, 1.3, -6.4, 1.2, 2.0);
	const int hull = m.add_part(lod, 0, Vec3{});
	for (const Box &b : {chassis, cab, wall_l, wall_r, tail}) m.add_box(lod, hull, paint, b);
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
	for (const Box &b : {chassis, cab, wall_l, wall_r, tail}) m.add_face_box(c_hull, b);
	m.add_volume(c_hull, 1, 0, chassis);
	m.add_volume(c_hull, 1, 0, cab);
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

// mrock.3di: a single rock formation in the shape of the shipped RckS05 (00TRg's bms 650): one
// part, one LOD, a closed mesh of 52 triangles over 28 vertices (a thirteen-sided body: the top
// apex, two rings and the bottom apex, so 13 + 26 + 13 triangles) whose collision faces are the
// render triangles themselves (poly type 12, as the shipped faces), centred on its origin (2.5
// across, 1.5 up and down), with one type-1 volume around it and its `ground` point at the origin.
synth3di::Model make_rock() {
	synth3di::Model m;
	m.name = "mrock";
	const int lod = m.add_lod(0);
	const int stone = m.add_material("FF_MT_OP", "mrock.tga");
	const int part = m.add_part(lod, 0, Vec3{});
	static constexpr int kSides = 13;
	constexpr double kPi = 3.14159265358979323846;
	std::vector<Vec3> verts;
	verts.push_back(Vec3{0.0, 0.0, 1.5});
	const double ring_z[2] = {0.6, -0.6}, ring_r[2] = {2.3, 2.5};
	for (int r = 0; r < 2; ++r)
		for (int s = 0; s < kSides; ++s) {
			const double a = 2.0 * kPi * (s + 0.5 * r) / kSides;
			verts.push_back(Vec3{ring_r[r] * std::cos(a), ring_r[r] * std::sin(a), ring_z[r]});
		}
	verts.push_back(Vec3{0.0, 0.0, -1.5});
	const auto ring = [](int r, int s) { return 1 + r * kSides + ((s % kSides) + kSides) % kSides; };
	// Each triangle counter-clockwise about its outward normal (mission axes).
	std::vector<std::array<int, 3>> tris;
	for (int s = 0; s < kSides; ++s) tris.push_back({0, ring(0, s), ring(0, s + 1)});
	for (int s = 0; s < kSides; ++s) {
		tris.push_back({ring(0, s), ring(1, s), ring(0, s + 1)});
		tris.push_back({ring(0, s + 1), ring(1, s), ring(1, s + 1)});
	}
	const int bottom = 1 + 2 * kSides;
	for (int s = 0; s < kSides; ++s) tris.push_back({bottom, ring(1, s + 1), ring(1, s)});
	// The render strip: the same 28 vertices (model axes), smooth normals, each triangle wound
	// counter-clockwise about its outward normal in model space.
	opennova::threedi::ThreediBuildStrip strip;
	strip.material = stone;
	for (const Vec3 &p : verts) {
		const Vec3 q = opennova::threedi::threedi_build_to_model(p);
		const double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		const Vec3 n = opennova::threedi::threedi_build_to_model(Vec3{p.x / len, p.y / len, p.z / len});
		opennova::threedi::ThreediVertex v{};
		v.position[0] = static_cast<float>(q.x);
		v.position[1] = static_cast<float>(q.y);
		v.position[2] = static_cast<float>(q.z);
		v.normal[0] = static_cast<float>(n.x);
		v.normal[1] = static_cast<float>(n.y);
		v.normal[2] = static_cast<float>(n.z);
		v.uv0[0] = static_cast<float>(0.5 + p.x / 5.0);
		v.uv0[1] = static_cast<float>(0.5 + p.y / 5.0);
		v.uv1[0] = v.uv0[0];
		v.uv1[1] = v.uv0[1];
		strip.vertices.push_back(v);
	}
	for (const auto &t : tris) {
		const Vec3 a = opennova::threedi::threedi_build_to_model(verts[t[0]]);
		const Vec3 b = opennova::threedi::threedi_build_to_model(verts[t[1]]);
		const Vec3 c = opennova::threedi::threedi_build_to_model(verts[t[2]]);
		const Vec3 centre{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0};
		const double ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
		const double fx = c.x - a.x, fy = c.y - a.y, fz = c.z - a.z;
		const double cx = ey * fz - ez * fy, cy = ez * fx - ex * fz, cz = ex * fy - ey * fx;
		const bool ccw = cx * centre.x + cy * centre.y + cz * centre.z > 0.0;
		strip.indices.push_back(static_cast<uint16_t>(t[0]));
		strip.indices.push_back(static_cast<uint16_t>(ccw ? t[1] : t[2]));
		strip.indices.push_back(static_cast<uint16_t>(ccw ? t[2] : t[1]));
	}
	m.lods[lod].parts[part].strips.push_back(strip);
	m.add_panm(lod, part, 0);
	m.add_user_point("ground", Vec3{}, kUp, 0, synth3di::kUserPointGameplay);
	const int c = m.add_cobj(0);
	std::vector<uint16_t> cv;
	for (const Vec3 &p : verts) cv.push_back(m.add_collision_vertex(c, p));
	for (const auto &t : tris) m.add_face(c, cv[t[0]], cv[t[1]], cv[t[2]], 12);
	m.add_volume(c, 1, 0, box(-2.6, -2.6, -1.6, 2.6, 2.6, 1.6));
	return m;
}

// matv.3di: an ATV in the shape of the shipped Datv1: the hull on part 0 and six wheels on parts
// 1..6 at the shipped hubs (0.729, -0.324 and -1.361 along, 0.74 each side, 0.068 up, 0.42 in
// radius, so 0.35 under the origin), the front pair steering, every wheel on VEHICLE_WHEELS and its
// own VEHICLE_TIRE0n; the shipped rider's controller ctrlx14 and the two passengers sitex08 and
// sitex12; a collision object per part, the hull's type-1 volumes the shipped skirts (-0.35..0.5)
// and decks (0.5..1.19).
synth3di::Model make_atv() {
	synth3di::Model m;
	m.name = "matv";
	const int lod = m.add_lod(160);
	const int paint = m.add_material("FF_ST_OP", "matv.tga");
	const Box deck = box(-1.76, -0.67, 0.2, 1.14, 0.74, 0.6);
	const Box seat = box(-1.06, -0.5, 0.6, 0.33, 0.5, 1.0);
	const Box nose = box(0.10, -0.6, 0.6, 1.62, 0.6, 1.0);
	const int hull = m.add_part(lod, 0, Vec3{});
	for (const Box &b : {deck, seat, nose}) m.add_box(lod, hull, paint, b);
	m.add_control_register("VEHICLE_STEERING");
	m.add_control_register("VEHICLE_WHEELS");
	for (int t = 0; t < 6; ++t) {
		char name[24];
		std::snprintf(name, sizeof(name), "VEHICLE_TIRE%02d", t);
		m.add_control_register(name);
	}
	m.add_panm(lod, hull, 0);
	const double hubs_x[3] = {0.729, -0.324, -1.361};
	constexpr double kHubZ = 0.068, kRadius = 0.418;
	std::vector<std::pair<Vec3, Box>> wheels;
	for (int axle = 0; axle < 3; ++axle) {
		for (int side = 0; side < 2; ++side) {
			const double y = side == 0 ? 0.74 : -0.74;
			const Vec3 hub{hubs_x[axle], y, kHubZ};
			const Box b = box(hubs_x[axle] - kRadius, y - 0.15, kHubZ - kRadius, hubs_x[axle] + kRadius, y + 0.15,
					kHubZ + kRadius);
			const int part = m.add_part(lod, 0, hub);
			m.add_box(lod, part, paint, b);
			const bool front = axle == 0;
			opennova::threedi::ThreediPartAnimation &row = m.add_panm(lod, part, 0, front ? 0x02010200u : 0x02000200u);
			if (front)
				row.rotation_x = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16338);
			row.rotation_y = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16338);
			row.translation = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER,
					static_cast<uint8_t>(2 + axle * 2 + side), 0, 0, 256);
			wheels.push_back({hub, b});
		}
	}
	m.add_user_point("ctrlx14", Vec3{-0.280, 0.360, 0.610}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("FX00", Vec3{-2.512, 0.027, 0.263}, Vec3{-0.996, 0.0, 0.087}, 0, synth3di::kUserPointEffect);
	m.add_user_point("ground", Vec3{}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("sitex08", Vec3{-1.004, 0.008, 0.570}, Vec3{-1.0, 0.0, 0.0}, 0, synth3di::kUserPointGameplay);
	m.add_user_point("sitex12", Vec3{-0.279, -0.390, 0.668}, kForward, 0, synth3di::kUserPointGameplay);
	const int c_hull = m.add_cobj(0);
	for (const Box &b : {deck, seat, nose}) m.add_face_box(c_hull, b);
	m.add_volume(c_hull, 1, 0, box(-1.76, 0.74, -0.35, 1.14, 1.30, 0.50));
	m.add_volume(c_hull, 1, 0, box(-1.76, -1.23, -0.35, 1.14, -0.67, 0.50));
	m.add_volume(c_hull, 1, 0, box(0.10, -0.87, 0.50, 1.62, 0.94, 1.09));
	m.add_volume(c_hull, 1, 0, box(-1.06, -0.95, 0.51, 0.33, 1.01, 1.19));
	for (const auto &wheel : wheels) {
		const int c = m.add_cobj(0, wheel.first);
		m.add_face_box(c, wheel.second);
	}
	return m;
}

// mladder.3di: an 8 m wooden ladder in the shape of the shipped WLad03: one part, two rails and
// sixteen rungs as collision faces (poly type 13, as the shipped faces), one type-1 volume around
// them (x -0.04..0.24, y -0.5..0.52, up to 8.01) and the climb volume, type 4 (CL), the flat
// face 0.37 in front of the rungs from the ground to 8.47, as the shipped ladder's.
synth3di::Model make_ladder() {
	synth3di::Model m;
	m.name = "mladder";
	const int lod = m.add_lod(128);
	const int wood = m.add_material("FF_ST_OP", "mladder.tga");
	const int part = m.add_part(lod, 0, Vec3{});
	const Box rails[2] = {box(0.0, -0.42, 0.0, 0.06, -0.36, 8.01), box(0.0, 0.43, 0.0, 0.06, 0.49, 8.01)};
	for (const Box &b : rails) m.add_box(lod, part, wood, b);
	std::vector<Box> rungs;
	for (int r = 0; r < 16; ++r) {
		const double z = 0.3 + 0.5 * r;
		rungs.push_back(box(-0.03, -0.36, z, 0.03, 0.43, z + 0.05));
	}
	for (const Box &b : rungs) m.add_box(lod, part, wood, b);
	m.add_panm(lod, part, 0);
	const int c = m.add_cobj(0);
	for (const Box &b : rails) m.add_face_box(c, b, 13);
	for (const Box &b : rungs) m.add_face_box(c, b, 13);
	m.add_volume(c, 1, 0, box(-0.04, -0.50, 0.0, 0.24, 0.52, 8.01));
	m.add_volume(c, 4, 0, box(0.37, -0.50, 0.0, 0.37, 0.52, 8.47));
	return m;
}

// mbuggy.3di: a dune buggy in the shape of the shipped Dbuggy1: the hull on part 0, four wheels
// on parts 1..4 (front hubs 1.851 forward, rear 1.686 back, 0.94 each side, 0.25 under the
// origin), the front pair steering; the shipped driver's controller ctrlx02, the passenger
// sitex06 and the gun anchor ewep01 (the items.def `addeweap` child rides it); a collision object
// per part, the hull's type-1 volumes its lower skirts and its frame.
synth3di::Model make_buggy() {
	synth3di::Model m;
	m.name = "mbuggy";
	const int lod = m.add_lod(96);
	const int paint = m.add_material("FF_ST_OP", "mbuggy.tga");
	const Box tub = box(-1.85, -0.9, -0.34, 2.27, 0.9, 0.41);
	const Box cage = box(-1.54, -0.75, 0.41, -0.04, 0.75, 1.13);
	const int hull = m.add_part(lod, 0, Vec3{});
	for (const Box &b : {tub, cage}) m.add_box(lod, hull, paint, b);
	m.add_control_register("VEHICLE_STEERING");
	m.add_control_register("VEHICLE_WHEELS");
	for (int t = 0; t < 4; ++t) {
		char name[24];
		std::snprintf(name, sizeof(name), "VEHICLE_TIRE%02d", t);
		m.add_control_register(name);
	}
	m.add_panm(lod, hull, 0);
	const double hubs_x[2] = {1.851, -1.686};
	constexpr double kHubZ = -0.25, kRadius = 0.49;
	std::vector<std::pair<Vec3, Box>> wheels;
	for (int axle = 0; axle < 2; ++axle) {
		for (int side = 0; side < 2; ++side) {
			const double y = side == 0 ? 0.94 : -0.94;
			const Vec3 hub{hubs_x[axle], y, kHubZ};
			const Box b = box(hubs_x[axle] - kRadius, y - 0.2, kHubZ - kRadius, hubs_x[axle] + kRadius, y + 0.2,
					kHubZ + kRadius);
			const int part = m.add_part(lod, 0, hub);
			m.add_box(lod, part, paint, b);
			const bool front = axle == 0;
			opennova::threedi::ThreediPartAnimation &row = m.add_panm(lod, part, 0, front ? 0x02010200u : 0x02000200u);
			if (front)
				row.rotation_x = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16338);
			row.rotation_y = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16338);
			row.translation = synth3di::track(opennova::threedi::THREEDI_PANM_STYLE_CONTROL_REGISTER,
					static_cast<uint8_t>(2 + axle * 2 + side), 0, 0, 256);
			wheels.push_back({hub, b});
		}
	}
	m.add_user_point("ctrlx02", Vec3{-0.288, 0.413, -0.150}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("ewep01", Vec3{-0.218, 0.007, 0.955}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("FX00", Vec3{-2.658, 0.304, 1.442}, Vec3{-0.766, 0.0, 0.643}, 0, synth3di::kUserPointEffect);
	m.add_user_point("sitex06", Vec3{-0.288, -0.340, -0.150}, kForward, 0, synth3di::kUserPointGameplay);
	m.add_user_point("FX01", Vec3{-2.520, -0.009, -0.418}, Vec3{-1.0, 0.0, 0.0}, 0, synth3di::kUserPointEffect);
	const int c_hull = m.add_cobj(0);
	for (const Box &b : {tub, cage}) m.add_face_box(c_hull, b);
	m.add_volume(c_hull, 1, 0, box(-2.23, 1.04, -0.75, -1.16, 1.47, 0.31));
	m.add_volume(c_hull, 1, 0, box(-2.23, -1.46, -0.75, -1.16, -1.03, 0.31));
	m.add_volume(c_hull, 1, 0, box(1.35, -1.34, -0.74, 2.35, -0.95, 0.22));
	m.add_volume(c_hull, 1, 0, box(1.35, 0.95, -0.74, 2.35, 1.35, 0.22));
	m.add_volume(c_hull, 1, 0, box(0.07, -1.01, -0.34, 2.27, 1.01, 0.41));
	m.add_volume(c_hull, 1, 0, box(-1.85, -1.25, -0.34, -0.60, 1.25, 0.91));
	for (const auto &wheel : wheels) {
		const int c = m.add_cobj(0, wheel.first);
		m.add_face_box(c, wheel.second);
	}
	return m;
}

// mlndmine.3di: an unmarked minefield section in the shape of the shipped Lndmine1: one flat part
// over 16 x 16 units, its `ground` point, then its fourteen mine points in the shipped order and
// places (nine `small`, five `large`, half a unit up); one collision object, the slab.
synth3di::Model make_minefield() {
	synth3di::Model m;
	m.name = "mlndmine";
	const int lod = m.add_lod(0);
	const int soil = m.add_material("FF_ST_OP", "mlndmine.tga");
	const Box slab = box(-8.0, -8.0, 0.0, 8.0, 8.0, 1.0);
	const int part = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, part, soil, slab);
	m.add_panm(lod, part, 0);
	m.add_user_point("ground", Vec3{}, kUp, 0, synth3di::kUserPointEffect);
	struct Mine {
		const char *name;
		double x, y;
	};
	const Mine mines[14] = {{"small", -0.656, -1.695}, {"small", -3.5, -3.5}, {"large", 3.5, -3.5}, {"large", -1.0, 3.5},
		{"large", 6.0, 0.0}, {"small", -5.549, 0.899}, {"small", 2.305, 0.344}, {"small", 6.110, -5.725},
		{"small", -1.428, -6.845}, {"small", -4.557, 7.257}, {"small", -7.216, 4.525}, {"small", -6.775, -7.110},
		{"large", 2.5, 7.0}, {"large", 7.0, 5.0}};
	for (const Mine &mine : mines)
		m.add_user_point(mine.name, Vec3{mine.x, mine.y, 0.5}, kUp, 0, synth3di::kUserPointEffect);
	const int c = m.add_cobj(0);
	m.add_face_box(c, slab);
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
// The markers take the ids from `first_id` on.
bool add_path(bms::File &doc, size_t path, std::initializer_list<std::pair<double, double>> stops, int flags,
		std::string &err, int32_t first_id) {
	std::vector<int> markers;
	int32_t id = first_id;
	for (const auto &stop : stops) {
		const size_t index = place_on_ground(doc, EntityKind::Marker,
				mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT, stop.first, stop.second, 0, id++);
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
	if (!add_path(doc, 6, {{-170.0, 0.0}, {-120.0, 0.0}, {-70.0, 0.0}, {-20.0, 0.0}}, 1, err, 5000)) return false;
	if (!add_event_rows(doc, 2, {{7, 39, 80, -1, -1, -1}},
			{{1, 0, 4, 6, 0, -1}, {3, 30, 4, 92, -1, -1}}, err))
		return false;
	return finish_mission(doc, bytes, err);
}

constexpr int kRockFormation = 101471; // the shipped "Singe rock formation5" row's id

// rock_collision.bms: the shipped 00TRg's single-player start (bms 1197, facing mission +y) with
// the rock formation (bms 650) seven units in front of it, both on the ground.
bool build_rock_collision(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Rock Collision", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -100.0, 0.0, 0, 1197);
	place_on_ground(doc, EntityKind::Item, kRockFormation, -100.0, 7.0, 0, 650);
	return finish_mission(doc, bytes, err);
}

constexpr int kIndoSoldier = 101799; // the shipped "Indonesian Soldier #2 with AK47" row's id

// infantry.bms: CP01's opening ground in miniature: the single-player start (facing mission +y)
// with a squad of the shipped AK47 soldier (team 2, group 33) spread some 60 units apart, 70 to 140
// units in front of it,
// each authored with CP01's own squad's record values (alert state 2, accuracies 39 and 70, the
// engagement distances 15 and 65, attack distance 40, advance timer 4), all on the ground.
bool build_infantry(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Infantry", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -150.0, -60.0, 0, 1);
	const double spots[6][2] = {{-210.0, 0.0}, {-150.0, 10.0}, {-90.0, 0.0}, {-180.0, 60.0}, {-120.0, 70.0},
		{-60.0, 60.0}};
	for (int i = 0; i < 6; ++i) {
		const size_t index = place_on_ground(doc, EntityKind::Organic, kIndoSoldier, spots[i][0], spots[i][1],
				180, 2803 + i);
		bms::Entity &e = doc.organics[index];
		e.team = 2;
		e.group_id = 33;
		e.alert_state = 2;
		e.w_accuracy1 = 39;
		e.w_accuracy2 = 70;
		e.min_engagement_distance = 15;
		e.max_engagement_distance = 65;
		e.max_attack_distance = 40;
		e.advancetimer = 4;
	}
	return finish_mission(doc, bytes, err);
}

constexpr int kLadder8m = 101622; // the shipped "Modular wooden Ladder 8m tall" row's id

// ladder.bms: 00TRa's ladder-climbing tutorial ground in miniature: the single-player start with
// an 8 m ladder standing twenty units in front of it, far enough west that the ground within 400
// units of the start stays under the 60 units the test's sweep drops the player from (00TRa's own
// ground lies lower still).
bool build_ladder(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Ladder", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -420.0, 100.0, 0, 1654);
	place_on_ground(doc, EntityKind::Building, kLadder8m, -420.0, 120.0, 0, 1134);
	return finish_mission(doc, bytes, err);
}

constexpr int kAtv = 101301; // the shipped "Drivable ATV" row's id

// vehicle_ride.bms: 00TRa's vehicle gate: truck SSN 11 (team 1, group 3) crewed at spawn by the
// instructor SSN 1 (command-mounted, waypoint_id 125), the free ATV SSN 1766 beside the motor pool,
// path 2 (DoesNotLoop) leading off from the truck, and 00TRa's first three events: 0 shows the
// waypoints (ShowWaypoints 0), 1 is empty, and 2 (a quantum's delay) fires when the player attaches
// to SSN 11 (a Player trigger, sub 38) and kicks the ride off: RedirectGroupTo group 3 onto path 2
// and ChangeGroupAI group 3 (40, sub 30), as 00TRa lists them.
bool build_vehicle_ride(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Vehicle Ride", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -120.0, -30.0, 0, 1654);
	const size_t truck = place_on_ground(doc, EntityKind::Item, kTransportTruck, -110.0, -10.0, 0, 11);
	doc.items[truck].team = 1;
	doc.items[truck].group_id = 3;
	name_record(doc.items[truck], "mtruck", "d_5ton");
	const size_t atv = place_on_ground(doc, EntityKind::Item, kAtv, -130.0, -12.0, 11, 1766);
	name_record(doc.items[atv], "matv", "d_buggy");
	place_crewman(doc, -108.0, -12.0, -60, 1, 11, 1);
	if (!add_path(doc, 2, {{-110.0, 10.0}, {-110.0, 40.0}, {-100.0, 70.0}, {-80.0, 100.0}, {-60.0, 130.0}}, 1, err,
			1655))
		return false;
	if (!add_event_rows(doc, 0, {}, {{40, 0, 0, -1, -1, -1}}, err)) return false;
	if (!add_event_rows(doc, 0, {}, {}, err)) return false;
	if (!add_event_rows(doc, 1, {{7, 38, 11, -1, -1, -1}}, {{1, 0, 3, 2, 0, -1}, {3, 30, 3, 40, -1, -1}}, err))
		return false;
	return finish_mission(doc, bytes, err);
}

constexpr int kDuneBuggy = 101291;     // the shipped "Drivable Dune Buggy" row's id
constexpr int kFriendlyM4 = 102070;    // the shipped "Friendly Soldier #1 with M4" row's id
constexpr int kLaneDividers[3] = {102289, 102290, 102291}; // the shipped concrete lane dividers 6..8

// land_vehicles.bms: 01TR's opening ground in miniature: the single-player start; the dune buggy
// (its items.def `addeweap` gun child rides its ewep01 point) beside it; the player's team's three
// riflemen by their SSNs (2, 70, 71: team 1, the shipped records' engagement distances 0 and 300
// and attack distance 300) beside the start; and five of the course's team-2 concrete lane
// dividers (hp 0) sixty to eighty units in front of them, well inside that range.
bool build_land_vehicles(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!begin_mission(doc, "Land Vehicles", err)) return false;
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -200.0, -80.0, 70, 1890);
	const size_t buggy = place_on_ground(doc, EntityKind::Item, kDuneBuggy, -188.0, -84.0, 70, 300);
	name_record(doc.items[buggy], "mbuggy", "d_buggy");
	const int32_t riflemen[3] = {2, 70, 71};
	for (int i = 0; i < 3; ++i) {
		const size_t index = place_on_ground(doc, EntityKind::Organic, kFriendlyM4, -196.0 + 6.0 * i, -74.0, 170,
				riflemen[i]);
		bms::Entity &e = doc.organics[index];
		e.team = 1;
		e.min_engagement_distance = 0;
		e.max_engagement_distance = 300;
		e.max_attack_distance = 300;
		e.advancetimer = 5;
	}
	// The dividers lie sixty-odd units in front of the riflemen, inside their 70-degree radar arc
	// (01TR's sit the same way off its three).
	const double spots[5][2] = {{-210.0, -138.0}, {-196.0, -142.0}, {-184.0, -136.0}, {-170.0, -146.0},
			{-205.0, -150.0}};
	for (int i = 0; i < 5; ++i) {
		const size_t index = place_on_ground(doc, EntityKind::Building, kLaneDividers[i % 3], spots[i][0], spots[i][1],
				-90, 1780 + i);
		doc.buildings[index].team = 2;
	}
	return finish_mission(doc, bytes, err);
}

constexpr int kMinefieldSection = 101896; // the shipped "All Un-marked Land Mine Section" row's id

// minefield.bms / minefield_coop.bms: the two shipped minefield missions in miniature (00TRd, a
// single-player training mission, and CP09, an objective Co-op mission): three and two unmarked
// sections laid out in front of the start.
bool build_minefield_mission(std::vector<uint8_t> &bytes, std::string &err, bool coop, int sections) {
	bms::File doc;
	if (!begin_mission(doc, coop ? "Minefield Co-op" : "Minefield", err)) return false;
	if (coop) bms::set_game_mode(doc.header.attrib_flags, uint32_t(bms::AttribFlags::Coop));
	place_on_ground(doc, EntityKind::Marker, kStartMarker, -150.0, -120.0, 0, 1);
	for (int i = 0; i < sections; ++i)
		place_on_ground(doc, EntityKind::Building, kMinefieldSection, -180.0 + 30.0 * i, -60.0, 0, 3000 + i);
	return finish_mission(doc, bytes, err);
}
bool build_minefield(std::vector<uint8_t> &bytes, std::string &err) {
	return build_minefield_mission(bytes, err, false, 3);
}
bool build_minefield_coop(std::vector<uint8_t> &bytes, std::string &err) {
	return build_minefield_mission(bytes, err, true, 2);
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

// --- the mission set's clips (fixtures/mission_set/ms_*.bad, mfp_*.bad and their tables) ---------

// One 30 fps clip of `frames` intervals on the one root bone: the hips `bottom` over the ground,
// falling to `bottom_end` across the clip (the head `top` keeping its height over them), a forward
// step of `step` metres a frame and a vertical one of `rise`, and the trigger word `trigger` on every
// `every`-th frame from 0 (none when `every` is 0).
opennova::bad::BadBuildClip one_bone_clip(const char *name, uint32_t frames, bool loop, double bottom,
		double bottom_end, double top, double step, int32_t trigger, uint32_t every, double rise = 0.0) {
	opennova::bad::BadBuildClip clip;
	clip.name = name;
	clip.fps = 30;
	clip.flags = loop ? 0x1u : 0u;
	clip.frame_count = frames;
	opennova::bad::BadBuildBone bone;
	bone.name = "BN01";
	bone.keys.assign(frames + 1, opennova::bad::BadBuildQuat{});
	clip.bones.push_back(bone);
	for (uint32_t f = 0; f <= frames; ++f) {
		const double t = static_cast<double>(f) / frames;
		opennova::bad::BadBuildEvent event;
		event.velocity.x = step; // mission axes: x forward, z up (the vertical lane)
		event.velocity.z = rise;
		event.bottom = bottom + (bottom_end - bottom) * t;
		event.top = top + (bottom_end - bottom) * t;
		event.trigger = (every != 0 && f % every == 0 && f < frames) ? trigger : 0;
		clip.events.push_back(event);
	}
	return clip;
}

// The soldier's attack: the shipped E_SHOT8a's shape, a looping 92-frame hold whose fire word
// (0x8, the shot the combat pass fires on) falls on its frames 9, 17, 30, 44, 55, 63, 69 and 82.
opennova::bad::BadBuildClip soldier_attack_clip() {
	opennova::bad::BadBuildClip clip = one_bone_clip("ms_attack", 92, true, 0.9128, 0.9128, 1.6468, 0.0, 0, 0);
	for (const uint32_t f : {9u, 17u, 30u, 44u, 55u, 63u, 69u, 82u}) clip.events[f].trigger = 0x8;
	return clip;
}

// The soldier's clips, in the shape of the shipped soldiers' (Dt1rst, I_idle1, I_walkf and the
// death matrix's clips): the reset; a standing idle; the walk (152 frames at 0.0619 m a frame, the
// footstep word 0x2 every 14th frame, as I_walkf authors its eleven); a run; a crouch and a prone
// idle and walk; an attack and a guard hold; a reload; a death that lowers the hips to the ground;
// and the ladder's hold, its climb up (0.8 a loop on the clip's vertical lane) and down.
std::vector<opennova::bad::BadBuildClip> soldier_clips() {
	return {
		one_bone_clip("ms_rst", 2, true, 1.0185, 1.0185, 1.7756, 0.0, 0, 0),
		one_bone_clip("ms_idle", 8, true, 0.95, 0.95, 1.69, 0.0, 0, 0),
		one_bone_clip("ms_walk", 152, true, 0.98, 0.98, 1.72, 0.0619, 0x2, 14),
		one_bone_clip("ms_run", 40, true, 0.95, 0.95, 1.68, 0.15, 0x2, 10),
		one_bone_clip("ms_crouch", 70, true, 0.6, 0.6, 1.2, 0.0, 0, 0),
		one_bone_clip("ms_crwalk", 60, true, 0.6, 0.6, 1.2, 0.03, 0x2, 15),
		one_bone_clip("ms_prone", 60, true, 0.164, 0.164, 0.244, 0.0, 0, 0),
		one_bone_clip("ms_prwalk", 60, true, 0.164, 0.164, 0.244, 0.02, 0, 0),
		soldier_attack_clip(),
		one_bone_clip("ms_guard", 70, true, 1.0, 1.0, 1.76, 0.0, 0, 0),
		one_bone_clip("ms_reload", 70, true, 1.0, 1.0, 1.76, 0.0, 0, 0),
		one_bone_clip("ms_death", 40, false, 1.0, 0.2, 1.76, 0.0, 0, 0),
		one_bone_clip("ms_climb", 30, true, 1.0, 1.0, 1.76, 0.0, 0, 0),
		one_bone_clip("ms_climbup", 30, true, 1.0, 1.0, 1.76, 0.0, 0, 0, 0.8 / 30.0),
		one_bone_clip("ms_climbdn", 30, true, 1.0, 1.0, 1.76, 0.0, 0, 0, -0.8 / 30.0),
		one_bone_clip("ms_jump", 20, false, 1.0, 1.0, 1.76, 0.1, 0, 0),
		one_bone_clip("ms_roll", 30, false, 0.164, 0.164, 0.244, 0.05, 0, 0),
	};
}

// The clip a body slot plays: a death slot the death; a walk or a wash the walk, a run or a jog
// the run; the crouch and prone families theirs; reloads, the guard family and the attacks theirs;
// every other body slot the idle. The reset row is the reset.
const char *soldier_slot_clip(const std::string &slot) {
	const auto starts = [&slot](const char *prefix) { return slot.rfind(prefix, 0) == 0; };
	if (slot == "reset") return "ms_rst";
	if (starts("death")) return "ms_death";
	if (slot == "climb_up") return "ms_climbup";
	if (slot == "climb_down") return "ms_climbdn";
	if (starts("climb")) return "ms_climb";
	if (starts("walk_crouch") || starts("run2crouch") || starts("runl2crouch") || starts("runr2crouch"))
		return "ms_crwalk";
	if (starts("roll_") || starts("run2prone")) return "ms_roll";
	if (starts("jump_start")) return "ms_jump";
	if (starts("walk_prone")) return "ms_prwalk";
	if (slot == "idle_crouch") return "ms_crouch";
	if (slot == "idle_prone") return "ms_prone";
	if (starts("walk") || starts("wash") || starts("wounded_walk")) return "ms_walk";
	if (starts("run_attack")) return "ms_attack";
	if (starts("run") || starts("jog") || starts("wounded_run") || starts("cover_run")) return "ms_run";
	if (starts("reload")) return "ms_reload";
	if (starts("guard")) return "ms_guard";
	if (starts("attack") || starts("post_attack") || starts("pre_attack") || starts("cover_attack"))
		return "ms_attack";
	return "ms_idle";
}

// The first-person M4's clips (the shipped M4_1st table's slots): the reset, the idle, the fire
// (6 frames), the recoil (4), the reload (90), the empty and the three switches (10 each).
std::vector<opennova::bad::BadBuildClip> fp_m4_clips() {
	return {
		one_bone_clip("mfp_rst", 2, true, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_idle", 30, true, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_fire", 6, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_recoil", 4, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_reload", 90, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_empty", 10, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_swto", 10, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_swfrom", 10, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
		one_bone_clip("mfp_swrank", 10, false, 0.0, 0.0, 0.0, 0.0, 0, 0),
	};
}

// A clip table written through the canonical .adm writer, its rows `rows` over `clips`.
bool mint_table(const std::vector<std::pair<std::string, std::string>> &rows,
		const std::vector<opennova::bad::BadBuildClip> &clips, std::vector<uint8_t> &bytes, std::string &err) {
	opennova::bad::BadBuildSet set;
	for (const auto &row : rows) set.rows.push_back(opennova::bad::BadBuildRow{row.first, {row.second}});
	set.clips = clips;
	std::string text;
	if (!opennova::bad::bad_build_mint_table(set, opennova::bad::bad_retail_limits(), text, &err)) return false;
	bytes.assign(text.begin(), text.end());
	return true;
}

// Whether `slot` is one a shipped table authors: the soldier's (Eindo01.adm: the idles, the burns,
// the emplaced and seat holds, the guard, the walk, jog and run forward, the climb holds, the jump
// loop, the attacks and the post attack, the reload, the cover poses, the run away, the swims, the
// washes and every death) or the player's (US01.adm: those but the guard, the jog and run forward,
// the swim attack, the cover poses, the wash run and the run away, and the emotes, the drag poses,
// the second burn, the held weapons' poses, the reloads, the binoculars, the mortar idle, the idles
// to idle_5, the crouch and prone idles and every walk, the jump start, the climb top, every swim,
// the rolls, the run into prone and runs 2 and 3). A table authoring every slot plays transitions
// the shipped ones leave to the reset ring (a looping jump start, run into crouch, stop), which
// stalls a body's think.
bool soldier_authors(const std::string &slot) {
	static const char *const kSlots[] = {"reset", "idle", "idle_2", "idle_3", "burn", "burn_2", "emplaced",
		"emplaced_2", "emplaced_3", "emplaced_4", "emplaced_5", "emplaced_6", "emplaced_7", "emplaced_8", "guard",
		"walk_forward", "jog_forward", "run_forward", "climb_idle", "climb_up", "climb_down", "jump_loop", "attack",
		"attack_2", "attack_3", "post_attack", "reload", "cover_idle", "cover_attack", "run_away", "swim_idle",
		"swim_forward", "swim_attack", "wash_idle", "wash_walk", "wash_run"};
	for (const char *s : kSlots)
		if (slot == s) return true;
	return slot.rfind("sit", 0) == 0 || slot.rfind("death_", 0) == 0;
}
bool player_authors(const std::string &slot) {
	static const char *const kSlots[] = {"reset", "run2prone", "dragger_idle", "dragger_walk", "draggee", "burn_2",
		"grenade_attack", "knife_attack", "P90", "P90_scoped", "MP7", "MP7_scoped", "parachute", "designator",
		"designator_scoped", "reload", "reload2", "binoculars", "stinger", "javelin", "javelin_scoped", "grenade",
		"knife", "pistol", "idle_mortar", "idle", "idle_2", "idle_3", "idle_4", "idle_5", "idle_crouch", "idle_prone",
		"wash_idle", "wash_walk", "jump_start", "jump_loop", "climb_idle", "climb_up", "climb_down", "climb_top",
		"swim_idle", "swim_forward", "swim_left", "swim_right", "swim_back", "roll_left", "roll_right", "run_2",
		"run_3"};
	for (const char *s : kSlots)
		if (slot == s) return true;
	if (slot.rfind("emote_", 0) == 0) return slot.size() == 7 && slot[6] >= '1' && slot[6] <= '7';
	return slot.rfind("walk_", 0) == 0 || slot.rfind("sit", 0) == 0 || slot.rfind("emplaced", 0) == 0 ||
	       slot.rfind("death_", 0) == 0;
}

// A table over the body slots (0..239) `authors` names, each row its clip.
bool build_body_table(bool (*authors)(const std::string &), std::vector<uint8_t> &bytes, std::string &err) {
	std::vector<std::pair<std::string, std::string>> rows;
	for (int s = 0; s < 240; ++s) {
		const std::string slot = opennova::anim::kAnimSlotNames[s];
		if (authors(slot)) rows.push_back({"anim_" + slot, soldier_slot_clip(slot)});
	}
	return mint_table(rows, soldier_clips(), bytes, err);
}

// msoldier.adm (and E_STAND.adm, the default infantry table, the same rows): the soldier's.
bool build_soldier_table(std::vector<uint8_t> &bytes, std::string &err) {
	return build_body_table(soldier_authors, bytes, err);
}

// mplayer.adm: the player's.
bool build_player_table(std::vector<uint8_t> &bytes, std::string &err) {
	return build_body_table(player_authors, bytes, err);
}

// mfp_m4.adm: the first-person M4's rows.
bool build_fp_m4_table(std::vector<uint8_t> &bytes, std::string &err) {
	return mint_table({{"anim_reset", "mfp_rst"}, {"anim_wpn_idle", "mfp_idle"}, {"anim_wpn_fire", "mfp_fire"},
			{"anim_wpn_recoil", "mfp_recoil"}, {"anim_wpn_reload", "mfp_reload"}, {"anim_wpn_empty", "mfp_empty"},
			{"anim_wpn_switchto", "mfp_swto"}, {"anim_wpn_switchfrom", "mfp_swfrom"},
			{"anim_wpn_switchrank", "mfp_swrank"}},
			fp_m4_clips(), bytes, err);
}

// Each clip of `clips` minted against the first (the table's reset).
bool mint_clips(const std::vector<opennova::bad::BadBuildClip> &clips, const std::string &dir, bool write_mode,
		int &failures) {
	for (const opennova::bad::BadBuildClip &clip : clips) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (!expect(opennova::bad::bad_build_mint(clip, &clips.front(), opennova::bad::bad_retail_limits(), bytes,
				&error), (clip.name + ": " + error).c_str()))
			return false;
		failures += emit(dir + clip.name + ".bad", bytes, write_mode);
	}
	return true;
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
		{"mrock.3di", make_rock},
		{"matv.3di", make_atv},
		{"mladder.3di", make_ladder},
		{"mbuggy.3di", make_buggy},
		{"mlndmine.3di", make_minefield},
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
		{"rock_collision.bms", build_rock_collision},
		{"infantry.bms", build_infantry},
		{"ladder.bms", build_ladder},
		{"vehicle_ride.bms", build_vehicle_ride},
		{"land_vehicles.bms", build_land_vehicles},
		{"minefield.bms", build_minefield},
		{"minefield_coop.bms", build_minefield_coop},
	};
	for (const MissionRecipe &recipe : missions) {
		std::vector<uint8_t> bytes;
		if (!expect(recipe.build(bytes, err), (std::string(recipe.file) + ": " + err).c_str())) return 1;
		if (emit(set + recipe.file, bytes, write_mode) != 0) return 1;
	}
	// Its clips and their tables.
	int failures = 0;
	if (!mint_clips(soldier_clips(), set, write_mode, failures) || !mint_clips(fp_m4_clips(), set, write_mode, failures))
		return 1;
	struct TableRecipe {
		const char *file;
		bool (*build)(std::vector<uint8_t> &, std::string &);
	};
	const TableRecipe tables[] = {
		{"msoldier.adm", build_soldier_table},
		{"E_STAND.adm", build_soldier_table},
		{"mplayer.adm", build_player_table},
		{"mfp_m4.adm", build_fp_m4_table},
	};
	for (const TableRecipe &recipe : tables) {
		std::vector<uint8_t> bytes;
		if (!expect(recipe.build(bytes, err), (std::string(recipe.file) + ": " + err).c_str())) return 1;
		failures += emit(set + recipe.file, bytes, write_mode);
	}
	return failures == 0 ? 0 : 1;
}
