#include "mission_mis.h"

// Emits the .mis text form of a mission. Section order and field spelling are the
// contract; see mission_mis_parser.cpp for the inverse.

#include "mission_detail.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission::detail {

namespace {

// A name as its line carries it: at most `width` bytes (kMisEventNameChars / kMisDescriptionChars, the original
// editor's dialogs), cut before a UTF-8 character the width would split, through mis_string. A name
// mis_name_allowed takes is written as it stands.
std::string mis_name(const std::string &name, size_t width) {
	size_t cut = std::min(name.size(), width);
	while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
	return mis_string(name.substr(0, cut));
}

// A record's name from `names`' list, "" past it or with no names, as its line carries it.
std::string listed_name(const std::vector<std::string> *list, size_t index, size_t width) {
	return list != nullptr && index < list->size() ? mis_name((*list)[index], width) : std::string();
}

// What the original editor's rebuilds count before it writes the path and group sections, over its items
// whatever their kind: a path's stops, the waypoint items (type 6005) whose waypoint_id is the path [orig:
// JOTACmed.exe sub_44CFD0 @0x44cfd0], and a group's members, the items whose group_id is the group [orig:
// sub_44D160 @0x44d160]. One pass over the four pools.
struct HeldCounts {
	std::array<size_t, 256> stops{};
	std::array<size_t, 256> members{};
};
HeldCounts held_counts(const bms::File &file) {
	HeldCounts held;
	for (const std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &entity : *pool) {
			if (entity.type_id == def::DEF_TYPE_WAYPOINT) ++held.stops[entity.waypoint_id];
			++held.members[entity.group_id];
		}
	return held;
}

void write_mis_general_information(const bms::File &file, std::string &out) {
	const bms::Header &header = file.header;
	const uint8_t *raw = header_bytes(header);
	const size_t item_total = file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size();

	append_line(out, "begin general_information");
	append_kv(out, "  file_version ", static_cast<int>(static_cast<uint8_t>(header.magic[3])));
	append_kv(out, "  name \"", mis_string(fixed_string(header.mission_name, sizeof(header.mission_name))), "\"");
	append_kv(out, "  designer \"", mis_string(fixed_string(header.designer, sizeof(header.designer))), "\"");
	append_kv(out, "  terrain ", fixed_string(header.terrain, 16));
	append_kv(out, "  cnv_file ", fixed_string(header.terrain + 16, 16));
	append_kv(out, "  tt_file ", fixed_string(header.terrain + 32, 16));
	append_kv(out, "  terrain_color ", static_cast<int32_t>(header.climate));
	append_kv(out, "  attrib ", static_cast<uint32_t>(header.attrib_flags));
	append_kv(out, "  visible_if ", read_u32_at(header, 140));
	append_kv(out, "  notvisible_if ", read_u32_at(header, 144));
	append_kv(out, "  arty ", read_u32_at(header, 148));
	append_kv(out, "  water_level ", read_u32_at(header, 152));
	append_kv(out, "  water_color ", packed_rgb(header.water_color));
	append_kv(out, "  murk ", static_cast<uint32_t>(header.murk));
	append_kv(out, "  fog_level ", read_u32_at(header, 156));
	append_kv(out, "  fog_color ", read_u32_at(header, 160));
	append_kv(out, "  weather_type ", static_cast<int32_t>(header.weather_type));
	append_kv(out, "  lowest_elev ", 0);
	append_kv(out, "  num_items ", item_total);
	append_kv(out, "  num_events ", file.events.size());
	append_kv(out, "  sunset ", fixed_string(header.environment, sizeof(header.environment)));
	// start_time keeps the HHMM clock zero-padding ("0500"); minutes_per_day is a plain count and
	// is written unpadded — the original importer parses every numeric base-10 (atol) so padding is
	// only cosmetic there, but a zero-padded count ("0120") fed back through a base-0 strtol reader
	// was the octal-corruption repro [orig: j__atol callers throughout MisLdr_ParseMisLine
	// @ 0x100017b0, misldr.dll]. See docs/mission/mis-format-re.md (D-MIS-5).
	append_kv(out, "  start_time ", four_digit(header_time_to_hhmm(header.start_time)));
	append_kv(out, "  minutes_per_day ", header.minutes_per_day);
	append_kv(out, "  viewx ", read_i32_at(raw, 588));
	append_kv(out, "  viewy ", read_i32_at(raw, 592));
	append_kv(out, "  viewz ", read_i32_at(raw, 596));
	append_kv(out, "  viewzoom 0.000000");
	append_kv(out, "  gen_def_val1 ", read_u32_at(header, 264));
	append_kv(out, "  gen_def_val2 ", read_u32_at(header, 268));
	append_kv(out, "  gen_def_val3 ", read_u32_at(header, 272));
	append_kv(out, "  gen_def_val4 ", read_u32_at(header, 276));
	append_kv(out, "  hardwin ", static_cast<int>(raw[262]));
	append_kv(out, "  hardlose ", static_cast<int>(raw[263]));
	append_kv(out, "  subgoals_win ",
	          static_cast<int>(header.win_conditions[0]), " ", static_cast<int>(header.win_conditions[1]), " ",
	          static_cast<int>(header.win_conditions[2]), " ", static_cast<int>(header.win_conditions[3]), " ",
	          static_cast<int>(header.win_conditions[4]), " ", static_cast<int>(header.win_conditions[5]), " ",
	          static_cast<int>(header.win_conditions[6]), " ", static_cast<int>(header.win_conditions[7]));
	append_kv(out, "  subgoals_lose ",
	          static_cast<int>(header.lose_conditions[0]), " ", static_cast<int>(header.lose_conditions[1]), " ",
	          static_cast<int>(header.lose_conditions[2]), " ", static_cast<int>(header.lose_conditions[3]), " ",
	          static_cast<int>(header.lose_conditions[4]), " ", static_cast<int>(header.lose_conditions[5]), " ",
	          static_cast<int>(header.lose_conditions[6]), " ", static_cast<int>(header.lose_conditions[7]));
	append_kv(out, "  win_scores ",
	          static_cast<int>(raw[556]) * 100, " ", static_cast<int>(raw[557]) * 100, " ",
	          static_cast<int>(raw[558]) * 100, " ", static_cast<int>(raw[559]) * 100, " ",
	          static_cast<int>(raw[560]) * 100, " ", static_cast<int>(raw[561]) * 100, " ",
	          static_cast<int>(raw[562]) * 100, " ", static_cast<int>(raw[563]) * 100);
	append_kv(out, "  lose_scores ",
	          static_cast<int>(raw[564]) * 100, " ", static_cast<int>(raw[565]) * 100, " ",
	          static_cast<int>(raw[566]) * 100, " ", static_cast<int>(raw[567]) * 100, " ",
	          static_cast<int>(raw[568]) * 100, " ", static_cast<int>(raw[569]) * 100, " ",
	          static_cast<int>(raw[570]) * 100, " ", static_cast<int>(raw[571]) * 100);
	append_kv(out, "  terrain_tile_tga ", fixed_string(header.terrain_tile, sizeof(header.terrain_tile)));
	append_kv(out, "  wind ", header.wind_speed, " ", header.wind_direction);
	append_line(out, "  wp_names_blue 0 0 0 0 0 0 0 0");
	append_line(out, "  wp_names_red 0 0 0 0 0 0 0 0");
	append_kv(out, "  player_type ", static_cast<int32_t>(header.mission_type));
	append_kv(out, "  max_saves ", static_cast<int>(header.max_saves));
	append_kv(out, "  map_zoom ", header.map_zoom);
	append_kv(out, "  bonus_expiration ", header.bonus_expiration);
	append_line(out, "  default_primary   WPN_M16M203BURST");
	append_line(out, "  default_secondary WPN_colt45");
	append_line(out, "  default_accessory WPN_AT4");
	append_line(out, "end general_information");
	append_line(out);
}

void write_mis_weapon_availability(std::string &out) {
	append_line(out, "begin weapon_availability");
	append_line(out);
	append_line(out, "end weapon_availability");
	append_line(out);
}

void write_mis_briefing(const bms::File &file, std::string &out) {
	const std::string briefing = mis_string(fixed_string(file.header.mission_briefing, sizeof(file.header.mission_briefing)));
	if (briefing.empty()) {
		return;
	}
	append_line(out, "begin briefing");
	append_line(out);
	for (size_t offset = 0; offset < briefing.size(); offset += 100) {
		append_kv(out, "  brief \"", briefing.substr(offset, std::min<size_t>(100, briefing.size() - offset)), "\"");
	}
	append_line(out, "endbriefing");
	append_line(out);
}

// The original editor keeps an area in the slot its zone id names and writes slots 1..99 in order: an area its
// type-6013 markers make (the slot's id word, which its rebuild sets from a marker's wpnumber of 1..99 alone),
// its description, then its color and its attrib, the area's flags (MISSION_AREA 1, constrain-Z 2: the area
// dialog's 1175 and 1169, the word its .bms writer stores at @28), each when not zero [orig: JOTACmed.exe
// MIS_SaveDocument @0x454630, the loop @0x454d11..0x454daf; sub_44D240 @0x44d240, the range @0x44d2a9;
// sub_40F360 @0x40f360; sub_44F920 @0x44f920, the area @0x450a16..0x450ab3]. So an area is written under its
// zone id, in id order, and the .bms holds no color. An area whose id is outside 1..99, or a second area of an
// id, has no slot in the original's .mis: the write is refused, naming it, rather than leaving it out. The
// original also writes slot i when path i has a description, testing the path table where it means the
// area's (@0x454d20..0x454d3f); this writer writes the areas there are (D-MIS-15).
bool write_mis_area_triggers(const bms::File &file, std::string &out, const MisNames *names, std::string &error) {
	std::array<const bms::AreaTrigger *, kMisAreaSlots> slots{};
	std::array<size_t, kMisAreaSlots> first{};
	for (size_t i = 0; i < file.area_triggers.size(); ++i) {
		const bms::AreaTrigger &area = file.area_triggers[i];
		if (area.id < 1 || area.id >= static_cast<int32_t>(kMisAreaSlots)) {
			error = "Area trigger " + std::to_string(i) + " has zone id " + std::to_string(area.id) +
			        ": the .mis holds an area under a zone id of 1..99 alone";
			return false;
		}
		const size_t id = static_cast<size_t>(area.id);
		if (slots[id] != nullptr) {
			error = "Area triggers " + std::to_string(first[id]) + " and " + std::to_string(i) + " share zone id " +
			        std::to_string(id) + ": the .mis holds one area under a zone id";
			return false;
		}
		slots[id] = &area;
		first[id] = i;
	}
	for (size_t id = 1; id < kMisAreaSlots; ++id) {
		const bms::AreaTrigger *area = slots[id];
		if (area == nullptr) continue;
		append_kv(out, "begin areatrig ", id);
		append_kv(out, "  description \"", names != nullptr ? mis_name(names->areas[id], kMisDescriptionChars) : std::string(),
		          "\"");
		if (area->flags != 0) append_kv(out, "  attrib ", area->flags);
		append_line(out, "end areatrig");
		append_line(out);
	}
	return true;
}

void write_mis_waypoints(const bms::File &file, const HeldCounts &held, std::string &out, const MisNames *names) {
	for (size_t i = 1; i < file.waypoint_records.size(); ++i) {
		const bms::WaypointRecord &record = file.waypoint_records[i];
		const std::string description =
		    names != nullptr && i < names->paths.size() ? mis_name(names->paths[i], kMisDescriptionChars) : std::string();
		// The original editor writes paths 1..127, each one a waypoint item (type 6005, any kind) carries in its
		// waypoint_id or one with a description, its color and its attrib each when not zero [orig: JOTACmed.exe
		// MIS_SaveDocument @0x454630, the loop @0x454dbe..0x454e5b; its rebuild sub_44CFD0 @0x44cfd0 counting
		// them]. Path MEMBERSHIP never travels in this section: the .mis carries it on the marker items and a
		// reader rebuilds the lists from them [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. So a
		// path with flags and no stop nor description has no section and its flags do not reach the .mis, as the
		// original's writer drops them (D-MIS-12); the sections a read text writes again are the same (D-MIS-5).
		if (held.stops[i] == 0 && description.empty()) {
			continue;
		}
		append_kv(out, "begin waypoint ", i);
		append_kv(out, "  description \"", description, "\"");
		if (static_cast<uint32_t>(record.flags) != 0) {
			append_kv(out, "  attrib ", static_cast<uint32_t>(record.flags));
		}
		append_line(out, "end waypoint");
		append_line(out);
	}
}

// The original editor writes groups 1..63 (group 0 is no group), each one an item's group_id names (any kind)
// or one with a description, then its attrib (the .bms record's flags @0), color and commander (the record's
// @8) each when not zero; its reader takes no other key in a group, refusing the whole file [orig: JOTACmed.exe
// MIS_SaveDocument @0x454630, the loop @0x454e6a..0x454f19; its count sub_44D160 @0x44d160; MIS_ParseToken
// @0x450e50, the group keys @0x453d08..0x453de4; sub_44F920 @0x44f920, the record @0x450940..0x4509b4]. A
// group's description is its name (the 20 bytes the game's loader drops, bms.h GroupRecord). The .bms holds no
// color. The original writes the commander line with the color's word (`mov ecx, [edi+104h]` @0x454eea, the
// commander being +108h), so its own save loses the commander; this writer writes the commander's (D-MIS-16).
void write_mis_groups(const bms::File &file, const HeldCounts &held, std::string &out) {
	for (size_t i = 1; i < file.group_records.size(); ++i) {
		const bms::GroupRecord &record = file.group_records[i];
		const std::string name = bms::group_name(record);
		if ((i >= held.members.size() || held.members[i] == 0) && name.empty()) {
			continue;
		}
		append_kv(out, "begin group ", i);
		append_kv(out, "  description \"", mis_name(name, kMisDescriptionChars), "\"");
		if (record.flags != 0) {
			append_kv(out, "  attrib ", record.flags);
		}
		if (record.value != 0) {
			append_kv(out, "  commander ", record.value);
		}
		append_line(out, "end group");
		append_line(out);
	}
}

void write_mis_layers(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.layer_records.size(); ++i) {
		const std::string description = fixed_string(file.layer_records[i].name, bms::kLayerRecordSize);
		if (description.empty()) {
			continue;
		}
		append_kv(out, "begin layer ", i);
		append_kv(out, "  description \"", mis_string(description), "\"");
		append_line(out, "end layer");
		append_line(out);
	}
}

