#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "mission/bms.h"
#include "mission/mission.h"

namespace {

std::string temp_path(const char *name) {
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/build/test-output";
	std::filesystem::create_directories(dir);
	return dir + "/" + name;
}

std::string read_text(const std::string &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file.good()) {
		return {};
	}
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
	using namespace opennova::mission;
	namespace bms = opennova::bms;

	MissionDocument authored;
	authored.create_default();
	TEST_EXPECT(authored.set_header_string("mission_name", "MIS Roundtrip"));
	TEST_EXPECT(authored.set_header_string("designer", "OpenNova"));
	TEST_EXPECT(authored.set_header_string("terrain", "dvxi5"));
	TEST_EXPECT(authored.set_header_string("environment", "full_00"));
	TEST_EXPECT(authored.set_header_int("weather", 2));
	TEST_EXPECT(authored.set_header_int("minutes_per_day", 1440));
	TEST_EXPECT(authored.set_header_float("map_zoom", 2.5f));

	EntityTransform transform;
	transform.x = 12.0f;
	transform.y = -3.5f;
	transform.z = 44.25f;
	transform.pitch = 1;
	transform.yaw = 90;
	transform.roll = 3;
	EntityRecord added;
	TEST_EXPECT(authored.add_entity(EntityKind::Item, 101291, transform, &added));
	EntityProperties props;
	props.group_id = 7;
	props.waypoint_id = 2;
	props.wp_number = 13;
	props.team = 1;
	props.ai_flags = static_cast<int>(bms::BmsiAttributeFlags::Blind);
	props.perception = 80;
	props.accuracy = 60;
	props.alert_state = 4;
	props.min_engagement_distance = 30;
	props.max_engagement_distance = 300;
	props.max_attack_distance = 500;
	props.spawn_count = 6;
	props.max_simultaneous = 2;
	props.no_less_than = 1;
	props.map_symbol = 9;
	props.name1 = "rifle";
	props.name2 = "patrol";
	TEST_EXPECT(authored.set_entity_properties(EntityKind::Item, 0, props));

