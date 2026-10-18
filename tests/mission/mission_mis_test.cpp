#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/def/reserved_items.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_mis.h>

namespace {

std::string temp_path(const char *name) {
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/build/test-output/" +
			std::to_string(test_paths_pid());
	std::filesystem::create_directories(dir);
	return dir + "/" + name;
}

// The .mis file legs the retired document facade carried: read the text, parse
// it; write the text, save it.
bool load_mis_file(const std::string &path, opennova::bms::File &out, std::string &error) {
	const std::string text = test_io::read_file_text(path);
	if (text.empty()) {
		error = "Cannot open MIS file: " + path;
		return false;
	}
	if (!opennova::mission::parse_mis_text(text, {}, out, error)) return false;
	opennova::mission::sync_counts(out);
	return true;
}

bool save_mis_file(const opennova::bms::File &file, const std::string &path, std::string &error,
		const std::vector<int32_t> *base_heights = nullptr) {
	std::string text;
	if (!opennova::mission::write_mis_text(file, text, error, base_heights)) return false;
	std::ofstream out(path, std::ios::binary);
	if (!out.good()) {
		error = "Cannot create MIS file: " + path;
		return false;
	}
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	return out.good();
}

} // namespace

int main() {
	using namespace opennova::mission;
	namespace bms = opennova::bms;
	std::string error;

	// The header's Q8.8 start hour as HHMM, its minutes rounded: 0x0C80 is 12:30, and every authored minute the
	// .mis reader packs comes back.
	TEST_EXPECT(header_time_to_hhmm(0x0C80) == 1230 && header_time_to_hhmm(0x0600) == 600 &&
	            header_time_to_hhmm(0x0604) == 601 && header_time_to_hhmm(0x06FC) == 659);

	bms::File authored;
	make_default(authored);
	TEST_EXPECT(set_header_string(authored, "mission_name", "MIS Roundtrip", error));
	TEST_EXPECT(set_header_string(authored, "designer", "OpenNova", error));
	TEST_EXPECT(set_header_string(authored, "terrain", "dvxi5", error));
	TEST_EXPECT(set_header_string(authored, "environment", "full_00", error));
	TEST_EXPECT(set_header_int(authored, "weather", 2, error));
	TEST_EXPECT(set_header_int(authored, "minutes_per_day", 1440, error));
	TEST_EXPECT(set_header_float(authored, "map_zoom", 2.5f, error));

	EntityTransform transform;
	transform.x = 12.0f;
	transform.y = -3.5f;
	transform.z = 44.25f;
	transform.pitch = 1;
	transform.yaw = 90;
	transform.roll = 3;
	TEST_EXPECT(add_entity(authored, EntityKind::Item, 101291, transform) == 0);
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "group", 7, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "waypoint_id", 2, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "wp_number", 13, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "team", 1, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "ai_flags",
			static_cast<int>(bms::BmsiAttributeFlags::Blind), error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "perception", 80, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "accuracy", 60, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "alert_state", 4, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "min_engagement_distance", 30, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "max_engagement_distance", 300, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "max_attack_distance", 500, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "spawn_count", 6, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "max_simultaneous", 2, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "no_less_than", 1, error));
	TEST_EXPECT(set_entity_property_int(authored, EntityKind::Item, 0, "map_symbol", 9, error));
	TEST_EXPECT(set_entity_property_string(authored, EntityKind::Item, 0, "name1", "rifle", error));
	TEST_EXPECT(set_entity_property_string(authored, EntityKind::Item, 0, "name2", "patrol", error));

	MissionEventRecord event_seed;
	event_seed.flags = static_cast<int>(bms::EventFlags::ResetAfter);
	event_seed.reset_after = 9;
	event_seed.delay = 4;
	const size_t event_index = add_event(authored, event_seed);
	MissionTriggerRecord trigger;
	trigger.condition_flags = 2;
	trigger.main_type = static_cast<int>(bms::TriggerMainType::MissionVariable);
	trigger.sub_type = static_cast<int>(bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
	trigger.param1 = 3;
	trigger.param2 = 10;
	TEST_EXPECT(insert_event_trigger(authored, event_index, 0, trigger, error));
	MissionActionRecord action;
	action.action_type = static_cast<int>(bms::ActionType::MisvarChange);
	action.action_sub_type = static_cast<int>(bms::MissionVariableActionSubType::Set);
	action.param1 = 4;
	action.param2 = 12;
	TEST_EXPECT(insert_event_action(authored, event_index, 0, action, error));

	const std::string mis_path = temp_path("mission_mis_roundtrip.mis");
	TEST_EXPECT(save_mis_file(authored, mis_path, error));
	const std::string text = test_io::read_file_text(mis_path);
	TEST_EXPECT(text.find("// mission metafile\r\n") == 0);
	TEST_EXPECT(text.find("begin general_information\r\n") != std::string::npos);
	TEST_EXPECT(text.find("begin event 0\r\n") != std::string::npos);
	TEST_EXPECT(text.find("begin item 0\r\n") != std::string::npos);

	bms::File parsed;
	// No items.def in this authored-fixture test: the empty resolver keeps every
	// record in the item pool (classification is pinned by the idempotency test).
	TEST_EXPECT(load_mis_file(mis_path, parsed, error));
	const MissionInfo parsed_info = mission_info(parsed);
	TEST_EXPECT(parsed_info.mission_name == "MIS Roundtrip");
	TEST_EXPECT(parsed_info.designer == "OpenNova");
	TEST_EXPECT(parsed_info.terrain == "dvxi5");
	TEST_EXPECT(parsed_info.environment == "full_00");
	TEST_EXPECT(parsed_info.weather == 2);
	TEST_EXPECT(parsed_info.minutes_per_day == 1440);
	TEST_EXPECT(parsed_info.map_zoom == 2.5f);
	TEST_EXPECT(entity_count(parsed, EntityKind::Item) == 1);
	TEST_EXPECT(entity_count(parsed, EntityKind::Building) == 0);
	TEST_EXPECT(entity_count(parsed, EntityKind::Marker) == 0);
	TEST_EXPECT(entity_count(parsed, EntityKind::Organic) == 0);
	const bms::Entity &parsed_entity = parsed.items[0];
	const EntityTransform parsed_transform = entity_transform(parsed_entity);
	TEST_EXPECT(entity_item_id(parsed_entity) == 101291);
	TEST_EXPECT(parsed_transform.x == 12.0f);
	TEST_EXPECT(parsed_transform.y == -3.5f);
	TEST_EXPECT(parsed_transform.z == 44.25f);
	TEST_EXPECT(parsed_transform.yaw == 90);
	TEST_EXPECT(parsed_entity.group_id == 7);
	TEST_EXPECT(parsed_entity.waypoint_id == 2);
	TEST_EXPECT(parsed_entity.wp_number == 13);
	TEST_EXPECT(parsed_entity.team == 1);
	TEST_EXPECT(parsed_entity.bmsi_attributes == static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind));
	TEST_EXPECT(parsed_entity.perception2 == 80);
	TEST_EXPECT(parsed_entity.w_accuracy1 == 60);
	TEST_EXPECT(parsed_entity.alert_state == 4);
	TEST_EXPECT(parsed_entity.min_engagement_distance == 30);
	TEST_EXPECT(parsed_entity.max_engagement_distance == 300);
	TEST_EXPECT(parsed_entity.max_attack_distance == 500);
	TEST_EXPECT(parsed_entity.spawns == 6);
	TEST_EXPECT(parsed_entity.no_more_than == 2);
	TEST_EXPECT(parsed_entity.no_less_than == 1);
	TEST_EXPECT(parsed_entity.map_symbol == 9);
	TEST_EXPECT(entity_name1(parsed_entity) == "rifle");
	TEST_EXPECT(entity_name2(parsed_entity) == "patrol");
	TEST_EXPECT(parsed.events.size() == 1);
	TEST_EXPECT(parsed.triggers.size() == 1);
	TEST_EXPECT(parsed.actions.size() == 1);
	MissionEventChain chain;
	TEST_EXPECT(event_chain(parsed, 0, chain));
	TEST_EXPECT(chain.event.flags == static_cast<int>(bms::EventFlags::ResetAfter));
	TEST_EXPECT(chain.event.reset_after == 9);
	TEST_EXPECT(chain.event.delay == 4);
	TEST_EXPECT(chain.triggers.size() == 1);
	TEST_EXPECT(chain.triggers[0].main_type == static_cast<int>(bms::TriggerMainType::MissionVariable));
	TEST_EXPECT(chain.triggers[0].param2 == 10);
	TEST_EXPECT(chain.actions.size() == 1);
	TEST_EXPECT(chain.actions[0].action_type == static_cast<int>(bms::ActionType::MisvarChange));
	TEST_EXPECT(chain.actions[0].action_sub_type == static_cast<int>(bms::MissionVariableActionSubType::Set));
	TEST_EXPECT(chain.actions[0].param2 == 12);

	const std::string bms_path = temp_path("mission_mis_roundtrip.bms");
	sync_counts(parsed);
	TEST_EXPECT(bms::write_file(parsed, bms_path, error));
	bms::File bms_reload;
	TEST_EXPECT(bms::parse_file(bms_path, bms_reload, error));
	TEST_EXPECT(mission_info(bms_reload).mission_name == "MIS Roundtrip");
	TEST_EXPECT(entity_count(bms_reload, EntityKind::Item) == 1);
	TEST_EXPECT(bms_reload.events.size() == 1);

	// --- .mis heights (D-MIS-4): the original editor's frames ------------------------------------
	// A new entity is unlocked, as the original editor's new item [orig: JOTACmed.exe MissionItem_AppendByTypeId @
	// 0x455954], its base 0. Written with no terrain height known it is written locked, its z absolute as it stands,
	// which the original reads exact over whatever terrain it loads (D-MIS-17), and it reads back so.
	TEST_EXPECT(authored.items[0].mis_height_lock == 0 && authored.items[0].mis_extra_bheight == 0);
	TEST_EXPECT(text.find("  height_lock 1\r\n") != std::string::npos);
	TEST_EXPECT(text.find("  extra_bheight 0\r\n") != std::string::npos);
	TEST_EXPECT(text.find("  position " + std::to_string(authored.items[0].x) + " " + std::to_string(authored.items[0].y) +
	                      " " + std::to_string(authored.items[0].z) + "\r\n") != std::string::npos);
	TEST_EXPECT(parsed.items.size() == 1);
	TEST_EXPECT(parsed.mis_frame == bms::MisFrame::Mis && authored.mis_frame == bms::MisFrame::Bms);
	TEST_EXPECT(parsed.items[0].mis_height_lock == 1);
	TEST_EXPECT(parsed.items[0].mis_extra_bheight == 0);
	TEST_EXPECT(parsed.items[0].z == authored.items[0].z);

	// The terrain's heights under the entities (flat, in write order): an unlocked entity's z is
	// written relative to the terrain, less its height, its base as it stands; a locked one's z and
	// its base, which the .bms's frame carries absolute (D-MIS-4), as they stand, height_lock 1
	// [orig: JOTACmed.exe sub_44E640 @ 0x44e640, from MIS_SaveDocument @ 0x4546b5]. The writer
	// changes nothing of the document.
	{
		const std::string baked_path = temp_path("mission_mis_baked_heights.mis");
		const std::vector<int32_t> base_heights = {25 * 65536};
		TEST_EXPECT(save_mis_file(authored, baked_path, error, &base_heights));
		const std::string baked_text = test_io::read_file_text(baked_path);
		const int32_t z = authored.items[0].z;
		const std::string at = "  position " + std::to_string(authored.items[0].x) + " " + std::to_string(authored.items[0].y) + " ";
		TEST_EXPECT(baked_text.find(at + std::to_string(z - 25 * 65536) + "\r\n") != std::string::npos);
		TEST_EXPECT(baked_text.find("  extra_bheight 0\r\n") != std::string::npos);
		TEST_EXPECT(baked_text.find("height_lock") == std::string::npos);
		TEST_EXPECT(authored.items[0].mis_extra_bheight == 0 && authored.items[0].z == z);

		bms::File locked = authored;
		locked.items[0].mis_height_lock = 1;
		locked.items[0].mis_extra_bheight =
				27 * 65536; // its base, absolute: two units above the 25-unit ground
		std::string locked_text;
		TEST_EXPECT(write_mis_text(locked, locked_text, error, &base_heights));
		TEST_EXPECT(locked_text.find(at + std::to_string(z) + "\r\n") != std::string::npos);
		TEST_EXPECT(locked_text.find("  extra_bheight " + std::to_string(27 * 65536) + "\r\n") != std::string::npos);
		TEST_EXPECT(locked_text.find("  height_lock 1\r\n") != std::string::npos);
		// A lock is written as it stands, as the original writes the int it read [orig: JOTACmed.exe MIS_SaveDocument @ 0x45589a].
		locked.items[0].mis_height_lock = -1;
		TEST_EXPECT(write_mis_text(locked, locked_text, error, &base_heights));
		TEST_EXPECT(locked_text.find("  height_lock -1\r\n") != std::string::npos);
		TEST_EXPECT(locked_text.find(at + std::to_string(z) + "\r\n") != std::string::npos);
	}

	// A .mis as the original editor writes it (authored): an unlocked item (z 3 units above the
	// ground, base 1) and a locked one (z 70 absolute, base 52 absolute), over a flat terrain 50
	// units high. The parse reads the file's own frame; mis_items_to_bms_frame (the original's
	// read, sub_44E6A0 @ 0x44e6a0, then its .bms export, sub_44E5A0 @ 0x44e5a0) takes it to the
	// .bms's: both z absolute (53, 70), the unlocked base relative (1) and the locked one absolute
	// (52), as the original keeps a locked item's through a terrain change (D-MIS-4); and the
	// writer over the same terrain gives the original's text back.
	{
		std::vector<uint16_t> atlas(1024 * 1024, 0);
		std::vector<int> sectors(256, 1);
		MisTerrain flat;
		flat.heights = atlas.data();
		flat.sectors = sectors.data();
		// The depth word whose height the original editor samples as 50 units: (word * 2) << 7 = 50 << 16.
		for (uint16_t &word : atlas) word = static_cast<uint16_t>((50 * 65536) / 256);
		const int32_t ground = mis_terrain_height(flat, 10 * 65536, -10 * 65536);
		TEST_EXPECT(ground == 50 * 65536);
		const std::string med =
				"// mission metafile\r\n\r\n"
				"begin item 0\r\n  type_id 1\r\n  id 1\r\n  position 655360 -655360 196608\r\n"
				"  extra_bheight 65536\r\nend item\r\n\r\n"
				"begin item 1\r\n  type_id 1\r\n  id 2\r\n  position 655360 -655360 4587520\r\n"
				"  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  height_lock 1\r\nend "
				"item\r\n\r\n";
		bms::File read;
		TEST_EXPECT(parse_mis_text(med, {}, read, error) && read.items.size() == 2);
		TEST_EXPECT(read.items[0].z == 3 * 65536 && read.items[0].mis_extra_bheight == 65536 && read.items[0].mis_height_lock == 0);
		TEST_EXPECT(read.items[1].z == 70 * 65536 && read.items[1].mis_extra_bheight == 52 * 65536 &&
		            read.items[1].mis_height_lock == 1);
		TEST_EXPECT(read.mis_frame == bms::MisFrame::Mis);
		// In the file's own frame, the writer writes every height back as it stands, whatever terrain it is given.
		const std::vector<int32_t> under = {ground, ground};
		std::string as_read;
		TEST_EXPECT(write_mis_text(read, as_read, error, &under));
		TEST_EXPECT(as_read.find("  position 655360 -655360 196608\r\n") != std::string::npos);
		TEST_EXPECT(as_read.find("  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  "
								 "height_lock 1\r\n") != std::string::npos);
		// No terrain leaves it in the file's frame.
		bms::File untaken = read;
		mis_items_to_bms_frame(untaken, MisTerrain{});
		TEST_EXPECT(untaken.mis_frame == bms::MisFrame::Mis && bms::equal(untaken, read));
		mis_items_to_bms_frame(read, flat);
		TEST_EXPECT(read.mis_frame == bms::MisFrame::Bms && !bms::equal(untaken, read));
		TEST_EXPECT(read.items[0].z == 53 * 65536 && read.items[0].mis_extra_bheight == 65536);
		TEST_EXPECT(read.items[1].z == 70 * 65536 && read.items[1].mis_extra_bheight == 52 * 65536);
		// A file in the .bms's frame is left as it is: the terrain's height is never added twice.
		mis_items_to_bms_frame(read, flat);
		TEST_EXPECT(read.items[0].z == 53 * 65536 && read.items[1].z == 70 * 65536 &&
				read.items[1].mis_extra_bheight == 52 * 65536);
		std::string back;
		TEST_EXPECT(write_mis_text(read, back, error, &under));
		TEST_EXPECT(back.find("  position 655360 -655360 196608\r\n") != std::string::npos);
		TEST_EXPECT(back.find("  extra_bheight 65536\r\n") != std::string::npos);
		TEST_EXPECT(back.find("  position 655360 -655360 4587520\r\n") != std::string::npos);
		TEST_EXPECT(back.find("  extra_bheight 3407872\r\n") != std::string::npos);
		size_t locks = 0;
		for (size_t at = back.find("height_lock"); at != std::string::npos; at = back.find("height_lock", at + 1)) ++locks;
		TEST_EXPECT(locks == 1);

		// A lock is any nonzero value, read as the original reads it, an int tested nonzero [orig: JOTACmed.exe
		// MIS_ParseToken @ 0x4536c9; sub_44E640 @ 0x44e657]: -1 is a lock, its z and base kept
		// absolute through the read and written back as -1; 300 likewise.
		bms::File odd;
		TEST_EXPECT(parse_mis_text(
				"begin item 0\r\n  type_id 1\r\n  id 1\r\n  position 655360 -655360 4587520\r\n"
				"  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  height_lock -1\r\nend "
				"item\r\n\r\n"
				"begin item 1\r\n  type_id 1\r\n  id 2\r\n  position 655360 -655360 4587520\r\n"
				"  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  height_lock 300\r\nend "
				"item\r\n\r\n",
				{}, odd, error));
		TEST_EXPECT(odd.items.size() == 2 && odd.items[0].mis_height_lock == -1 && odd.items[1].mis_height_lock == 300);
		mis_items_to_bms_frame(odd, flat);
		TEST_EXPECT(odd.items[0].z == 70 * 65536 && odd.items[0].mis_extra_bheight == 52 * 65536);
		TEST_EXPECT(odd.items[1].z == 70 * 65536 && odd.items[1].mis_extra_bheight == 52 * 65536);
		std::string odd_text;
		TEST_EXPECT(write_mis_text(odd, odd_text, error, &under));
		TEST_EXPECT(odd_text.find("  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  "
								  "height_lock -1\r\n") != std::string::npos);
		TEST_EXPECT(odd_text.find("  extra_bheight 3407872\r\n  gen_string \"null\"\r\n  "
								  "height_lock 300\r\n") != std::string::npos);

		// The areas the original editor makes of its type-6013 items (D-MIS-14) [orig: JOTACmed.exe
		// MissionDocument_RebuildAreasFromMarkers @ 0x44d240; MIS_ExportBmsFile @ 0x450a16..0x450ab3]: zone 3's three unlocked markers
		// span its box, its flags its areatrig's; zone 5's areatrig has no marker, so no area; zone
		// 7's least-x marker holds an extra_valmode, so the export keeps none. The markers are none
		// of the pools. Over the 50-unit terrain the box's z is the markers' absolute z.
		{
			const std::string areas_med =
					"begin areatrig 3\r\n  description \"Bridge\"\r\n  attrib 1\r\nend "
					"areatrig\r\n\r\n"
					"begin areatrig 5\r\n  description \"Empty\"\r\nend areatrig\r\n\r\n"
					"begin item 0\r\n  type_id 1\r\n  id 1\r\n  position 655360 -655360 "
					"196608\r\nend item\r\n\r\n"
					"begin item 1\r\n  type_id 6013\r\n  id 2\r\n  position 1310720 -1310720 "
					"65536\r\n  wpnumber 3\r\nend item\r\n\r\n"
					"begin item 2\r\n  type_id 6013\r\n  id 3\r\n  position 655360 -2621440 "
					"131072\r\n  wpnumber 3\r\nend item\r\n\r\n"
					"begin item 3\r\n  type_id 6013\r\n  id 4\r\n  position 1966080 -655360 0\r\n  "
					"wpnumber 3\r\nend item\r\n\r\n"
					"begin item 4\r\n  type_id 6013\r\n  id 5\r\n  position 65536 0 0\r\n  "
					"wpnumber 7\r\n  extra_valmode 1\r\n  extra_bheight 65536\r\nend item\r\n\r\n"
					"begin item 5\r\n  type_id 6013\r\n  id 6\r\n  position 131072 0 0\r\n  "
					"wpnumber 7\r\nend item\r\n\r\n";
			bms::File areas_read;
			TEST_EXPECT(parse_mis_text(areas_med, {}, areas_read, error));
			TEST_EXPECT(areas_read.items.size() == 1 && areas_read.mis_area_markers.size() == 5);
			TEST_EXPECT(areas_read.area_triggers.size() == 1);
			if (areas_read.area_triggers.size() == 1) {
				const bms::AreaTrigger &a = areas_read.area_triggers[0];
				TEST_EXPECT(a.id == 3 && a.flags == 1 && a.x_min == 655360 && a.x_max == 1966080 &&
						a.y_min == -2621440 && a.y_max == -655360 && a.z_min == 0 &&
						a.z_max == 131072);
			}
			// num_items counts every item section, the markers among them: the original editor
			// walks that many.
			const auto items_counted = [](const std::string &text) {
				size_t sections = 0;
				for (size_t at = text.find("begin item "); at != std::string::npos;
						at = text.find("begin item ", at + 1))
					++sections;
				const size_t key = text.find("  num_items ");
				return key != std::string::npos && std::stoul(text.substr(key + 12)) == sections;
			};
			const auto count_of = [](const std::string &text, const std::string &what) {
				size_t n = 0;
				for (size_t at = text.find(what); at != std::string::npos;
						at = text.find(what, at + 1))
					++n;
				return n;
			};
			// Written in the .mis's frame (no terrain took it over): the markers as read, zone 7's
			// extra_valmode and its least-x marker's base kept, num_items 6.
			{
				std::string as_read;
				TEST_EXPECT(write_mis_text(areas_read, as_read, error));
				TEST_EXPECT(items_counted(as_read) &&
						as_read.find("  num_items 6\r\n") != std::string::npos);
				TEST_EXPECT(count_of(as_read, "  type_id 6013\r\n") == 5 &&
						as_read.find("  extra_valmode 1\r\n") != std::string::npos &&
						as_read.find("  extra_bheight 65536\r\n") != std::string::npos);
				bms::File again;
				TEST_EXPECT(parse_mis_text(as_read, {}, again, error));
				TEST_EXPECT(again.mis_area_markers.size() == 5 && again.area_triggers.size() == 1 &&
						again.area_triggers[0].flags == 1);
				// Zone 3's box widened: its three markers go, two corners at the new box come,
				// unlocked as its least-x marker was, their base 0; zone 7's stay. Read back, the
				// edited box.
				bms::File edited = areas_read;
				edited.area_triggers[0].x_max += 65536;
				std::string written;
				TEST_EXPECT(write_mis_text(edited, written, error));
				TEST_EXPECT(items_counted(written) &&
						count_of(written, "  type_id 6013\r\n") == 4 &&
						written.find("  position 2031616 -655360 131072\r\n") != std::string::npos);
				bms::File reread;
				TEST_EXPECT(parse_mis_text(written, {}, reread, error));
				TEST_EXPECT(reread.area_triggers.size() == 1 &&
						reread.area_triggers[0].x_max == 2031616 &&
						reread.area_triggers[0].x_min == 655360 &&
						reread.area_triggers[0].z_max == 131072 &&
						reread.area_triggers[0].flags == 1);
				// The area gone: its markers go with it; zone 7's stay.
				edited.area_triggers.clear();
				TEST_EXPECT(write_mis_text(edited, written, error));
				TEST_EXPECT(items_counted(written) && count_of(written, "  type_id 6013\r\n") == 2);
				// Zone 3's least-x marker locked: its edited box's corners are written locked, as
				// it was.
				bms::File locked = areas_read;
				for (bms::MisAreaMarker &m : locked.mis_area_markers)
					if (m.zone == 3 && m.x == 655360)
						m.height_lock = 1;
				locked.area_triggers[0].x_max += 65536;
				TEST_EXPECT(write_mis_text(locked, written, error));
				const size_t corner = written.find("  position 655360 -2621440 0\r\n");
				TEST_EXPECT(corner != std::string::npos &&
						written.find("  height_lock 1\r\n", corner) <
								written.find("end item", corner));
			}
			mis_items_to_bms_frame(areas_read, flat);
			// Zone 7, which the export makes no area of, keeps its markers as read, its least-x
			// marker's base among them.
			TEST_EXPECT(areas_read.area_triggers.size() == 1 &&
					areas_read.mis_area_markers.size() == 2 &&
					areas_read.mis_area_markers[0].zone == 7 &&
					areas_read.mis_area_markers[0].extra_valmode == 1 &&
					areas_read.mis_area_markers[0].extra_bheight == 65536);
			if (areas_read.area_triggers.size() == 1)
				TEST_EXPECT(areas_read.area_triggers[0].z_min == 50 * 65536 &&
						areas_read.area_triggers[0].z_max == 52 * 65536);
			// Written as two locked markers at the box's corners, its zone their wpnumber; read
			// back, the same area.
			std::string corners;
			TEST_EXPECT(write_mis_text(areas_read, corners, error));
			TEST_EXPECT(corners.find("  type_id 6013\r\n") != std::string::npos);
			TEST_EXPECT(
					corners.find("  position 655360 -2621440 3276800\r\n") != std::string::npos);
			TEST_EXPECT(
					corners.find("  position 1966080 -655360 3407872\r\n") != std::string::npos);
			// Five item sections: the item, zone 3's two corners, zone 7's two markers; the
			// corners' base 0, zone 7's extra_valmode kept.
			TEST_EXPECT(items_counted(corners) &&
					corners.find("  num_items 5\r\n") != std::string::npos);
			TEST_EXPECT(corners.find("  extra_valmode 1\r\n") != std::string::npos &&
					corners.find("  extra_bheight 3276800\r\n") == std::string::npos);
			bms::File corners_read;
			TEST_EXPECT(parse_mis_text(corners, {}, corners_read, error));
			mis_items_to_bms_frame(corners_read, flat);
			TEST_EXPECT(corners_read.area_triggers.size() == 1 && corners_read.items.size() == 1 &&
					corners_read.mis_area_markers.size() == 2);
			if (corners_read.area_triggers.size() == 1 && areas_read.area_triggers.size() == 1) {
				const bms::AreaTrigger &a = corners_read.area_triggers[0],
									   &b = areas_read.area_triggers[0];
				TEST_EXPECT(a.id == b.id && a.flags == b.flags && a.x_min == b.x_min &&
						a.x_max == b.x_max && a.y_min == b.y_min && a.y_max == b.y_max &&
						a.z_min == b.z_min && a.z_max == b.z_max);
			}
		}
	}

	// Over a sloped terrain, every pool, each entity at a point of its own: an atlas whose four 512-texel quadrants
	// differ and slope along both axes, its sectors naming each quadrant and none. An entity in each pool, unlocked
	// and locked, KOTH centres (6006) with bases of their own among the markers. Written with the terrain's height
	// under each (the original's save), read with the pools' resolver, taken to the .bms's frame (the original's read
	// and its .bms export): every z and base as they were, and the text written again the same. Written with no
	// terrain height, every entity is written locked and comes back to its z over any terrain (D-MIS-17).
	{
		std::vector<uint16_t> atlas(1024 * 1024, 0);
		for (int row = 0; row < 1024; ++row)
			for (int column = 0; column < 1024; ++column) {
				const int quadrant = (row >= 512 ? 1 : 0) | (column >= 512 ? 2 : 0);
				atlas[static_cast<size_t>(row) * 1024 + static_cast<size_t>(column)] =
						static_cast<uint16_t>(100 + 2000 * quadrant + 3 * (row % 512) + 2 * (column % 512));
			}
		std::vector<int> sectors(256, 0);
		for (int i = 0; i < 256; ++i) sectors[static_cast<size_t>(i)] = i % 5; // 0 names no sector
		MisTerrain sloped;
		sloped.heights = atlas.data();
		sloped.sectors = sectors.data();

		bms::File source;
		make_default(source);
		struct Placed {
			EntityKind kind;
			int def_id;
			int cell_x, cell_y, east, south; // its sector cell and its point within it, in units
			int32_t base;
			int32_t lock;
		};
		const Placed placed[] = {
				{EntityKind::Item, 100010, 1, 0, 37, 59, 0, 0},
				{EntityKind::Item, 100010, 4, 3, 300, 11, 65536, 1},
				{EntityKind::Building, 100020, 7, 2, 451, 402, 0, 0},
				{EntityKind::Building, 100020, 2, 9, 128, 260, 4 * 65536, 1},
				{EntityKind::Marker, 106006, 13, 5, 77, 490, 5 * 65536, 0},
				{EntityKind::Marker, 106006, 10, 12, 205, 33, 3 * 65536, -1},
				{EntityKind::Marker, 100050, 0, 0, 10, 10, 0, 0}, // cell 0: no sector, the height 0
				{EntityKind::Organic, 100030, 6, 13, 15, 333, 0, 0},
				{EntityKind::Organic, 100030, 11, 1, 499, 499, 65536 / 2, 1},
		};
		for (const Placed &p : placed) {
			EntityTransform where;
			where.x = static_cast<float>(p.cell_x * 512 + p.east) + 0.25f;
			where.y = -(static_cast<float>(p.cell_y * 512 + p.south) + 0.75f);
			where.z = 40.0f + static_cast<float>(p.east % 7);
			const int index = add_entity(source, p.kind, p.def_id, where);
			TEST_EXPECT(index >= 0);
			std::vector<bms::Entity> &pool = p.kind == EntityKind::Item       ? source.items
			                                 : p.kind == EntityKind::Building ? source.buildings
			                                 : p.kind == EntityKind::Marker   ? source.markers
			                                                                  : source.organics;
			pool[static_cast<size_t>(index)].mis_extra_bheight = p.base;
			pool[static_cast<size_t>(index)].mis_height_lock = p.lock;
		}
		TEST_EXPECT(source.items.size() == 2 && source.buildings.size() == 2 && source.markers.size() == 3 &&
		            source.organics.size() == 2);
		const std::vector<bms::Entity> *pools[4] = {&source.items, &source.buildings, &source.markers, &source.organics};
		std::vector<int32_t> heights;
		for (const std::vector<bms::Entity> *pool : pools)
			for (const bms::Entity &entity : *pool) heights.push_back(mis_terrain_height(sloped, entity.x, entity.y));
		// A height of its own under each entity, the one on no sector 0.
		for (size_t i = 0; i < heights.size(); ++i)
			for (size_t j = i + 1; j < heights.size(); ++j) TEST_EXPECT(heights[i] != heights[j]);
		TEST_EXPECT(heights[6] == 0);
		// Not the height under the point with y not negated, nor with x and y swapped.
		for (const std::vector<bms::Entity> *pool : pools)
			for (const bms::Entity &entity : *pool)
				if (mis_terrain_height(sloped, entity.x, entity.y) != 0)
					TEST_EXPECT(mis_terrain_height(sloped, entity.x, entity.y) != mis_terrain_height(sloped, entity.x, -entity.y) &&
					            mis_terrain_height(sloped, entity.x, entity.y) != mis_terrain_height(sloped, -entity.y, -entity.x));

		std::string sloped_text;
		TEST_EXPECT(write_mis_text(source, sloped_text, error, &heights));
		// The KOTH centres' lines: the unlocked one's z less the height and its base as it stands,
		// the locked one's z as it stands, its base (carried absolute, D-MIS-4) and its lock as
		// they stand.
		const bms::Entity &koth = source.markers[0];
		const bms::Entity &koth_locked = source.markers[1];
		TEST_EXPECT(sloped_text.find("  position " + std::to_string(koth.x) + " " + std::to_string(koth.y) + " " +
		                             std::to_string(koth.z - heights[4]) + "\r\n") != std::string::npos);
		TEST_EXPECT(sloped_text.find("  position " + std::to_string(koth_locked.x) + " " + std::to_string(koth_locked.y) +
		                             " " + std::to_string(koth_locked.z) + "\r\n") != std::string::npos);
		TEST_EXPECT(sloped_text.find("  extra_bheight " + std::to_string(5 * 65536) + "\r\n  gen_string") !=
		            std::string::npos);
		TEST_EXPECT(
				sloped_text.find("  extra_bheight " + std::to_string(3 * 65536) +
						"\r\n  gen_string \"null\"\r\n  height_lock -1\r\n") != std::string::npos);

		const MisItemTypeResolver resolver = [](int def_item_id) {
			switch (def_item_id) {
			case 100010: return 1; // vehicle: the item pool
			case 100020: return 5; // building
			case 106006: case 100050: return 4; // marker
			case 100030: return 3; // person: the organic pool
			default: return -1;
			}
		};
		bms::File read;
		TEST_EXPECT(parse_mis_text(sloped_text, resolver, read, error));
		const std::vector<bms::Entity> *read_pools[4] = {&read.items, &read.buildings, &read.markers, &read.organics};
		for (int pool = 0; pool < 4; ++pool) TEST_EXPECT(read_pools[pool]->size() == pools[pool]->size());
		mis_items_to_bms_frame(read, sloped);
		for (int pool = 0; pool < 4; ++pool)
			for (size_t i = 0; i < pools[pool]->size() && i < read_pools[pool]->size(); ++i) {
				const bms::Entity &was = (*pools[pool])[i];
				const bms::Entity &is = (*read_pools[pool])[i];
				TEST_EXPECT(is.id == was.id && is.x == was.x && is.y == was.y);
				TEST_EXPECT(is.z == was.z);
				TEST_EXPECT(is.mis_extra_bheight == was.mis_extra_bheight);
				TEST_EXPECT(is.mis_height_lock == was.mis_height_lock);
			}
		std::string again;
		TEST_EXPECT(write_mis_text(read, again, error, &heights));
		TEST_EXPECT(again == sloped_text);

		// No terrain height known: every entity locked, its own lock where it has one, and back to its z over the
		// terrain the reader loads, whatever it is.
		std::string blind;
		TEST_EXPECT(write_mis_text(source, blind, error));
		size_t blind_locks = 0;
		for (size_t at = blind.find("  height_lock "); at != std::string::npos; at = blind.find("  height_lock ", at + 1))
			++blind_locks;
		TEST_EXPECT(blind_locks == heights.size());
		TEST_EXPECT(blind.find("  height_lock -1\r\n") != std::string::npos);
		bms::File blind_read;
		TEST_EXPECT(parse_mis_text(blind, resolver, blind_read, error));
		mis_items_to_bms_frame(blind_read, sloped);
		const std::vector<bms::Entity> *blind_pools[4] = {&blind_read.items, &blind_read.buildings, &blind_read.markers,
		                                                  &blind_read.organics};
		for (int pool = 0; pool < 4; ++pool) {
			TEST_EXPECT(blind_pools[pool]->size() == pools[pool]->size());
			for (size_t i = 0; i < pools[pool]->size() && i < blind_pools[pool]->size(); ++i)
				TEST_EXPECT((*blind_pools[pool])[i].z == (*pools[pool])[i].z);
		}
	}

	// --- Base-10 numeric parsing (D-MIS-5): the octal regression ------------------------------
	// minutes_per_day emits UNPADDED (the old four_digit padding produced "0120", which a base-0
	// strtol reader corrupted to octal 80, degrading to 0 by the third generation); a
	// hand-authored zero-padded value still parses base-10, matching the original importer's
	// plain atol [orig: j__atol callers throughout MisLdr_ParseMisLine @ 0x100017b0, misldr.dll].
	{
		bms::File octal;
		make_default(octal);
		TEST_EXPECT(set_header_int(octal, "minutes_per_day", 120, error));
		std::string octal_text;
		TEST_EXPECT(write_mis_text(octal, octal_text, error));
		TEST_EXPECT(octal_text.find("  minutes_per_day 120\r\n") != std::string::npos);
		TEST_EXPECT(octal_text.find("  minutes_per_day 0120") == std::string::npos);

		bms::File hand;
		TEST_EXPECT(parse_mis_text(
				"begin general_information\r\n"
				"  minutes_per_day 0120\r\n"
				"end general_information\r\n",
				{}, hand, error));
		TEST_EXPECT(mission_info(hand).minutes_per_day == 120);
	}

	// --- Header write/read symmetry (D-MIS-5): fog_level / water_level / gen_def_val1..4 ------
	// fog_level / water_level are the RAW u32s at header offsets 156/152 (straddling the u16
	// override fields — the old parse truncated 45875200 to 0); gen_def_val1..4 are the u32s at
	// 264..276 (Header health/mana/music/reverb), previously write-only.
	{
		bms::File sym;
		make_default(sym);
		bms::Header &header = sym.header;
		header.water_override = 21;  // water_level u32 @152 reads 21 while unknown1 stays 0
		header.fog_override = 700;   // fog_level u32 @156 reads 700 << 16 == 45875200
		header.health = 11;
		header.mana = 22;
		header.music = 33;
		header.reverb = 44;
		std::string sym_text;
		TEST_EXPECT(write_mis_text(sym, sym_text, error));
		TEST_EXPECT(sym_text.find("  water_level 21\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  fog_level 45875200\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val1 11\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val2 22\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val3 33\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val4 44\r\n") != std::string::npos);

		bms::File sym_back;
		TEST_EXPECT(parse_mis_text(sym_text, {}, sym_back, error));
		const bms::Header &back = sym_back.header;
		TEST_EXPECT(back.water_override == 21);
		TEST_EXPECT(back.fog_override == 700);
		TEST_EXPECT(back.health == 11);
		TEST_EXPECT(back.mana == 22);
		TEST_EXPECT(back.music == 33);
		TEST_EXPECT(back.reverb == 44);
	}

	// --- A group's name (bms.h GroupRecord, the 20 bytes the game's loader drops) is its .mis
	// `description`. The original editor writes groups 1..63, each one an item names or one with a
	// description, its flags as `attrib` and its value as `commander` (the keys its reader takes). ------
	{
		bms::File named;
		make_default(named);
		TEST_EXPECT(bms::set_group_name(named.group_records[5], "Bridge guards", error));
		named.group_records[6].flags = 1;
		named.group_records[6].value = 3;
		TEST_EXPECT(bms::set_group_name(named.group_records[6], "Convoy", error));
		named.group_records[7].flags = 2; // flags, no member, no name: no section
		named.group_records[0].flags = 1; // group 0 is no group
		bms::Entity member{};
		member.type_id = 1;
		member.id = 1;
		member.group_id = 8; // a member, no name: a section with an empty description
		named.items.push_back(member);
		sync_counts(named);
		std::string named_text;
		TEST_EXPECT(write_mis_text(named, named_text, error));
		TEST_EXPECT(named_text.find("begin group 5\r\n  description \"Bridge guards\"\r\nend group\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin group 6\r\n  description \"Convoy\"\r\n  attrib 1\r\n  commander 3\r\n"
		                            "end group\r\n") != std::string::npos);
		TEST_EXPECT(named_text.find("begin group 8\r\n  description \"\"\r\nend group\r\n") != std::string::npos);
		TEST_EXPECT(named_text.find("begin group 4\r\n") == std::string::npos); // no name, no member
		TEST_EXPECT(named_text.find("begin group 7\r\n") == std::string::npos);
		TEST_EXPECT(named_text.find("begin group 0\r\n") == std::string::npos);
		TEST_EXPECT(named_text.find("  flags ") == std::string::npos && named_text.find("  value ") == std::string::npos);
		bms::File named_back;
		TEST_EXPECT(parse_mis_text(named_text, {}, named_back, error));
		TEST_EXPECT(bms::group_name(named_back.group_records[5]) == "Bridge guards");
		TEST_EXPECT(bms::group_name(named_back.group_records[6]) == "Convoy" &&
		            named_back.group_records[6].flags == 1 && named_back.group_records[6].value == 3);
		TEST_EXPECT(bms::group_name(named_back.group_records[4]).empty());
		std::string named_again;
		TEST_EXPECT(write_mis_text(named_back, named_again, error));
		TEST_EXPECT(named_again == named_text);
		// A description that is no group name (past 20 characters, a control character) is skipped with an issue,
		// never stored as bytes; the group's flags still read.
		bms::File long_name;
		std::vector<MisIssue> issues;
		TEST_EXPECT(parse_mis_text("begin group 2\r\n  description \"The northern ridge sentries\"\r\n  attrib 1\r\n"
		                           "end group\r\nbegin group 3\r\n  description \"tab\there\"\r\nend group\r\n",
		                           {}, long_name, error, &issues));
		TEST_EXPECT(bms::group_name(long_name.group_records[2]).empty() && long_name.group_records[2].flags == 1);
		TEST_EXPECT(bms::group_name_readable(long_name.group_records[2]) && bms::group_name_readable(long_name.group_records[3]));
		TEST_EXPECT(issues.size() == 2 && issues[0].line == 2 && issues[1].line == 6 &&
		            issues[0].message.find("group 2") != std::string::npos &&
		            issues[0].message.find("20 characters") != std::string::npos);
		// The same text read with no issues list still reads.
		bms::File quiet;
		TEST_EXPECT(parse_mis_text("begin group 2\r\n  description \"The northern ridge sentries\"\r\nend group\r\n", {}, quiet,
		                           error));
	}

	// --- The .mis names (MisNames, ADR 0046 S24 "Names"): an event's `name`, an area trigger's and a
	// path's `description`, written from the names and read back by the indexes the parse gives the
	// records (an area by its zone id); a path is written for a stop or a description, whatever its
	// flags. ------------------------------------------------------------------------------------------
	{
		bms::File doc;
		make_default(doc);
		const size_t first = add_event(doc, MissionEventRecord{});
		const size_t second = add_event(doc, MissionEventRecord{});
		AreaTriggerRecord area;
		area.wp_number = 9; // the zone id: the higher first, so the writer's id order shows
		area.max_x = 10.0f;
		area.max_y = 10.0f;
		add_area_trigger(doc, area);
		area.wp_number = 4;
		add_area_trigger(doc, area);
		doc.waypoint_records[3].flags = static_cast<bms::WaypointFlags>(1);
		sync_counts(doc);

		doc.waypoint_records[5].flags = static_cast<bms::WaypointFlags>(2); // flags, no stop, no description
		bms::Entity stop{};
		stop.type_id = opennova::def::DEF_TYPE_WAYPOINT;
		stop.id = 1;
		stop.waypoint_id = 6; // a stop, no flags, no description: a section with an empty description
		doc.markers.push_back(stop);
		sync_counts(doc);

		MisNames names;
		names.events = {"Get to the bridge", ""};
		names.areas[9] = "The bridge";
		names.paths[3] = "Patrol \"north\"";
		names.paths[7] = "Escape route";
		std::string named_text;
		TEST_EXPECT(write_mis_text(doc, named_text, error, nullptr, &names));
		TEST_EXPECT(named_text.find("begin event " + std::to_string(first) + "\r\n  name \"Get to the bridge\"\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin event " + std::to_string(second) + "\r\n  name \"\"\r\n") != std::string::npos);
		// An area is written under its zone id, in id order.
		TEST_EXPECT(named_text.find("begin areatrig 9\r\n  description \"The bridge\"\r\n") != std::string::npos);
		TEST_EXPECT(named_text.find("begin areatrig 4\r\n") < named_text.find("begin areatrig 9\r\n"));
		// A double quote cannot ride a quoted value: it is written as a single one.
		TEST_EXPECT(named_text.find("begin waypoint 3\r\n  description \"Patrol 'north'\"\r\n  attrib 1\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 7\r\n  description \"Escape route\"\r\nend waypoint\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 8\r\n") == std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 5\r\n") == std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 6\r\n  description \"\"\r\nend waypoint\r\n") != std::string::npos);

		MisNames back;
		TEST_EXPECT(parse_mis_names(named_text, back, error));
		TEST_EXPECT(back.events.size() == doc.events.size() && back.events[first] == "Get to the bridge" &&
		            back.events[second].empty());
		TEST_EXPECT(back.areas[9] == "The bridge" && back.areas[4].empty());
		TEST_EXPECT(back.paths[3] == "Patrol 'north'" && back.paths[7] == "Escape route" && back.paths[8].empty());
		// The records parse as before beside them: path 7 without flags, path 3 with its attrib.
		bms::File records;
		TEST_EXPECT(parse_mis_text(named_text, {}, records, error));
		TEST_EXPECT(static_cast<uint32_t>(records.waypoint_records[7].flags) == 0 &&
		            static_cast<uint32_t>(records.waypoint_records[3].flags) == 1);
		TEST_EXPECT(records.events.size() == doc.events.size() && records.area_triggers.size() == 2 &&
		            records.area_triggers[0].id == 4 && records.area_triggers[1].id == 9);
		// A path with flags and neither a stop nor a description has no section, so its flags do not come back
		// through the .mis: the original's writer drops them the same way (D-MIS-12).
		TEST_EXPECT(static_cast<uint32_t>(records.waypoint_records[5].flags) == 0);
		// Written again from what was read: the same text.
		sync_counts(records);
		std::string again;
		TEST_EXPECT(write_mis_text(records, again, error, nullptr, &back));
		TEST_EXPECT(again == named_text);
		// No names: the empty lines as before, and no section for a flag-less path.
		std::string unnamed;
		TEST_EXPECT(write_mis_text(doc, unnamed, error));
		TEST_EXPECT(unnamed.find("begin waypoint 7\r\n") == std::string::npos);
		TEST_EXPECT(unnamed.find("begin areatrig 9\r\n  description \"\"\r\n") != std::string::npos);
		MisNames none;
		TEST_EXPECT(parse_mis_names(unnamed, none, error));
		TEST_EXPECT(none.events.size() == doc.events.size() && none.events[first].empty());
		// What the stock readers cannot take is not written: a ';' (they cut the line there, quoted or not) as a
		// ',', and a name past the original editor's dialog cut to it (an event's 59 characters, a
		// description's 249).
		MisNames long_names;
		long_names.events = {std::string(70, 'e'), "north; south"};
		long_names.areas[4] = std::string(300, 'a');
		long_names.paths[3] = std::string(256, 'p');
		std::string capped;
		TEST_EXPECT(write_mis_text(doc, capped, error, nullptr, &long_names));
		TEST_EXPECT(capped.find("  name \"" + std::string(59, 'e') + "\"\r\n") != std::string::npos);
		TEST_EXPECT(capped.find("  name \"north, south\"\r\n") != std::string::npos);
		TEST_EXPECT(capped.find("begin areatrig 4\r\n  description \"" + std::string(249, 'a') + "\"\r\n") !=
		            std::string::npos);
		TEST_EXPECT(capped.find("begin waypoint 3\r\n  description \"" + std::string(249, 'p') + "\"\r\n") !=
		            std::string::npos);
		// An area's flags are its `attrib`, read back; an areatrig number past the original's 100 slots is
		// refused by the names' reader.
		bms::File flagged = doc;
		flagged.area_triggers[1].flags = 3; // zone 4
		std::string flagged_text;
		TEST_EXPECT(write_mis_text(flagged, flagged_text, error));
		TEST_EXPECT(flagged_text.find("begin areatrig 4\r\n  description \"\"\r\n  attrib 3\r\nend areatrig\r\n") !=
		            std::string::npos);
		bms::File flagged_back;
		TEST_EXPECT(parse_mis_text(flagged_text, {}, flagged_back, error));
		TEST_EXPECT(flagged_back.area_triggers.size() == 2 && flagged_back.area_triggers[0].id == 4 &&
		            flagged_back.area_triggers[0].flags == 3 && flagged_back.area_triggers[1].flags == 0);
		TEST_EXPECT(capped.find(';') == std::string::npos);

		// The original's .mis has an area slot per zone id 1..99 alone: an area outside them, or a second of an
		// id, is refused by the writer, naming it, never left out.
		for (const int32_t bad : {0, 100}) {
			bms::File off = doc;
			off.area_triggers[0].id = bad;
			std::string off_text, why;
			TEST_EXPECT(!write_mis_text(off, off_text, why));
			TEST_EXPECT(why.find("Area trigger 0 has zone id " + std::to_string(bad)) != std::string::npos);
		}
		bms::File twice = doc;
		twice.area_triggers[0].id = 4;
		std::string twice_text, twice_why;
		TEST_EXPECT(!write_mis_text(twice, twice_text, twice_why));
		TEST_EXPECT(twice_why.find("Area triggers 0 and 1 share zone id 4") != std::string::npos);
		// The readers agree on 1..99: a number outside makes no area and no name, a second section
		// of a number goes to the first's slot (its lines to the first's area, its description
		// replacing the first's), each reported, never dropped in silence; zone 7's area is its
		// marker's (D-MIS-14).
		const std::string odd_areas =
				"begin areatrig 0\r\n  description \"zero\"\r\nend areatrig\r\n"
				"begin areatrig 100\r\n  description \"past\"\r\nend areatrig\r\n"
				"begin areatrig 7\r\n  description \"first\"\r\nend areatrig\r\n"
				"begin areatrig 7\r\n  description \"second\"\r\n  attrib 2\r\nend areatrig\r\n"
				"begin item 0\r\n  type_id 6013\r\n  id 1\r\n  position 0 0 0\r\n  wpnumber "
				"7\r\nend item\r\n";
		bms::File odd;
		std::vector<MisIssue> odd_issues;
		TEST_EXPECT(parse_mis_text(odd_areas, {}, odd, error, &odd_issues));
		TEST_EXPECT(odd.area_triggers.size() == 1 && odd.area_triggers[0].id == 7 && odd.area_triggers[0].flags == 2);
		TEST_EXPECT(odd_issues.size() == 3 && odd_issues[0].line == 1 && odd_issues[1].line == 4 &&
		            odd_issues[2].line == 10 && odd_issues[2].message.find("second section") != std::string::npos);
		MisNames odd_names;
		std::vector<MisIssue> name_issues;
		TEST_EXPECT(parse_mis_names(odd_areas, odd_names, error, &name_issues));
		TEST_EXPECT(odd_names.areas[0].empty() && odd_names.areas[7] == "second");
		TEST_EXPECT(name_issues.size() == 3 && name_issues[0].line == 1 && name_issues[1].line == 4 &&
		            name_issues[2].line == 10);
		// An item's name and the mission's are not a record's: an unterminated section is refused.
		MisNames broken;
		TEST_EXPECT(!parse_mis_names("begin event 0\r\n  name \"x\"\r\n", broken, error));

		// A cap never splits a UTF-8 character: 58 bytes then a two-byte character is written as the 58.
		MisNames wide;
		wide.events = {std::string(58, 'a') + "\xC3\xA9", ""};
		std::string wide_text;
		TEST_EXPECT(write_mis_text(doc, wide_text, error, nullptr, &wide));
		TEST_EXPECT(wide_text.find("  name \"" + std::string(58, 'a') + "\"\r\n") != std::string::npos);
	}

	// --- One rule for a name the .mis carries (bms::mis_value_char): an event's, area's or path's name
	// refuses a control character, a double quote, a ';' and a length past its slot, as a group's does;
	// UTF-8 is taken. ----------------------------------------------------------------------------------
	{
		std::string why;
		TEST_EXPECT(mis_name_allowed("Get to the bridge", kMisEventNameChars, why));
		TEST_EXPECT(mis_name_allowed("Pont de l'\xC3\xA9t\xC3\xA9", kMisEventNameChars, why));
		TEST_EXPECT(mis_name_allowed(std::string(kMisDescriptionChars, 'd'), kMisDescriptionChars, why));
		TEST_EXPECT(!mis_name_allowed(std::string(kMisEventNameChars + 1, 'e'), kMisEventNameChars, why));
		TEST_EXPECT(why.find("59 bytes") != std::string::npos);
		TEST_EXPECT(!mis_name_allowed("north; south", kMisEventNameChars, why));
		TEST_EXPECT(!mis_name_allowed("say \"hi\"", kMisEventNameChars, why));
		TEST_EXPECT(!mis_name_allowed("two\nlines", kMisEventNameChars, why));
	}

	// --- What the reader does not keep is reported: a key the original editor's reader refuses in
	// its section (the old group keys among them), and the original's teleport_target and wind
	// sections, walked; its wav_list read into the table after the groups (D-MIS-13). Keys and
	// section names read without case, as both originals match them; the original's aliases
	// (noloop, alignment, perception, perfectionist) read as its words. -------------
	{
		const std::string text =
		    "BEGIN Group 2\r\n  Description \"Bravo\"\r\n  flags 1\r\n  value 2\r\n  ATTRIB 1\r\n  color 3\r\nEND GROUP\r\n"
		    "begin waypoint 4\r\n  description \"\"\r\n  noloop 1\r\n  speed 3\r\n  bogus 1\r\nend waypoint\r\n"
		    "begin areatrig 5\r\n  description \"\"\r\n  color 2\r\n  bounds 1\r\nend areatrig\r\n"
		    "begin layer 1\r\n  description \"one\"\r\n  color \"0\"\r\n  bogus 1\r\nend layer\r\n"
		    "begin teleport_target 1\r\n  description \"there\"\r\nend teleport_target\r\n"
		    "begin wav_list 1\r\n  description \"d\"\r\n  file_name \"f.wav\"\r\nend wav_list\r\n"
		    "begin wind 1\r\n  name \"w\"\r\nend wind\r\n"
		    "begin event 0\r\n  name \"e\"\r\n  bogus 1\r\nend event\r\n"
		    "begin item 0\r\n  type_id 1\r\n  id 1\r\n  alignment 2\r\n  perception 70\r\n  perfectionist 60\r\n"
		    "  formation 1\r\n  bogus 1\r\nend item\r\n";
		bms::File read;
		std::vector<MisIssue> issues;
		TEST_EXPECT(parse_mis_text(text, {}, read, error, &issues));
		TEST_EXPECT(read.group_records[2].flags == 1 && read.group_records[2].value == 0 &&
		            bms::group_name(read.group_records[2]) == "Bravo");
		TEST_EXPECT(static_cast<uint32_t>(read.waypoint_records[4].flags) == 1);
		// An areatrig section of no 6013 marker makes no area (D-MIS-14); the wav list slot's file
		// name is read.
		TEST_EXPECT(read.area_triggers.empty());
		TEST_EXPECT(std::string(read.wav_list_records[1].file_name) == "f.wav");
		TEST_EXPECT(read.items.size() == 1 && read.items[0].team == 2 && read.items[0].perception2 == 70 &&
		            read.items[0].perfectionist2 == 60);
		// Reported: the group's flags and value, the waypoint's, area's, layer's, event's and
		// item's bogus key, and the two sections.
		std::vector<size_t> lines;
		for (const MisIssue &issue : issues) lines.push_back(issue.line);
		TEST_EXPECT((lines == std::vector<size_t>{ 3, 4, 12, 17, 22, 24, 31, 36, 45 }));
		TEST_EXPECT(issues[0].message.find("group 2's key 'flags'") != std::string::npos);
		TEST_EXPECT(issues[5].message.find("teleport_target") != std::string::npos);
		MisNames names;
		TEST_EXPECT(parse_mis_names(text, names, error));
		TEST_EXPECT(names.events.size() == 1 && names.events[0] == "e");
	}

	// The original editor's terrain height (mis_terrain_height, JOTACmed's sub_4226D0): the four texels about the
	// point blended in 16-bit fractions, each twice its depth word shifted to 16.16, times polytrn_scale / 128; the
	// sector cell picking the atlas's quadrant; a cell naming no sector 0; a cell off the grid clamped to its edge,
	// or wrapped.
	{
		std::vector<uint16_t> atlas(1024 * 1024, 0);
		int sectors[16 * 16] = {};
		const auto texel = [&atlas](int row, int column) -> uint16_t & {
			return atlas[static_cast<size_t>(row) * 1024 + static_cast<size_t>(column)];
		};
		texel(2, 3) = 100;
		texel(2, 4) = 200;
		texel(3, 3) = 300;
		texel(3, 4) = 400;
		texel(514, 3) = 1000;
		texel(2, 511) = 50;
		sectors[0] = 1; // cell (0, 0): the atlas's first quadrant
		sectors[1] = 2; // cell (1, 0): its second 512 rows
		MisTerrain terrain;
		TEST_EXPECT(mis_terrain_height(terrain, 0, 0) == 0); // no terrain
		terrain.heights = atlas.data();
		terrain.sectors = sectors;
		// x 3.5, y -2.25 (2.25 south): row 2, column 3, a quarter down and half across; 0.375 * 100 + 0.375 * 200 +
		// 0.125 * 300 + 0.125 * 400 = 200 words, a word 1/256 unit: 51200 in 16.16.
		const int32_t x = bms::to_fixed_16_16(3.5), y = bms::to_fixed_16_16(-2.25);
		TEST_EXPECT(mis_terrain_height(terrain, x, y) == 51200);
		terrain.scale = 256;
		TEST_EXPECT(mis_terrain_height(terrain, x, y) == 102400);
		terrain.scale = 128;
		// On a texel: its own height. In cell (1, 0), sector 2: the second 512 rows.
		TEST_EXPECT(mis_terrain_height(terrain, bms::to_fixed_16_16(4.0), bms::to_fixed_16_16(-3.0)) == 400 * 256);
		TEST_EXPECT(mis_terrain_height(terrain, bms::to_fixed_16_16(515.0), bms::to_fixed_16_16(-2.0)) == 1000 * 256);
		// A cell naming no sector: 0.
		TEST_EXPECT(mis_terrain_height(terrain, bms::to_fixed_16_16(1030.0), y) == 0);
		// West of the grid: clamped to its first column's cell (the point's own place in the quadrant, column 511),
		// unless the axis wraps, to its last column's cell, which names none.
		TEST_EXPECT(mis_terrain_height(terrain, bms::to_fixed_16_16(-1.0), bms::to_fixed_16_16(-2.0)) == 50 * 256);
		terrain.wrap_x = true;
		TEST_EXPECT(mis_terrain_height(terrain, bms::to_fixed_16_16(-1.0), bms::to_fixed_16_16(-2.0)) == 0);
	}

	// The default kit (D-MIS-19): the loadout chunk the original editor's .bms export builds from it over its weapon
	// table, and the kit a loadout leads with, which the .mis carries [orig: JOTACmed.exe MIS_BuildDefaultLoadoutChunk @ 0x4328a0;
	// MIS_ParseToken @ 0x451caa..0x451d1c; MIS_SaveDocument @ 0x454ba7..0x454bcb].
	{
		const MisWeaponTable table = {{"WPN_RIFLE", 2}, {"WPN_RIFLE_A", 0}, {"WPN_RIFLE_B", 0}, {"WPN_PISTOL", 0},
		                              {"WPN_AT", 0}};
		const auto names_of = [](const bms::WeaponLoadout &loadout) {
			std::vector<std::string> out;
			for (const bms::WeaponLoadoutRecord &record : loadout.entries) out.push_back(record.name);
			return out;
		};
		const auto with_tail = [](std::vector<std::string> names) {
			for (const char *name : kMisLoadoutTail) names.push_back(name);
			return names;
		};
		// Each slot's name (as the kit spells it: the row is found case aside), its row's subclass rows, the tail.
		const MisDefaultKit kit = {"wpn_rifle", "WPN_PISTOL", "WPN_AT"};
		const bms::WeaponLoadout built = mis_default_loadout(kit, &table);
		TEST_EXPECT(names_of(built) == with_tail({"wpn_rifle", "WPN_RIFLE_A", "WPN_RIFLE_B", "WPN_PISTOL", "WPN_AT"}));
		bool minus_ones = true;
		for (const bms::WeaponLoadoutRecord &record : built.entries)
			minus_ones = minus_ones && record.ammo_primary == "-1" && record.ammo_secondary == "-1" &&
			             record.flags == "-1" && record.has_flags;
		TEST_EXPECT(minus_ones);
		TEST_EXPECT(mis_default_kit(built, &table) == kit);
		// A name no row holds is left out, an empty slot too; with no table each name stands alone.
		TEST_EXPECT(names_of(mis_default_loadout({"WPN_NONE", "WPN_PISTOL", ""}, &table)) == with_tail({"WPN_PISTOL"}));
		TEST_EXPECT(names_of(mis_default_loadout(kit, nullptr)) == with_tail({"wpn_rifle", "WPN_PISTOL", "WPN_AT"}));
		TEST_EXPECT(mis_default_kit(built, nullptr) == MisDefaultKit({"wpn_rifle", "WPN_RIFLE_A", "WPN_RIFLE_B"}));
		// A loadout short of three leaves the slots past it empty; one with no tail is read whole.
		bms::WeaponLoadout short_one;
		short_one.entries = {built.entries[3]};
		TEST_EXPECT(mis_default_kit(short_one, &table) == MisDefaultKit({"WPN_PISTOL", "", ""}));
		TEST_EXPECT(mis_default_kit(bms::WeaponLoadout(), &table) == MisDefaultKit());

		// The .mis carries it: written from the file's loadout, read back into the chunk the export builds.
		bms::File kitted = authored;
		kitted.loadout = built;
		sync_counts(kitted);
		std::string kit_text;
		TEST_EXPECT(write_mis_text(kitted, kit_text, error, nullptr, nullptr, &table));
		TEST_EXPECT(kit_text.find("  default_primary   wpn_rifle\r\n  default_secondary WPN_PISTOL\r\n"
		                          "  default_accessory WPN_AT\r\n") != std::string::npos);
		bms::File kit_back;
		TEST_EXPECT(parse_mis_text(kit_text, {}, kit_back, error, nullptr, &table));
		TEST_EXPECT(names_of(kit_back.loadout) == names_of(built));
		// An empty slot is written as the original writes one, its key alone, and reads back empty.
		kitted.loadout = short_one;
		TEST_EXPECT(write_mis_text(kitted, kit_text, error, nullptr, nullptr, &table));
		TEST_EXPECT(kit_text.find("  default_primary   WPN_PISTOL\r\n  default_secondary \r\n  default_accessory \r\n") !=
		            std::string::npos);
		TEST_EXPECT(parse_mis_text(kit_text, {}, kit_back, error, nullptr, &table));
		TEST_EXPECT(names_of(kit_back.loadout) == with_tail({"WPN_PISTOL"}));
		// A .mis naming no slot reads as the original's reset kit [orig: JOTACmed.exe MissionDocument_Reset
		// @ 0x44f86f..0x44f8dd].
		std::string unnamed = kit_text;
		for (const std::string key : {"  default_primary   WPN_PISTOL\r\n", "  default_secondary \r\n",
		                              "  default_accessory \r\n"}) {
			const size_t at = unnamed.find(key);
			TEST_EXPECT(at != std::string::npos);
			if (at != std::string::npos) unnamed.erase(at, key.size());
		}
		TEST_EXPECT(parse_mis_text(unnamed, {}, kit_back, error, nullptr, nullptr));
		TEST_EXPECT(names_of(kit_back.loadout) == with_tail({"WPN_M4AUTO", "WPN_colt45", "WPN_AT4"}));
	}

	return 0;
}