void write_mis_events(const bms::File &file, std::string &out, const MisNames *names) {
	for (size_t i = 0; i < file.events.size(); ++i) {
		const bms::Event &event = file.events[i];
		append_kv(out, "begin event ", i);
		append_kv(out, "  name \"", listed_name(names ? &names->events : nullptr, i, kMisEventNameChars), "\"");
		if (static_cast<uint32_t>(event.flags) != 0) {
			append_kv(out, "  attrib ", static_cast<uint32_t>(event.flags));
		}
		if (event.reset_after != 0) {
			append_kv(out, "  reset_value ", event.reset_after);
		}
		if (event.delay != 0) {
			append_kv(out, "  delay_value ", event.delay);
		}
		if (event.trigger_count > 0) {
			append_kv(out, "  begin triggers ", static_cast<int>(event.trigger_count));
			for (int t = 0; t < static_cast<int>(event.trigger_count); ++t) {
				const int index = event.trigger_index + t;
				if (index < 0 || static_cast<size_t>(index) >= file.triggers.size()) {
					continue;
				}
				const bms::Trigger &trigger = file.triggers[static_cast<size_t>(index)];
				append_kv(out, "    ",
				          trigger.condition_flags, " ", static_cast<int32_t>(trigger.main_type), " ",
				          trigger.sub_type, " ", trigger.param1, " ", trigger.param2, " ",
				          trigger.param3, " ", trigger.param4, " ", trigger.unknown7);
			}
			append_line(out, "  end triggers");
		}
		if (event.action_count > 0) {
			append_kv(out, "  begin actions ", static_cast<int>(event.action_count));
			for (int a = 0; a < static_cast<int>(event.action_count); ++a) {
				const int index = event.action_index + a;
				if (index < 0 || static_cast<size_t>(index) >= file.actions.size()) {
					continue;
				}
				const bms::Action &action = file.actions[static_cast<size_t>(index)];
				append_kv(out, "    ",
				          action.reserved0, " ", static_cast<int32_t>(action.action_type), " ",
				          action.param1, " ", action.param2, " ", action.param3, " ",
				          action.param4, " ", action.action_sub_type, " ", action.reserved1);
			}
			append_line(out, "  end actions");
		}
		append_line(out, "end event");
		append_line(out);
	}
}

