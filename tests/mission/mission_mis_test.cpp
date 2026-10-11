#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
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

	// --- .mis height declaration (D-MIS-4): height_lock + extra_bheight round-trip -----------
	// [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292, height_lock->rec+356);
	//  MisLdr_WriteNileProjectXml @ 0x10004930 (scene Y = z/65536 - (lock ? bheight/65536 : 0));
	//  both misldr.dll]
	TEST_EXPECT(text.find("  height_lock 1\r\n") != std::string::npos);
	TEST_EXPECT(text.find("  extra_bheight 0\r\n") != std::string::npos);
	TEST_EXPECT(parsed.items.size() == 1);
	TEST_EXPECT(parsed.items[0].mis_height_lock == 1);
	TEST_EXPECT(parsed.items[0].mis_extra_bheight == 0);

	// Editor-provided base heights (flat, in write order) bake into the emitted extra_bheight; the
	// absolute z is untouched and the source document's entity is not mutated by the writer.
	{
		const std::string baked_path = temp_path("mission_mis_baked_heights.mis");
		const std::vector<int32_t> base_heights = {25 * 65536};
		TEST_EXPECT(save_mis_file(authored, baked_path, error, &base_heights));
		const std::string baked_text = test_io::read_file_text(baked_path);
		TEST_EXPECT(baked_text.find("  extra_bheight 1638400\r\n") != std::string::npos);
		TEST_EXPECT(baked_text.find("  height_lock 1\r\n") != std::string::npos);
		TEST_EXPECT(authored.items[0].mis_extra_bheight == 0);

		bms::File baked;
		TEST_EXPECT(load_mis_file(baked_path, baked, error));
		TEST_EXPECT(baked.items.size() == 1);
		TEST_EXPECT(baked.items[0].mis_extra_bheight == 25 * 65536);
		TEST_EXPECT(baked.items[0].mis_height_lock == 1);
		TEST_EXPECT(baked.items[0].z == authored.items[0].z);
		// The parsed document re-exports its own baked value without a provider.
		std::string rewritten;
		TEST_EXPECT(write_mis_text(baked, rewritten, error));
		TEST_EXPECT(rewritten.find("  extra_bheight 1638400\r\n") != std::string::npos);
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
	// `description`; a named group is written even with no flags and no value. ---------------------
	{
		bms::File named;
		make_default(named);
		TEST_EXPECT(bms::set_group_name(named.group_records[5], "Bridge guards", error));
		named.group_records[6].flags = 1;
		TEST_EXPECT(bms::set_group_name(named.group_records[6], "Convoy", error));
		std::string named_text;
		TEST_EXPECT(write_mis_text(named, named_text, error));
		TEST_EXPECT(named_text.find("begin group 5\r\n  description \"Bridge guards\"\r\nend group\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin group 6\r\n  description \"Convoy\"\r\n  flags 1\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin group 4\r\n") == std::string::npos); // no name, flags or value
		bms::File named_back;
		TEST_EXPECT(parse_mis_text(named_text, {}, named_back, error));
		TEST_EXPECT(bms::group_name(named_back.group_records[5]) == "Bridge guards");
		TEST_EXPECT(bms::group_name(named_back.group_records[6]) == "Convoy" &&
		            named_back.group_records[6].flags == 1);
		TEST_EXPECT(bms::group_name(named_back.group_records[4]).empty());
		std::string named_again;
		TEST_EXPECT(write_mis_text(named_back, named_again, error));
		TEST_EXPECT(named_again == named_text);
		// A description that is no group name (past 20 characters, a control character) is skipped with an issue,
		// never stored as bytes; the group's flags still read.
		bms::File long_name;
		std::vector<MisIssue> issues;
		TEST_EXPECT(parse_mis_text("begin group 2\r\n  description \"The northern ridge sentries\"\r\n  flags 1\r\n"
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
	// records; a described path with no flags is written, its attrib left out. -----------------------
	{
		bms::File doc;
		make_default(doc);
		const size_t first = add_event(doc, MissionEventRecord{});
		const size_t second = add_event(doc, MissionEventRecord{});
		AreaTriggerRecord area;
		area.wp_number = 4; // the zone id
		area.max_x = 10.0f;
		area.max_y = 10.0f;
		add_area_trigger(doc, area);
		area.wp_number = 9;
		add_area_trigger(doc, area);
		doc.waypoint_records[3].flags = static_cast<bms::WaypointFlags>(1);
		sync_counts(doc);

		MisNames names;
		names.events = {"Get to the bridge", ""};
		names.areas = {"", "The bridge"};
		names.paths[3] = "Patrol \"north\"";
		names.paths[7] = "Escape route";
		std::string named_text;
		TEST_EXPECT(write_mis_text(doc, named_text, error, nullptr, &names));
		TEST_EXPECT(named_text.find("begin event " + std::to_string(first) + "\r\n  name \"Get to the bridge\"\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin event " + std::to_string(second) + "\r\n  name \"\"\r\n") != std::string::npos);
		TEST_EXPECT(named_text.find("begin areatrig 2\r\n  description \"The bridge\"\r\n") != std::string::npos);
		// A double quote cannot ride a quoted value: it is written as a single one.
		TEST_EXPECT(named_text.find("begin waypoint 3\r\n  description \"Patrol 'north'\"\r\n  attrib 1\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 7\r\n  description \"Escape route\"\r\nend waypoint\r\n") !=
		            std::string::npos);
		TEST_EXPECT(named_text.find("begin waypoint 8\r\n") == std::string::npos);

		MisNames back;
		TEST_EXPECT(parse_mis_names(named_text, back, error));
		TEST_EXPECT(back.events.size() == doc.events.size() && back.events[first] == "Get to the bridge" &&
		            back.events[second].empty());
		TEST_EXPECT(back.areas.size() == 2 && back.areas[0].empty() && back.areas[1] == "The bridge");
		TEST_EXPECT(back.paths[3] == "Patrol 'north'" && back.paths[7] == "Escape route" && back.paths[8].empty());
		// The records parse as before beside them: path 7 without flags, path 3 with its attrib.
		bms::File records;
		TEST_EXPECT(parse_mis_text(named_text, {}, records, error));
		TEST_EXPECT(static_cast<uint32_t>(records.waypoint_records[7].flags) == 0 &&
		            static_cast<uint32_t>(records.waypoint_records[3].flags) == 1);
		TEST_EXPECT(records.events.size() == doc.events.size() && records.area_triggers.size() == 2);
		// Written again from what was read: the same text.
		sync_counts(records);
		std::string again;
		TEST_EXPECT(write_mis_text(records, again, error, nullptr, &back));
		TEST_EXPECT(again == named_text);
		// No names: the empty lines as before, and no section for a flag-less path.
		std::string unnamed;
		TEST_EXPECT(write_mis_text(doc, unnamed, error));
		TEST_EXPECT(unnamed.find("begin waypoint 7\r\n") == std::string::npos);
		TEST_EXPECT(unnamed.find("begin areatrig 2\r\n  description \"\"\r\n") != std::string::npos);
		MisNames none;
		TEST_EXPECT(parse_mis_names(unnamed, none, error));
		TEST_EXPECT(none.events.size() == doc.events.size() && none.events[first].empty());
		// What the stock reader cannot take is not written: a ';' (it cuts the line there, quoted or not) as a
		// ',', and a name past the reader's slot cut to it (an event's 63 characters, a description's 255).
		MisNames long_names;
		long_names.events = {std::string(70, 'e'), "north; south"};
		long_names.areas = {std::string(300, 'a')};
		long_names.paths[3] = std::string(256, 'p');
		std::string capped;
		TEST_EXPECT(write_mis_text(doc, capped, error, nullptr, &long_names));
		TEST_EXPECT(capped.find("  name \"" + std::string(63, 'e') + "\"\r\n") != std::string::npos);
		TEST_EXPECT(capped.find("  name \"north, south\"\r\n") != std::string::npos);
		TEST_EXPECT(capped.find("begin areatrig 1\r\n  description \"" + std::string(255, 'a') + "\"\r\n") !=
		            std::string::npos);
		TEST_EXPECT(capped.find("begin waypoint 3\r\n  description \"" + std::string(255, 'p') + "\"\r\n") !=
		            std::string::npos);
		TEST_EXPECT(capped.find(';') == std::string::npos);
		// An item's name and the mission's are not a record's: an unterminated section is refused.
		MisNames broken;
		TEST_EXPECT(!parse_mis_names("begin event 0\r\n  name \"x\"\r\n", broken, error));
	}

	return 0;
}