	MissionEventRecord event_seed;
	event_seed.flags = static_cast<int>(bms::EventFlags::ResetAfter);
	event_seed.reset_after = 9;
	event_seed.delay = 4;
	MissionEventRecord event;
	TEST_EXPECT(authored.add_event(event_seed, &event));
	MissionTriggerRecord trigger;
	trigger.condition_flags = 2;
	trigger.main_type = static_cast<int>(bms::TriggerMainType::MissionVariable);
	trigger.sub_type = static_cast<int>(bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
	trigger.param1 = 3;
	trigger.param2 = 10;
	TEST_EXPECT(authored.insert_event_trigger(event.index, 0, trigger));
	MissionActionRecord action;
	action.action_type = static_cast<int>(bms::ActionType::MisvarChange);
	action.action_sub_type = static_cast<int>(bms::MissionVariableActionSubType::Set);
	action.param1 = 4;
	action.param2 = 12;
	TEST_EXPECT(authored.insert_event_action(event.index, 0, action));

	const std::string mis_path = temp_path("mission_mis_roundtrip.mis");
	TEST_EXPECT(authored.save_mis_file(mis_path));
	TEST_EXPECT(authored.source_path() == mis_path);
	const std::string text = read_text(mis_path);
	TEST_EXPECT(text.find("// mission metafile\r\n") == 0);
	TEST_EXPECT(text.find("begin general_information\r\n") != std::string::npos);
	TEST_EXPECT(text.find("begin event 0\r\n") != std::string::npos);
	TEST_EXPECT(text.find("begin item 0\r\n") != std::string::npos);

	MissionDocument parsed;
	TEST_EXPECT(parsed.load_mis_file(mis_path));
	TEST_EXPECT(parsed.source_path() == mis_path);
	TEST_EXPECT(parsed.info().mission_name == "MIS Roundtrip");
	TEST_EXPECT(parsed.info().designer == "OpenNova");
	TEST_EXPECT(parsed.info().terrain == "dvxi5");
	TEST_EXPECT(parsed.info().environment == "full_00");
	TEST_EXPECT(parsed.info().weather == 2);
	TEST_EXPECT(parsed.info().minutes_per_day == 1440);
	TEST_EXPECT(parsed.info().map_zoom == 2.5f);
	TEST_EXPECT(parsed.entity_count(EntityKind::Item) == 1);
	TEST_EXPECT(parsed.entity_count(EntityKind::Building) == 0);
	TEST_EXPECT(parsed.entity_count(EntityKind::Marker) == 0);
	TEST_EXPECT(parsed.entity_count(EntityKind::Organic) == 0);
	EntityRecord parsed_entity;
	TEST_EXPECT(parsed.get_entity(EntityKind::Item, 0, parsed_entity));
	TEST_EXPECT(parsed_entity.item_id == 101291);
	TEST_EXPECT(parsed_entity.transform.x == 12.0f);
	TEST_EXPECT(parsed_entity.transform.y == -3.5f);
	TEST_EXPECT(parsed_entity.transform.z == 44.25f);
	TEST_EXPECT(parsed_entity.transform.yaw == 90);
	TEST_EXPECT(parsed_entity.group_id == 7);
	TEST_EXPECT(parsed_entity.waypoint_id == 2);
	TEST_EXPECT(parsed_entity.wp_number == 13);
	TEST_EXPECT(parsed_entity.team == 1);
	TEST_EXPECT(parsed_entity.ai_flags == static_cast<int>(bms::BmsiAttributeFlags::Blind));
	TEST_EXPECT(parsed_entity.perception == 80);
	TEST_EXPECT(parsed_entity.accuracy == 60);
	TEST_EXPECT(parsed_entity.alert_state == 4);
	TEST_EXPECT(parsed_entity.min_engagement_distance == 30);
	TEST_EXPECT(parsed_entity.max_engagement_distance == 300);
	TEST_EXPECT(parsed_entity.max_attack_distance == 500);
	TEST_EXPECT(parsed_entity.spawn_count == 6);
	TEST_EXPECT(parsed_entity.max_simultaneous == 2);
	TEST_EXPECT(parsed_entity.no_less_than == 1);
	TEST_EXPECT(parsed_entity.map_symbol == 9);
	TEST_EXPECT(parsed_entity.name1 == "rifle");
	TEST_EXPECT(parsed_entity.name2 == "patrol");
	TEST_EXPECT(parsed.event_count() == 1);
	TEST_EXPECT(parsed.trigger_count() == 1);
	TEST_EXPECT(parsed.action_count() == 1);
	MissionEventChain chain;
	TEST_EXPECT(parsed.get_event_chain(0, chain));
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
	TEST_EXPECT(parsed.save_bms_file(bms_path));
	MissionDocument bms_reload;
	TEST_EXPECT(bms_reload.load_bms_file(bms_path));
	TEST_EXPECT(bms_reload.info().mission_name == "MIS Roundtrip");
	TEST_EXPECT(bms_reload.entity_count(EntityKind::Item) == 1);
	TEST_EXPECT(bms_reload.event_count() == 1);

	// --- .mis height declaration (D-MIS-4): height_lock + extra_bheight round-trip -----------
	// [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292, height_lock->rec+356);
	//  MisLdr_WriteNileProjectXml @ 0x10004930 (scene Y = z/65536 - (lock ? bheight/65536 : 0));
	//  both misldr.dll]
	TEST_EXPECT(text.find("  height_lock 1\r\n") != std::string::npos);
	TEST_EXPECT(text.find("  extra_bheight 0\r\n") != std::string::npos);
	TEST_EXPECT(parsed.bms_file().items.size() == 1);
	TEST_EXPECT(parsed.bms_file().items[0].mis_height_lock == 1);
	TEST_EXPECT(parsed.bms_file().items[0].mis_extra_bheight == 0);

	// Editor-provided base heights (flat, in write order) bake into the emitted extra_bheight; the
	// absolute z is untouched and the source document's entity is not mutated by the writer.
	{
		const std::string baked_path = temp_path("mission_mis_baked_heights.mis");
		const std::vector<int32_t> base_heights = {25 * 65536};
		TEST_EXPECT(authored.save_mis_file(baked_path, &base_heights));
		const std::string baked_text = read_text(baked_path);
		TEST_EXPECT(baked_text.find("  extra_bheight 1638400\r\n") != std::string::npos);
		TEST_EXPECT(baked_text.find("  height_lock 1\r\n") != std::string::npos);
		TEST_EXPECT(authored.bms_file().items[0].mis_extra_bheight == 0);

		MissionDocument baked;
		TEST_EXPECT(baked.load_mis_file(baked_path));
		TEST_EXPECT(baked.bms_file().items.size() == 1);
		TEST_EXPECT(baked.bms_file().items[0].mis_extra_bheight == 25 * 65536);
		TEST_EXPECT(baked.bms_file().items[0].mis_height_lock == 1);
		TEST_EXPECT(baked.bms_file().items[0].z == authored.bms_file().items[0].z);
		// The parsed document re-exports its own baked value without a provider.
		std::string rewritten;
		TEST_EXPECT(baked.write_mis_text(rewritten));
		TEST_EXPECT(rewritten.find("  extra_bheight 1638400\r\n") != std::string::npos);
	}

	// --- Base-10 numeric parsing (D-MIS-5): the octal regression ------------------------------
	// minutes_per_day emits UNPADDED (the old four_digit padding produced "0120", which a base-0
	// strtol reader corrupted to octal 80, degrading to 0 by the third generation); a
	// hand-authored zero-padded value still parses base-10, matching the original importer's
	// plain atol [orig: j__atol callers throughout MisLdr_ParseMisLine @ 0x100017b0, misldr.dll].
	{
		MissionDocument octal;
		octal.create_default();
		TEST_EXPECT(octal.set_header_int("minutes_per_day", 120));
		std::string octal_text;
		TEST_EXPECT(octal.write_mis_text(octal_text));
		TEST_EXPECT(octal_text.find("  minutes_per_day 120\r\n") != std::string::npos);
		TEST_EXPECT(octal_text.find("  minutes_per_day 0120") == std::string::npos);

		MissionDocument hand;
		TEST_EXPECT(hand.load_mis_text(
				"begin general_information\r\n"
				"  minutes_per_day 0120\r\n"
				"end general_information\r\n"));
		TEST_EXPECT(hand.info().minutes_per_day == 120);
	}

	// --- Header write/read symmetry (D-MIS-5): fog_level / water_level / gen_def_val1..4 ------
	// fog_level / water_level are the RAW u32s at header offsets 156/152 (straddling the u16
	// override fields — the old parse truncated 45875200 to 0); gen_def_val1..4 are the u32s at
	// 264..276 (Header health/mana/music/reverb), previously write-only.
	{
		MissionDocument sym;
		sym.create_default();
		bms::Header &header = sym.bms_file().header;
		header.water_override = 21;  // water_level u32 @152 reads 21 while unknown1 stays 0
		header.fog_override = 700;   // fog_level u32 @156 reads 700 << 16 == 45875200
		header.health = 11;
		header.mana = 22;
		header.music = 33;
		header.reverb = 44;
		std::string sym_text;
		TEST_EXPECT(sym.write_mis_text(sym_text));
		TEST_EXPECT(sym_text.find("  water_level 21\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  fog_level 45875200\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val1 11\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val2 22\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val3 33\r\n") != std::string::npos);
		TEST_EXPECT(sym_text.find("  gen_def_val4 44\r\n") != std::string::npos);

		MissionDocument sym_back;
		TEST_EXPECT(sym_back.load_mis_text(sym_text));
		const bms::Header &back = sym_back.bms_file().header;
		TEST_EXPECT(back.water_override == 21);
		TEST_EXPECT(back.fog_override == 700);
		TEST_EXPECT(back.health == 11);
		TEST_EXPECT(back.mana == 22);
		TEST_EXPECT(back.music == 33);
		TEST_EXPECT(back.reverb == 44);
	}

	return 0;
}