// `base_height` (optional) is an editor-sampled terrain height under the entity, 16.16 fixed-point.
// When provided it overrides entity.mis_extra_bheight in the emitted text; the entity's absolute
// z is written either way and height_lock declares it absolute, so the original editor recovers
// the terrain-relative offset as z - extra_bheight
// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll].
void write_mis_entity(const bms::Entity &entity, size_t index, std::string &out, const int32_t *base_height) {
	const uint16_t crouch_timer = combined_u16(entity.crouch_timer, entity.unk15a);
	const int32_t group_rel = combined_i32_from_i16(entity.group_rel_lo, entity.group_rel_hi);
	const std::string gen_string = fixed_string(entity.gen_string, sizeof(entity.gen_string));
	const int32_t extra_bheight = base_height != nullptr ? *base_height : entity.mis_extra_bheight;

	append_kv(out, "begin item ", index);
	append_kv(out, "  type_id ", entity.type_id);
	append_line(out, "  name \"\"");
	append_kv(out, "  iai_name \"", mis_string(fixed_string(entity.name1, sizeof(entity.name1))), "\"");
	append_kv(out, "  ai_textfile \"", mis_string(fixed_string(entity.name2, sizeof(entity.name2))), "\"");
	append_kv(out, "  id ", entity.id);
	append_kv(out, "  position ", entity.x, " ", entity.y, " ", entity.z);
	if (entity.yaw != 0 || entity.pitch != 0 || entity.roll != 0) {
		append_kv(out, "  facing ", entity.yaw, " ", entity.pitch, " ", entity.roll);
	}
	if (entity.team != 0) append_kv(out, "  team ", static_cast<int>(entity.team));
	if (entity.map_symbol != 0) append_kv(out, "  map_symbol ", static_cast<int>(entity.map_symbol));
	if (entity.name_index != 0) append_kv(out, "  name_index ", entity.name_index);
	if (entity.team_budget != 0) append_kv(out, "  team_budget ", static_cast<int>(entity.team_budget));
	if (entity.bmsi_attributes != 0) append_kv(out, "  bmsi_attributes ", entity.bmsi_attributes);
	if (entity.no_less_than != 0) append_kv(out, "  nolessthan ", static_cast<int>(entity.no_less_than));
	// nomorethan / movetimer / wp_adv_trigger emit UNCONDITIONALLY: their parse-time authoring
	// defaults are nonzero (no_more_than = 1, spawns = 1) or the old emit condition was not
	// value-symmetric (wp_adv_trigger omitted only at -1), so omitting the line let the parser's
	// default rewrite the field on every .mis round-trip (all 1365 entities on retail 00TRg gained
	// `nomorethan 1`; 1351 gained `movetimer 1`). The parser defaults stay — hand-authored sparse
	// files rely on them. See docs/mission/mis-format-re.md (D-MIS-5).
	append_kv(out, "  nomorethan ", static_cast<int>(entity.no_more_than));
	if (byte_from_i16(entity.weapon_types, 0) != 0) append_kv(out, "  weapon_type ", byte_from_i16(entity.weapon_types, 0));
	if (byte_from_i16(entity.weapon_types, 1) != 0) append_kv(out, "  sweapon_type ", byte_from_i16(entity.weapon_types, 1));
	if (entity.group_id != 0) append_kv(out, "  group_id ", static_cast<int>(entity.group_id));
	if (group_rel != 0) append_kv(out, "  group_rel ", group_rel);
	if (entity.waypoint_id != 0) append_kv(out, "  waypoint_id ", static_cast<int>(entity.waypoint_id));
	if (entity.wp_number != 0) append_kv(out, "  wpnumber ", entity.wp_number);
	append_kv(out, "  wpdistance ", entity.wp_distance);
	append_kv(out, "  wp_adv_trigger ", entity.wp_adv_trigger);
	for (int i = 0; i < 4; ++i) {
		const int32_t goal = byte_from_i32(entity.wp_goals, i);
		if (goal != 0) {
			append_kv(out, "  wpgoal", i, " ", goal);
		}
	}
	if (entity.ttool_index != 0) append_kv(out, "  ttoolindex ", entity.ttool_index);
	if (entity.alert_state != 0) append_kv(out, "  alert_state ", static_cast<int>(entity.alert_state));
	append_kv(out, "  obliqueness ", static_cast<int>(entity.obliqueness));
	append_kv(out, "  waccuracy1 ", entity.w_accuracy1);
	append_kv(out, "  waccuracy2 ", entity.w_accuracy2);
	append_kv(out, "  perception2 ", entity.perception2);
	append_kv(out, "  perfectionist2 ", entity.perfectionist2);
	append_kv(out, "  movetimer ", entity.spawns);
	append_kv(out, "  crouchtimer ", crouch_timer);
	append_kv(out, "  shoottimer ", entity.shoot_timer);
	append_kv(out, "  attention ", entity.attention);
	append_kv(out, "  advancetimer ", entity.advancetimer);
	append_kv(out, "  max_attack_distance ", entity.max_attack_distance);
	append_kv(out, "  edistances ", entity.min_engagement_distance, " ", entity.max_engagement_distance);
	if (entity.next_ssn != 0) append_kv(out, "  next_ssn ", entity.next_ssn);
	if (entity.color_override != 0) append_kv(out, "  color_override ", static_cast<int>(entity.color_override));
	if (entity.grenades != 0) append_kv(out, "  grenades ", static_cast<int>(entity.grenades));
	if (entity.mission_critical != 0) append_kv(out, "  mission_critical ", static_cast<int>(entity.mission_critical));
	if (entity.lfp_group != 0) append_kv(out, "  lfp_group ", static_cast<int>(entity.lfp_group));
	append_line(out, "  extra_mlink \"\"");
	append_line(out, "  extra_val1 0");
	append_line(out, "  extra_val2 0");
	append_line(out, "  extra_val3 0");
	append_line(out, "  extra_val4 0");
	append_line(out, "  extra_val5 0");
	append_line(out, "  extra_valmode 0");
	// Height declaration: the entity's z above is absolute for BMS-sourced documents, so
	// height_lock marks it ABSOLUTE and extra_bheight carries the baked base (terrain) height
	// under the item; the original editor recovers the terrain-relative offset as
	// z - extra_bheight. Without height_lock the editor reads z as terrain-relative and every
	// object floats by the local terrain height (the D-MIS-4 repro).
	// [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292, height_lock->rec+356);
	//  MisLdr_WriteNileProjectXml @ 0x10004930 (scene Y = z/65536 - (lock ? bheight/65536 : 0),
	//  <ABSOLUTE>TRUE</ABSOLUTE> iff height_lock); both misldr.dll]
	append_kv(out, "  extra_bheight ", extra_bheight);
	append_kv(out, "  height_lock ", static_cast<int>(entity.mis_height_lock));
	append_kv(out, "  gen_string \"", mis_string(gen_string.empty() ? std::string("null") : gen_string), "\"");
	append_line(out, "end item");
	append_line(out);
}

