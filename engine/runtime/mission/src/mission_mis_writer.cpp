#include "mission_mis.h"

// Split out of mission.cpp (quality campaign W3-1). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Emits the .mis text form of a mission. Section order and field spelling are the
// contract; see mission_mis_parser.cpp for the inverse.

#include "mission_detail.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission::detail {

namespace {

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

void write_mis_area_triggers(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.area_triggers.size(); ++i) {
		append_kv(out, "begin areatrig ", i + 1);
		append_line(out, "  description \"\"");
		append_line(out, "end areatrig");
		append_line(out);
	}
}

void write_mis_waypoints(const bms::File &file, std::string &out) {
	for (size_t i = 1; i < file.waypoint_records.size(); ++i) {
		const bms::WaypointRecord &record = file.waypoint_records[i];
		// Emit a waypoint section only when it carries information (its flags). Path MEMBERSHIP
		// never travels in this section: the .mis text carries it on the marker items themselves
		// and the original importer rebuilds the lists from the type-6005 pair ids
		// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. A flag-less section body
		// would be just `description ""` — unrecoverable on re-parse (marker_count is not a .mis
		// concept), which broke .mis write->parse->write idempotency (D-MIS-5).
		if (static_cast<uint32_t>(record.flags) == 0) {
			continue;
		}
		append_kv(out, "begin waypoint ", i);
		append_line(out, "  description \"\"");
		append_kv(out, "  attrib ", static_cast<uint32_t>(record.flags));
		append_line(out, "end waypoint");
		append_line(out);
	}
}

void write_mis_groups(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.group_records.size(); ++i) {
		const bms::GroupRecord &record = file.group_records[i];
		if (record.flags == 0 && record.value == 0) {
			continue;
		}
		append_kv(out, "begin group ", i);
		append_line(out, "  description \"\"");
		if (record.flags != 0) {
			append_kv(out, "  flags ", record.flags);
		}
		if (record.value != 0) {
			append_kv(out, "  value ", record.value);
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

void write_mis_events(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.events.size(); ++i) {
		const bms::Event &event = file.events[i];
		append_kv(out, "begin event ", i);
		append_line(out, "  name \"\"");
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
                    const std::vector<int32_t> *base_heights) {
	(void)error;
	out.clear();
	out.reserve(kMisTextReserveBytes);
	append_line(out, "// mission metafile");
	append_line(out);
	write_mis_general_information(file, out);
	write_mis_weapon_availability(out);
	write_mis_briefing(file, out);
	write_mis_area_triggers(file, out);
	write_mis_waypoints(file, out);
	write_mis_groups(file, out);
	write_mis_layers(file, out);
	write_mis_events(file, out);
	write_mis_items(file, out, base_heights);
	return true;
}

} // namespace opennova::mission::detail