// `base_heights` (optional): editor-sampled terrain heights (16.16 fixed-point), FLAT and in WRITE
// ORDER — items, buildings, markers, organics — one per entity; entries beyond the vector fall
// back to the entity's own mis_extra_bheight.
void write_mis_items(const bms::File &file, std::string &out, const std::vector<int32_t> *base_heights) {
	size_t index = 0;
	const auto height_at = [base_heights](size_t i) -> const int32_t * {
		return (base_heights != nullptr && i < base_heights->size()) ? &(*base_heights)[i] : nullptr;
	};
	for (const bms::Entity &entity : file.items) { write_mis_entity(entity, index, out, height_at(index)); ++index; }
	for (const bms::Entity &entity : file.buildings) { write_mis_entity(entity, index, out, height_at(index)); ++index; }
	for (const bms::Entity &entity : file.markers) { write_mis_entity(entity, index, out, height_at(index)); ++index; }
	for (const bms::Entity &entity : file.organics) { write_mis_entity(entity, index, out, height_at(index)); ++index; }
}

// Starting size for the emitted text. A shipped mission lands in the tens of
// kilobytes, so one reserve up front saves a dozen reallocations while the
// sections append. Purely a growth hint — nothing reads it back, and it is NOT
// the networking 32768 that npwire/net_ports.h owns; same number, unrelated
// meaning, which is why it gets its own name here.
constexpr size_t kMisTextReserveBytes = 32u * 1024u;

} // namespace

bool write_mis_text(const bms::File &file, std::string &out, std::string &error,
                    const std::vector<int32_t> *base_heights, const MisNames *names) {
	out.clear();
	out.reserve(kMisTextReserveBytes);
	append_line(out, "// mission metafile");
	append_line(out);
	write_mis_general_information(file, out);
	write_mis_weapon_availability(out);
	write_mis_briefing(file, out);
	if (!write_mis_area_triggers(file, out, names, error)) {
		out.clear();
		return false;
	}
	const HeldCounts held = held_counts(file);
	write_mis_waypoints(file, held, out, names);
	write_mis_groups(file, held, out);
	write_mis_layers(file, out);
	write_mis_events(file, out, names);
	write_mis_items(file, out, base_heights);
	return true;
}

} // namespace opennova::mission::detail

namespace opennova::mission {

// [orig: JOTACmed.exe sub_4226D0 @ 0x4226d0, the original editor's terrain height, reached through sub_433690 @
// 0x433690 from its save's height-lock pass sub_44E640 @ 0x44e640]
int32_t mis_terrain_height(const MisTerrain &terrain, int32_t x, int32_t y) {
	if (terrain.heights == nullptr || terrain.sectors == nullptr) return 0;
	// The sector cell: x and the negated y in 512-unit steps less the origin, a cell off the grid clamped to its
	// edge where the axis does not wrap (the mask -16), else wrapped by the & 15 [@ 0x4226dc..0x42271e; the masks
	// polytrn_wrapx ? 0 : -16, sub_464BD0 @ 0x464dc9].
	const uint32_t south = 0u - static_cast<uint32_t>(y);
	int32_t cell_x = (x >> 25) - terrain.origin_x;
	int32_t cell_y = (static_cast<int32_t>(south) >> 25) - terrain.origin_y;
	if ((cell_x & (terrain.wrap_x ? 0 : -16)) != 0) cell_x = ~(cell_x >> 31);
	if ((cell_y & (terrain.wrap_y ? 0 : -16)) != 0) cell_y = ~(cell_y >> 31);
	const int32_t sector = terrain.sectors[(cell_y & 15) * 16 + (cell_x & 15)] - 1;
	if (sector < 0) return 0;
	// The point within its sector's quadrant of the atlas: bit 0 of the sector (less one) the atlas's second 512
	// rows, bit 1 its second 512 columns [@ 0x422733..0x42275E].
	uint32_t row_at = south & 0x01FFFFFFu, column_at = static_cast<uint32_t>(x) & 0x01FFFFFFu;
	if ((sector & 1) != 0) row_at += 0x02000000u;
	if ((sector & 2) != 0) column_at += 0x02000000u;
	const uint32_t row = (row_at >> 16) & 0x3FFu, column = (column_at >> 16) & 0x3FFu;
	const uint32_t next_row = ((row + 1u) << 10) & 0xFFC00u, next_column = (column + 1u) & 0x3FFu;
	// A texel as the original editor holds it, twice the depth word, shifted to 16.16 [@ 0x4227ba..0x422811].
	const auto texel = [&terrain](uint32_t index) { return (static_cast<uint32_t>(terrain.heights[index]) * 2u) << 7; };
	const uint32_t here = texel((row << 10) + column), east = texel((row << 10) + next_column),
	               south_of = texel(next_row + column), south_east = texel(next_row + next_column);
	// The bilinear weights in 16-bit fractions, and the sum in two halves of each texel, in 32-bit words
	// [@ 0x4227c5..0x42285d].
	const uint32_t down = row_at & 0xFFFFu, across = column_at & 0xFFFFu;
	const uint32_t both = (down * across) >> 16;
	const uint32_t w_here = both - down - across + 0x10000u, w_east = across - both, w_south = down - both;
	const uint32_t low = (east & 0xFFFFu) * w_east + (south_of & 0xFFFFu) * w_south + (here & 0xFFFFu) * w_here +
	                     (south_east & 0xFFFFu) * both;
	const uint32_t sum = (low >> 16) + (east >> 16) * w_east + (south_of >> 16) * w_south + (here >> 16) * w_here +
	                     (south_east >> 16) * both;
	// Times scale / 128 on the FPU, the sum read unsigned and the product truncated (the control word's chop); a
	// product past the word is the FPU's integer indefinite [@ 0x422865..0x42289f].
	const double height = static_cast<double>(sum) * (static_cast<double>(terrain.scale) * 0.0078125);
	if (!(height > -2147483649.0 && height < 2147483648.0)) return INT32_MIN;
	return static_cast<int32_t>(height);
}

} // namespace opennova::mission
