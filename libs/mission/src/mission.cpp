#include "mission/mission.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>

namespace opennova::mission {

namespace {

std::string fixed_string(const char *data, size_t max_len) {
	size_t len = 0;
	while (len < max_len && data[len] != '\0') {
		++len;
	}
	return std::string(data, len);
}

std::string mis_string(std::string value) {
	for (char &ch : value) {
		if (ch == '\r' || ch == '\n') {
			ch = '|';
		} else if (ch == '"') {
			ch = '\'';
		}
	}
	return value;
}

void append_line(std::string &out, const std::string &line = std::string()) {
	out += line;
	out += "\r\n";
}

template <typename... Args>
void append_kv(std::string &out, Args &&...args) {
	std::ostringstream stream;
	(stream << ... << args);
	append_line(out, stream.str());
}

const uint8_t *header_bytes(const bms::Header &header) {
	return reinterpret_cast<const uint8_t *>(&header);
}

uint16_t read_u16_at(const bms::Header &header, size_t offset) {
	const uint8_t *bytes = header_bytes(header);
	return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t read_u32_at(const bms::Header &header, size_t offset) {
	const uint8_t *bytes = header_bytes(header);
	return static_cast<uint32_t>(bytes[offset]) |
	       (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
	       (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
	       (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

int32_t read_i32_at(const uint8_t *bytes, size_t offset) {
	return static_cast<int32_t>(static_cast<uint32_t>(bytes[offset]) |
	                            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
	                            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
	                            (static_cast<uint32_t>(bytes[offset + 3]) << 24));
}

void write_i32_at(uint8_t *bytes, size_t offset, int32_t value) {
	const uint32_t v = static_cast<uint32_t>(value);
	bytes[offset] = static_cast<uint8_t>(v & 0xFF);
	bytes[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
	bytes[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
	bytes[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

int header_time_to_hhmm(uint16_t encoded) {
	const int hours = (encoded >> 8) & 0xFF;
	const int frac = encoded & 0xFF;
	const int minutes = (frac * 60 + 128) / 256;
	return hours * 100 + minutes;
}

std::string four_digit(int value) {
	std::ostringstream stream;
	if (value < 0) {
		stream << value;
		return stream.str();
	}
	stream.width(4);
	stream.fill('0');
	stream << value;
	return stream.str();
}

uint32_t packed_rgb(const uint8_t rgb[3]) {
	return static_cast<uint32_t>(rgb[2]) |
	       (static_cast<uint32_t>(rgb[1]) << 8) |
	       (static_cast<uint32_t>(rgb[0]) << 16);
}

uint16_t combined_u16(uint8_t lo, uint8_t hi) {
	return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

int32_t combined_i32_from_i16(int16_t lo, int16_t hi) {
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(lo)) |
	                            (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16));
}

int32_t byte_from_i16(int16_t value, int index) {
	return static_cast<int32_t>((static_cast<uint16_t>(value) >> (index * 8)) & 0xFF);
}

int32_t byte_from_i32(int32_t value, int index) {
	return static_cast<int32_t>((static_cast<uint32_t>(value) >> (index * 8)) & 0xFF);
}

int item_id_to_bms_type_id(int item_id) {
	return item_id >= kItemIdOffset ? item_id - kItemIdOffset : item_id;
}

int bms_type_id_to_item_id(int type_id) {
	return type_id + kItemIdOffset;
}

bms::ItemType to_bms_type(EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return bms::ItemType::Marker;
		case EntityKind::Item: return bms::ItemType::Item;
		case EntityKind::Building: return bms::ItemType::Building;
		case EntityKind::Organic: return bms::ItemType::Organic;
	}
	return bms::ItemType::Item;
}

bool from_int_kind(int kind, EntityKind &out) {
	switch (kind) {
		case 0:
			out = EntityKind::Marker;
			return true;
		case 1:
			out = EntityKind::Item;
			return true;
		case 2:
			out = EntityKind::Building;
			return true;
		case 3:
			out = EntityKind::Organic;
			return true;
		default:
			return false;
	}
}

void resize_waypoint_padding(bms::WaypointRecord &record) {
	const size_t marker_count = std::min<size_t>(record.waypoint_numbers.size(), kMaxWaypointPathMarkers);
	if (record.waypoint_numbers.size() != marker_count) {
		record.waypoint_numbers.resize(marker_count);
	}
	record.marker_count = static_cast<uint32_t>(marker_count);
	const size_t used = marker_count * sizeof(uint32_t);
	record.padding.assign(128 - used, 0);
}

WaypointPath to_path(const bms::WaypointRecord &record, size_t index) {
	WaypointPath out;
	out.index = index;
	out.flags = static_cast<int>(record.flags);
	out.marker_indices.reserve(record.waypoint_numbers.size());
	for (uint32_t marker_index : record.waypoint_numbers) {
		out.marker_indices.push_back(static_cast<int>(marker_index));
	}
	return out;
}

bool validate_waypoint_path(const bms::File &file,
                            size_t index,
                            const std::vector<int> &marker_indices,
                            std::string &error) {
	if (index >= file.waypoint_records.size()) {
		error = "Waypoint path index out of range";
		return false;
	}
	if (marker_indices.size() > kMaxWaypointPathMarkers) {
		error = "Waypoint path marker count exceeds 32";
		return false;
	}
	for (int marker_index : marker_indices) {
		if (marker_index < 0 || static_cast<size_t>(marker_index) >= file.markers.size()) {
			error = "Waypoint path marker index out of range";
			return false;
		}
	}
	return true;
}

void apply_waypoint_path_to_record(bms::WaypointRecord &record, const std::vector<int> &marker_indices, int flags) {
	record.flags = static_cast<bms::WaypointFlags>(static_cast<uint32_t>(flags));
	record.waypoint_numbers.clear();
	record.waypoint_numbers.reserve(marker_indices.size());
	for (int marker_index : marker_indices) {
		record.waypoint_numbers.push_back(static_cast<uint32_t>(marker_index));
	}
	resize_waypoint_padding(record);
}

void repair_waypoint_marker_references(bms::File &file, size_t removed_index) {
	for (bms::WaypointRecord &record : file.waypoint_records) {
		bool changed = false;
		std::vector<uint32_t> repaired;
		repaired.reserve(record.waypoint_numbers.size());
		for (uint32_t marker_index : record.waypoint_numbers) {
			if (marker_index == removed_index) {
				changed = true;
				continue;
			}
			if (marker_index > removed_index) {
				repaired.push_back(marker_index - 1);
				changed = true;
			} else {
				repaired.push_back(marker_index);
			}
		}
		if (changed) {
			record.waypoint_numbers = repaired;
			resize_waypoint_padding(record);
		}
	}
}

std::string unknown_label(const char *prefix, int value) {
	return std::string(prefix) + "(" + std::to_string(value) + ")";
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
	append_kv(out, "  start_time ", four_digit(header_time_to_hhmm(header.start_time)));
	append_kv(out, "  minutes_per_day ", four_digit(header.minutes_per_day));
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
		if (static_cast<uint32_t>(record.flags) == 0 && record.marker_count == 0) {
			continue;
		}
		append_kv(out, "begin waypoint ", i);
		append_line(out, "  description \"\"");
		if (static_cast<uint32_t>(record.flags) != 0) {
			append_kv(out, "  attrib ", static_cast<uint32_t>(record.flags));
		}
		append_line(out, "end waypoint");
		append_line(out);
	}
}

void write_mis_groups(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.group_records.size(); ++i) {
		const bms::GroupRecord &record = file.group_records[i];
		const uint8_t *raw = record.raw_data;
		// [orig: Mission_LoadBMSFile @0x40fbbb keeps three ints per 32-byte record from offsets
		//  0, 8, 12 (temp_record[0], [2], [3]). Field meanings are NOT proven by the loader, so the
		//  keys are emitted as tentative field0/field8/field12 (was: 0/4/8 with guessed attrib/commander/color).]
		const int32_t field0 = read_i32_at(raw, 0);
		const int32_t field8 = read_i32_at(raw, 8);
		const int32_t field12 = read_i32_at(raw, 12);
		if (field0 == 0 && field8 == 0 && field12 == 0) {
			continue;
		}
		append_kv(out, "begin group ", i);
		append_line(out, "  description \"\"");
		if (field0 != 0) {
			append_kv(out, "  field0 ", field0);
		}
		if (field8 != 0) {
			append_kv(out, "  field8 ", field8);
		}
		if (field12 != 0) {
			append_kv(out, "  field12 ", field12);
		}
		append_line(out, "end group");
		append_line(out);
	}
}

void write_mis_layers(const bms::File &file, std::string &out) {
	for (size_t i = 0; i < file.layer_records.size(); ++i) {
		const std::string description = fixed_string(reinterpret_cast<const char *>(file.layer_records[i].raw_data),
		                                             bms::kLayerRecordSize);
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

void write_mis_entity(const bms::Entity &entity, size_t index, std::string &out) {
	const uint16_t crouch_timer = combined_u16(entity.crouch_timer, entity.unk15a);
	const int32_t group_rel = combined_i32_from_i16(entity.unk25, entity.unk26);
	const uint8_t *gen_raw = reinterpret_cast<const uint8_t *>(entity.gen_string);
	const std::string gen_string = fixed_string(entity.gen_string, 32);

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
	if (entity.no_more_than != 0) append_kv(out, "  nomorethan ", static_cast<int>(entity.no_more_than));
	if (byte_from_i16(entity.unk19, 0) != 0) append_kv(out, "  weapon_type ", byte_from_i16(entity.unk19, 0));
	if (byte_from_i16(entity.unk19, 1) != 0) append_kv(out, "  sweapon_type ", byte_from_i16(entity.unk19, 1));
	if (entity.group_id != 0) append_kv(out, "  group_id ", static_cast<int>(entity.group_id));
	if (group_rel != 0) append_kv(out, "  group_rel ", group_rel);
	if (entity.waypoint_id != 0) append_kv(out, "  waypoint_id ", static_cast<int>(entity.waypoint_id));
	if (entity.wp_number != 0) append_kv(out, "  wpnumber ", entity.wp_number);
	append_kv(out, "  wpdistance ", entity.wp_distance);
	if (entity.wp_adv_trigger != -1) append_kv(out, "  wp_adv_trigger ", entity.wp_adv_trigger);
	for (int i = 0; i < 4; ++i) {
		const int32_t goal = byte_from_i32(entity.unk30_31, i);
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
	if (entity.spawns > 0) append_kv(out, "  movetimer ", entity.spawns);
	append_kv(out, "  crouchtimer ", crouch_timer);
	append_kv(out, "  shoottimer ", entity.shoot_timer);
	append_kv(out, "  attention ", entity.attention);
	append_kv(out, "  advancetimer ", entity.fire_timer);
	append_kv(out, "  max_attack_distance ", entity.max_attack_distance);
	append_kv(out, "  edistances ", entity.min_engagement_distance, " ", entity.max_engagement_distance);
	if (entity.unk41 != 0) append_kv(out, "  next_ssn ", entity.unk41);
	if (entity.color_override != 0) append_kv(out, "  color_override ", static_cast<int>(entity.color_override));
	if (gen_raw[32] != 0) append_kv(out, "  grenades ", static_cast<int>(gen_raw[32]));
	if (gen_raw[34] != 0) append_kv(out, "  mission_critical ", static_cast<int>(gen_raw[34]));
	if (gen_raw[35] != 0) append_kv(out, "  lfp_group ", static_cast<int>(gen_raw[35]));
	append_line(out, "  extra_mlink \"\"");
	append_line(out, "  extra_val1 0");
	append_line(out, "  extra_val2 0");
	append_line(out, "  extra_val3 0");
	append_line(out, "  extra_val4 0");
	append_line(out, "  extra_val5 0");
	append_line(out, "  extra_valmode 0");
	append_line(out, "  extra_bheight 0");
	append_kv(out, "  gen_string \"", mis_string(gen_string.empty() ? std::string("null") : gen_string), "\"");
	append_line(out, "end item");
	append_line(out);
}

void write_mis_items(const bms::File &file, std::string &out) {
	size_t index = 0;
	for (const bms::Entity &entity : file.items) write_mis_entity(entity, index++, out);
	for (const bms::Entity &entity : file.buildings) write_mis_entity(entity, index++, out);
	for (const bms::Entity &entity : file.markers) write_mis_entity(entity, index++, out);
	for (const bms::Entity &entity : file.organics) write_mis_entity(entity, index++, out);
}

bool write_mis_text(const bms::File &file, std::string &out, std::string &error) {
	(void)error;
	out.clear();
	out.reserve(32768);
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
	write_mis_items(file, out);
	return true;
}

std::string trigger_main_type_name(int value) {
	switch (static_cast<bms::TriggerMainType>(value)) {
		case bms::TriggerMainType::Group: return "Group";
		case bms::TriggerMainType::Single: return "Single";
		case bms::TriggerMainType::Event: return "Event";
		case bms::TriggerMainType::MissionVariable: return "MissionVariable";
		case bms::TriggerMainType::SecondTimeThrough: return "SecondTimeThrough";
		case bms::TriggerMainType::Teammate: return "Teammate";
		case bms::TriggerMainType::Player: return "Player";
	}
	return unknown_label("Unknown", value);
}

std::string group_trigger_type_name(int value) {
	switch (static_cast<bms::GroupTriggerType>(value)) {
		case bms::GroupTriggerType::Null: return "Null";
		case bms::GroupTriggerType::GroupSeesGroup: return "GroupSeesGroup";
		case bms::GroupTriggerType::GroupHasTargetedGroup: return "GroupHasTargetedGroup";
		case bms::GroupTriggerType::GroupAtRedAlert: return "GroupAtRedAlert";
		case bms::GroupTriggerType::GroupDestroyed: return "GroupDestroyed";
		case bms::GroupTriggerType::GroupAlive: return "GroupAlive";
		case bms::GroupTriggerType::GroupHasLostMoreUnits: return "GroupHasLostMoreUnits";
		case bms::GroupTriggerType::GroupAtWaypoint: return "GroupAtWaypoint";
		case bms::GroupTriggerType::GroupIntact: return "GroupIntact";
		case bms::GroupTriggerType::GroupIsWithinArea: return "GroupIsWithinArea";
		case bms::GroupTriggerType::GroupHoldingGroup: return "GroupHoldingGroup";
		case bms::GroupTriggerType::GroupHasMoreUnits: return "GroupHasMoreUnits";
		case bms::GroupTriggerType::GroupHasShotGroup: return "GroupHasShotGroup";
		case bms::GroupTriggerType::GroupAtYellowAlert: return "GroupAtYellowAlert";
		case bms::GroupTriggerType::GroupHasTargetedSingle: return "GroupHasTargetedSingle";
		case bms::GroupTriggerType::GroupSeesSingle: return "GroupSeesSingle";
		case bms::GroupTriggerType::GroupHasShotSingle: return "GroupHasShotSingle";
	}
	return unknown_label("Unknown", value);
}

std::string single_trigger_type_name(int value) {
	switch (static_cast<bms::SingleTriggerType>(value)) {
		case bms::SingleTriggerType::Null: return "Null";
		case bms::SingleTriggerType::SingleSeesGroup: return "SingleSeesGroup";
		case bms::SingleTriggerType::SingleHasTargetedGroup: return "SingleHasTargetedGroup";
		case bms::SingleTriggerType::SingleAtRedAlert: return "SingleAtRedAlert";
		case bms::SingleTriggerType::SingleDestroyed: return "SingleDestroyed";
		case bms::SingleTriggerType::SingleAlive: return "SingleAlive";
		case bms::SingleTriggerType::SingleHasLostMoreUnits: return "SingleHasLostMoreUnits";
		case bms::SingleTriggerType::SingleAtWaypoint: return "SingleAtWaypoint";
		case bms::SingleTriggerType::SingleIntact: return "SingleIntact";
		case bms::SingleTriggerType::SingleIsWithinArea: return "SingleIsWithinArea";
		case bms::SingleTriggerType::SingleHoldingGroup: return "SingleHoldingGroup";
		case bms::SingleTriggerType::SingleHasMoreUnits: return "SingleHasMoreUnits";
		case bms::SingleTriggerType::SingleHasShotGroup: return "SingleHasShotGroup";
		case bms::SingleTriggerType::SingleAtYellowAlert: return "SingleAtYellowAlert";
		case bms::SingleTriggerType::SingleHasTargetedSingle: return "SingleHasTargetedSingle";
		case bms::SingleTriggerType::SingleSeesSingle: return "SingleSeesSingle";
		case bms::SingleTriggerType::SingleHasShotSingle: return "SingleHasShotSingle";
		case bms::SingleTriggerType::SingleOnTopOf: return "SingleOnTopOf";
		case bms::SingleTriggerType::SingleFartherThan: return "SingleFartherThan";
		case bms::SingleTriggerType::SingleHasNoLOS: return "SingleHasNoLOS";
		case bms::SingleTriggerType::SingleDoesNotSeeOrFarther: return "SingleDoesNotSeeOrFarther";
	}
	return unknown_label("Unknown", value);
}

std::string trigger_sub_type_name(int main_type, int sub_type) {
	switch (static_cast<bms::TriggerMainType>(main_type)) {
		case bms::TriggerMainType::Group:
			return group_trigger_type_name(sub_type);
		case bms::TriggerMainType::Single:
			return single_trigger_type_name(sub_type);
		case bms::TriggerMainType::MissionVariable:
			switch (static_cast<bms::MissionVariableTriggerType>(sub_type)) {
				case bms::MissionVariableTriggerType::MissionVariableIsEqual: return "MissionVariableIsEqual";
				case bms::MissionVariableTriggerType::MissionVariableIsLessThan: return "MissionVariableIsLessThan";
				case bms::MissionVariableTriggerType::MissionVariableIsLessThanOrEqual: return "MissionVariableIsLessThanOrEqual";
				case bms::MissionVariableTriggerType::MissionVariableIsGreaterThan: return "MissionVariableIsGreaterThan";
				case bms::MissionVariableTriggerType::MissionVariableIsGreaterThanOrEqual: return "MissionVariableIsGreaterThanOrEqual";
			}
			break;
		case bms::TriggerMainType::Teammate:
			switch (static_cast<bms::TeammateTriggerType>(sub_type)) {
				case bms::TeammateTriggerType::TeammateIsEnabled: return "TeammateIsEnabled";
				case bms::TeammateTriggerType::TeammateMedicAssisting: return "TeammateMedicAssisting";
				case bms::TeammateTriggerType::TeammateEvacuating: return "TeammateEvacuating";
			}
			break;
		case bms::TriggerMainType::Player:
			switch (static_cast<bms::PlayerTriggerType>(sub_type)) {
				case bms::PlayerTriggerType::PlayerBerserk: return "PlayerBerserk";
				case bms::PlayerTriggerType::PlayerFirstPerson: return "PlayerFirstPerson";
				case bms::PlayerTriggerType::PlayerThirdPerson: return "PlayerThirdPerson";
				case bms::PlayerTriggerType::PlayerCockpitView: return "PlayerCockpitView";
				case bms::PlayerTriggerType::PlayerDialogDone: return "PlayerDialogDone";
				case bms::PlayerTriggerType::PlayerDialogFinished: return "PlayerDialogFinished";
				case bms::PlayerTriggerType::PlayerAwol: return "PlayerAwol";
				case bms::PlayerTriggerType::PlayerSatchel: return "PlayerSatchel";
				case bms::PlayerTriggerType::PlayerAttachedToSsn: return "PlayerAttachedToSsn";
				case bms::PlayerTriggerType::PlayerOnSsn: return "PlayerOnSsn";
				case bms::PlayerTriggerType::PlayerDrivingSsn: return "PlayerDrivingSsn";
				case bms::PlayerTriggerType::PlayerOnGun: return "PlayerOnGun";
			}
			break;
		case bms::TriggerMainType::Event:
		case bms::TriggerMainType::SecondTimeThrough:
			if (sub_type == 0) {
				return "Null";
			}
			break;
	}
	return unknown_label("Unknown", sub_type);
}

std::string action_type_name(int value) {
	switch (static_cast<bms::ActionType>(value)) {
		case bms::ActionType::Null: return "Null";
		case bms::ActionType::RedirectGroupTo: return "RedirectGroupTo";
		case bms::ActionType::KillGroup: return "KillGroup";
		case bms::ActionType::ChangeGroupAI: return "ChangeGroupAI";
		case bms::ActionType::VaporizeGroup: return "VaporizeGroup";
		case bms::ActionType::MisvarChange: return "MisvarChange";
		case bms::ActionType::OutputText: return "OutputText";
		case bms::ActionType::PlayWavList: return "PlayWavList";
		case bms::ActionType::BlueWin: return "BlueWin";
		case bms::ActionType::RedWin: return "RedWin";
		case bms::ActionType::GreenWin: return "GreenWin";
		case bms::ActionType::GroupVelocity: return "GroupVelocity";
		case bms::ActionType::AreaAiRed: return "AreaAiRed";
		case bms::ActionType::AreaAiBlue: return "AreaAiBlue";
		case bms::ActionType::SubGoalWon: return "SubGoalWon";
		case bms::ActionType::SubGoalLost: return "SubGoalLost";
		case bms::ActionType::ChangeGTeamAction: return "ChangeGTeamAction";
		case bms::ActionType::ChangeGroupAction: return "ChangeGroupAction";
		case bms::ActionType::GroupTeleportAction: return "GroupTeleportAction";
		case bms::ActionType::RedirectSingleTo: return "RedirectSingleTo";
		case bms::ActionType::KillSingle: return "KillSingle";
		case bms::ActionType::ChangeSingleAI: return "ChangeSingleAI";
		case bms::ActionType::VaporizeSingle: return "VaporizeSingle";
		case bms::ActionType::SingleVelocity: return "SingleVelocity";
		case bms::ActionType::ChangeSteamAction: return "ChangeSteamAction";
		case bms::ActionType::SingleChangeGroup: return "SingleChangeGroup";
		case bms::ActionType::SingleTeleportAction: return "SingleTeleportAction";
		case bms::ActionType::ParticleEffectAction: return "ParticleEffectAction";
		case bms::ActionType::GroupOpenDoorAction: return "GroupOpenDoorAction";
		case bms::ActionType::GroupCloseDoorAction: return "GroupCloseDoorAction";
		case bms::ActionType::GroupResetHasVisited: return "GroupResetHasVisited";
		case bms::ActionType::SingleResetHasVisited: return "SingleResetHasVisited";
		case bms::ActionType::ResetEvent: return "ResetEvent";
		case bms::ActionType::ShowWinSubgoal: return "ShowWinSubgoal";
		case bms::ActionType::ShowLoseSubgoal: return "ShowLoseSubgoal";
		case bms::ActionType::AttachToEmplaced: return "AttachToEmplaced";
		case bms::ActionType::SetLightState: return "SetLightState";
		case bms::ActionType::Teammates: return "Teammates";
		case bms::ActionType::ShowWaypoints: return "ShowWaypoints";
		case bms::ActionType::ExecuteWac: return "ExecuteWac";
		case bms::ActionType::SsnTargetSsnPri: return "SsnTargetSsnPri";
		case bms::ActionType::SsnTargetSsnExc: return "SsnTargetSsnExc";
		case bms::ActionType::SsnTargetGroupPri: return "SsnTargetGroupPri";
		case bms::ActionType::SsnTargetGroupExc: return "SsnTargetGroupExc";
		case bms::ActionType::GroupTargetSsnPri: return "GroupTargetSsnPri";
		case bms::ActionType::GroupTargetSsnExc: return "GroupTargetSsnExc";
		case bms::ActionType::GroupTargetGroupPri: return "GroupTargetGroupPri";
		case bms::ActionType::GroupTargetGroupExc: return "GroupTargetGroupExc";
	}
	return unknown_label("Unknown", value);
}

std::string ai_action_sub_type_name(int value) {
	switch (static_cast<bms::AIActionSubType>(value)) {
		case bms::AIActionSubType::GuardBit: return "GuardBit";
		case bms::AIActionSubType::Accuracy: return "Accuracy";
		case bms::AIActionSubType::BlindBit: return "BlindBit";
		case bms::AIActionSubType::BerserkBit: return "BerserkBit";
		case bms::AIActionSubType::ClimberBit: return "ClimberBit";
		case bms::AIActionSubType::CowardBit: return "CowardBit";
		case bms::AIActionSubType::Skill1: return "Skill1";
		case bms::AIActionSubType::Skill2: return "Skill2";
		case bms::AIActionSubType::AiState: return "AiState";
		case bms::AIActionSubType::SpeedKmh1: return "SpeedKmh1";
		case bms::AIActionSubType::SpeedKmh2: return "SpeedKmh2";
		case bms::AIActionSubType::TargetSsn1: return "TargetSsn1";
		case bms::AIActionSubType::AnimNum: return "AnimNum";
		case bms::AIActionSubType::HudItem: return "HudItem";
		case bms::AIActionSubType::TmateStatus: return "TmateStatus";
		case bms::AIActionSubType::AiNodePathBit: return "AiNodePathBit";
		case bms::AIActionSubType::AttackDistanceValue: return "AttackDistanceValue";
		case bms::AIActionSubType::EngageDistanceMin: return "EngageDistanceMin";
		case bms::AIActionSubType::IndestructableBit: return "IndestructableBit";
		case bms::AIActionSubType::TargetSsn2: return "TargetSsn2";
		case bms::AIActionSubType::StartFiringBit: return "StartFiringBit";
		case bms::AIActionSubType::FiringAngle: return "FiringAngle";
	}
	return unknown_label("Unknown", value);
}

std::string action_sub_type_name(int action_type, int sub_type) {
	switch (static_cast<bms::ActionType>(action_type)) {
		case bms::ActionType::ChangeGroupAI:
		case bms::ActionType::ChangeSingleAI:
			return ai_action_sub_type_name(sub_type);
		case bms::ActionType::MisvarChange:
			switch (static_cast<bms::MissionVariableActionSubType>(sub_type)) {
				case bms::MissionVariableActionSubType::Null: return "Null";
				case bms::MissionVariableActionSubType::Set: return "Set";
				case bms::MissionVariableActionSubType::Add: return "Add";
				case bms::MissionVariableActionSubType::Subtract: return "Subtract";
				case bms::MissionVariableActionSubType::Increment: return "Increment";
				case bms::MissionVariableActionSubType::Decrement: return "Decrement";
			}
			break;
		case bms::ActionType::Teammates:
			switch (static_cast<bms::TeammateActionSubType>(sub_type)) {
				case bms::TeammateActionSubType::Null: return "Null";
				case bms::TeammateActionSubType::MedicAssist: return "MedicAssist";
				case bms::TeammateActionSubType::EvacuateTt: return "EvacuateTt";
				case bms::TeammateActionSubType::EvacuateAt: return "EvacuateAt";
			}
			break;
		default:
			if (sub_type == 0) {
				return "Null";
			}
			break;
	}
	return unknown_label("Unknown", sub_type);
}

AreaTriggerRecord to_area_trigger_record(const bms::AreaTrigger &area, size_t index) {
	AreaTriggerRecord out;
	out.index = index;
	// Corrected mapping: file layout is interleaved per axis with a flags dword at off 28.
	// (id at off 0 carried in wp_number for ABI stability; raw flags in reserved.)
	out.wp_number = area.id;
	out.min_x = area.get_x_min();
	out.min_y = area.get_y_min();
	out.min_z = area.get_z_min();
	out.max_x = area.get_x_max();
	out.max_y = area.get_y_max();
	out.max_z = area.get_z_max();
	out.reserved = static_cast<int>(area.flags);
	out.active = area.is_active();
	out.constrain_z = area.constrains_z();
	return out;
}

// Inverse of to_area_trigger_record: build a byte-faithful bms::AreaTrigger from the typed record.
// Bounds go to interleaved fixed-point 16.16 (no swap). The flags dword keeps every bit of `reserved`
// except the two low bits, which are recomposed from active/constrain_z so the UI toggles stay
// consistent with the raw value the engine reads (Entity_IsTeamInTriggerBounds @0x43c75c).
bms::AreaTrigger from_area_trigger_record(const AreaTriggerRecord &rec) {
	bms::AreaTrigger area;
	area.id = rec.wp_number;
	area.x_min = bms::to_fixed_16_16(rec.min_x);
	area.x_max = bms::to_fixed_16_16(rec.max_x);
	area.y_min = bms::to_fixed_16_16(rec.min_y);
	area.y_max = bms::to_fixed_16_16(rec.max_y);
	area.z_min = bms::to_fixed_16_16(rec.min_z);
	area.z_max = bms::to_fixed_16_16(rec.max_z);
	uint32_t flags = static_cast<uint32_t>(rec.reserved);
	flags = (flags & ~0x3u) | (rec.active ? 0x1u : 0u) | (rec.constrain_z ? 0x2u : 0u);
	area.flags = flags;
	return area;
}

MissionEventRecord to_event_record(const bms::Event &event, size_t index) {
	MissionEventRecord out;
	out.index = index;
	out.flags = static_cast<int>(event.flags);
	out.trigger_index = event.trigger_index;
	out.action_index = event.action_index;
	out.trigger_count = event.trigger_count;
	out.action_count = event.action_count;
	out.reset_after = event.reset_after;
	out.delay = event.delay;
	out.unknown5 = event.unknown5;
	out.unknown6 = event.unknown6;
	return out;
}

MissionTriggerRecord to_trigger_record(const bms::Trigger &trigger, size_t index) {
	MissionTriggerRecord out;
	out.index = index;
	out.condition_flags = trigger.condition_flags;
	out.main_type = static_cast<int>(trigger.main_type);
	out.main_type_name = trigger_main_type_name(out.main_type);
	out.sub_type = trigger.sub_type;
	out.sub_type_name = trigger_sub_type_name(out.main_type, out.sub_type);
	out.param1 = trigger.param1;
	out.param2 = trigger.param2;
	out.param3 = trigger.param3;
	out.param4 = trigger.param4;
	out.unknown7 = trigger.unknown7;
	out.negated = trigger.is_negated();
	out.logic_or = trigger.is_or();
	out.logic_xor = trigger.is_xor();
	out.logic_operator = trigger.get_logic_operator();
	return out;
}

MissionActionRecord to_action_record(const bms::Action &action, size_t index) {
	MissionActionRecord out;
	out.index = index;
	out.action_type = static_cast<int>(action.action_type);
	out.action_type_name = action_type_name(out.action_type);
	out.action_sub_type = action.action_sub_type;
	out.action_sub_type_name = action_sub_type_name(out.action_type, out.action_sub_type);
	out.param1 = action.param1;
	out.param2 = action.param2;
	out.param3 = action.param3;
	out.param4 = action.param4;
	out.reserved0 = action.reserved0;
	out.reserved1 = action.reserved1;
	return out;
}

constexpr int kMaxEventChainEntries = 20;

void apply_event_record(bms::Event &event, const MissionEventRecord &record) {
	event.flags = static_cast<bms::EventFlags>(record.flags);
	event.reset_after = record.reset_after;
	event.delay = record.delay;
	event.unknown5 = static_cast<uint8_t>(std::clamp(record.unknown5, 0, 255));
	event.unknown6 = static_cast<uint8_t>(std::clamp(record.unknown6, 0, 255));
}

bms::Trigger trigger_from_record(const MissionTriggerRecord &record) {
	bms::Trigger trigger = {};
	trigger.condition_flags = record.condition_flags;
	trigger.main_type = static_cast<bms::TriggerMainType>(record.main_type);
	trigger.sub_type = record.sub_type;
	trigger.param1 = record.param1;
	trigger.param2 = record.param2;
	trigger.param3 = record.param3;
	trigger.param4 = record.param4;
	trigger.unknown7 = record.unknown7;
	return trigger;
}

bms::Action action_from_record(const MissionActionRecord &record) {
	bms::Action action = {};
	action.reserved0 = record.reserved0;
	action.action_type = static_cast<bms::ActionType>(record.action_type);
	action.action_sub_type = record.action_sub_type;
	action.param1 = record.param1;
	action.param2 = record.param2;
	action.param3 = record.param3;
	action.param4 = record.param4;
	action.reserved1 = record.reserved1;
	return action;
}

MissionLogicDiagnostic logic_diagnostic(const std::string &code,
                                        const std::string &message,
                                        const std::string &subject_kind,
                                        int subject_index) {
	MissionLogicDiagnostic diagnostic;
	diagnostic.severity = "warning";
	diagnostic.code = code;
	diagnostic.message = message;
	diagnostic.subject_kind = subject_kind;
	diagnostic.subject_index = subject_index;
	return diagnostic;
}

bool valid_range(int start, int count, size_t total) {
	if (count == 0) {
		return true;
	}
	if (start < 0 || count < 0) {
		return false;
	}
	const size_t range_start = static_cast<size_t>(start);
	const size_t range_count = static_cast<size_t>(count);
	return range_start <= total && range_count <= total - range_start;
}

MissionLogicReference logic_reference(const std::string &source_kind,
                                      int source_index,
                                      const std::string &target_kind,
                                      int target_index,
                                      int param_slot,
                                      int raw_value,
                                      const std::string &label,
                                      bool valid) {
	MissionLogicReference reference;
	reference.source_kind = source_kind;
	reference.source_index = source_index;
	reference.target_kind = target_kind;
	reference.target_index = target_index;
	reference.param_slot = param_slot;
	reference.raw_value = raw_value;
	reference.label = label;
	reference.valid = valid;
	return reference;
}

void add_trigger_area_reference(const MissionTriggerRecord &trigger,
                                size_t area_count,
                                MissionEventChain &chain) {
	const bool area_trigger =
			(trigger.main_type == static_cast<int>(bms::TriggerMainType::Group) &&
					trigger.sub_type == static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea)) ||
			(trigger.main_type == static_cast<int>(bms::TriggerMainType::Single) &&
					trigger.sub_type == static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea));
	if (!area_trigger) {
		return;
	}
	const int area_index = trigger.param2;
	const bool valid = area_index >= 0 && static_cast<size_t>(area_index) < area_count;
	chain.references.push_back(logic_reference("trigger", static_cast<int>(trigger.index), "area_trigger", area_index, 2, trigger.param2, "area", valid));
	if (!valid) {
		chain.diagnostics.push_back(logic_diagnostic(
				"logic.area_reference_out_of_range",
				"Trigger references an area trigger index outside the mission area table.",
				"trigger",
				static_cast<int>(trigger.index)));
	}
}

std::vector<bms::Entity> *entities_for(bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

const std::vector<bms::Entity> *entities_for(const bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

int next_entity_id(const bms::File &file) {
	int max_id = 0;
	auto scan = [&max_id](const std::vector<bms::Entity> &entities) {
		for (const bms::Entity &entity : entities) {
			max_id = std::max(max_id, entity.id);
		}
	};
	scan(file.items);
	scan(file.buildings);
	scan(file.markers);
	scan(file.organics);
	return max_id + 1;
}

EntityRecord to_record(const bms::Entity &entity, EntityKind kind, size_t index) {
	EntityRecord out;
	out.kind = kind;
	out.index = index;
	out.item_id = bms_type_id_to_item_id(entity.type_id);
	out.bms_type_id = entity.type_id;
	out.bms_id = entity.id;
	out.transform.x = entity.get_x();
	out.transform.y = entity.get_y();
	out.transform.z = entity.get_z();
	out.transform.pitch = entity.pitch;
	out.transform.yaw = entity.yaw;
	out.transform.roll = entity.roll;
	out.group_id = entity.group_id;
	out.waypoint_id = entity.waypoint_id;
	out.wp_number = entity.wp_number;
	out.team = entity.team;
	out.ai_flags = static_cast<int>(entity.bmsi_attributes);
	out.perception = entity.perception2;
	out.accuracy = entity.w_accuracy1;
	out.alert_state = entity.alert_state;
	out.min_engagement_distance = entity.min_engagement_distance;
	out.max_engagement_distance = entity.max_engagement_distance;
	out.max_attack_distance = entity.max_attack_distance;
	out.spawn_count = entity.spawns;
	out.max_simultaneous = entity.no_more_than;
	out.no_less_than = entity.no_less_than;
	out.map_symbol = entity.map_symbol;
	out.name1 = fixed_string(entity.name1, sizeof(entity.name1));
	out.name2 = fixed_string(entity.name2, sizeof(entity.name2));
	return out;
}

// Defined further below; used by apply_properties for the fixed-string name fields.
void copy_cstr(char *dest, size_t dest_size, const std::string &value);

// Copy into a fixed-width on-disk field that may use ALL dest_size bytes (no reserved NUL
// terminator). The format's name1/name2 are 8-byte slots a shipped mission can fill completely,
// so copy_cstr (which forces dest[dest_size-1] = '\0') would drop the 8th byte and silently
// truncate an 8-char name on every property round-trip. Values longer than the field are cut to
// dest_size; shorter values zero-pad the remainder. fixed_string reads it back symmetrically.
void copy_fixed_field(char *dest, size_t dest_size, const std::string &value) {
	const size_t copy_len = std::min(dest_size, value.size());
	if (copy_len > 0) {
		std::memcpy(dest, value.data(), copy_len);
	}
	if (copy_len < dest_size) {
		std::memset(dest + copy_len, 0, dest_size - copy_len);
	}
}

void apply_transform(bms::Entity &entity, const EntityTransform &transform) {
	entity.set_x(transform.x);
	entity.set_y(transform.y);
	entity.set_z(transform.z);
	entity.pitch = static_cast<int16_t>(transform.pitch);
	entity.yaw = static_cast<int16_t>(transform.yaw);
	entity.roll = static_cast<int16_t>(transform.roll);
}

void apply_properties(bms::Entity &entity, const EntityProperties &properties) {
	// The uint8-backed fields clamp (rather than a bare static_cast) so an out-of-range value from a
	// programmatic caller saturates instead of silently wrapping (e.g. map_symbol 300 -> 44). The
	// inspector SpinBoxes already cap these, but set_entity_properties is a public API boundary.
	entity.group_id = static_cast<uint8_t>(std::clamp(properties.group_id, 0, 255));
	entity.waypoint_id = static_cast<uint8_t>(std::clamp(properties.waypoint_id, 0, 255));
	entity.wp_number = properties.wp_number;
	entity.team = static_cast<uint8_t>(std::clamp(properties.team, 0, 255));
	entity.bmsi_attributes = static_cast<uint32_t>(properties.ai_flags);
	entity.perception2 = properties.perception;
	entity.w_accuracy1 = static_cast<int16_t>(properties.accuracy);
	entity.alert_state = static_cast<uint8_t>(std::clamp(properties.alert_state, 0, 255));
	entity.min_engagement_distance = properties.min_engagement_distance;
	entity.max_engagement_distance = properties.max_engagement_distance;
	entity.max_attack_distance = properties.max_attack_distance;
	entity.spawns = static_cast<int16_t>(properties.spawn_count);
	entity.no_more_than = static_cast<uint8_t>(std::clamp(properties.max_simultaneous, 0, 255));
	entity.no_less_than = static_cast<uint8_t>(std::clamp(properties.no_less_than, 0, 255));
	entity.map_symbol = static_cast<uint8_t>(std::clamp(properties.map_symbol, 0, 255));
	// name1/name2 are fixed 8-byte slots a mission can fill completely; copy_fixed_field keeps all
	// 8 bytes (copy_cstr would force a NUL into byte 7 and truncate an 8-char name on every edit).
	copy_fixed_field(entity.name1, sizeof(entity.name1), properties.name1);
	copy_fixed_field(entity.name2, sizeof(entity.name2), properties.name2);
}

bms::Entity make_default_entity(const bms::File &file,
                                EntityKind kind,
                                int item_id,
                                const EntityTransform &transform) {
	bms::Entity entity = {};
	entity.type = to_bms_type(kind);
	entity.type_id = item_id_to_bms_type_id(item_id);
	entity.id = next_entity_id(file);
	entity.perception2 = 100;
	entity.perfectionist2 = 100;
	entity.min_engagement_distance = 20;
	entity.max_engagement_distance = 200;
	entity.w_accuracy1 = 50;
	entity.w_accuracy2 = 50;
	entity.spawns = 1;
	entity.no_more_than = 1;
	entity.max_attack_distance = 100;
	apply_transform(entity, transform);
	return entity;
}

void copy_cstr(char *dest, size_t dest_size, const std::string &value) {
	if (dest_size == 0) {
		return;
	}
	const size_t copy_len = std::min(dest_size - 1, value.size());
	std::memcpy(dest, value.data(), copy_len);
	dest[copy_len] = '\0';
	if (copy_len + 1 < dest_size) {
		std::memset(dest + copy_len + 1, 0, dest_size - copy_len - 1);
	}
}

OpenNovaMissionEntityTransform to_c_transform(const EntityTransform &transform) {
	return {
		transform.x,
		transform.y,
		transform.z,
		transform.pitch,
		transform.yaw,
		transform.roll,
	};
}

EntityTransform from_c_transform(const OpenNovaMissionEntityTransform &transform) {
	return {
		transform.x,
		transform.y,
		transform.z,
		transform.pitch,
		transform.yaw,
		transform.roll,
	};
}

EntityProperties from_c_properties(const OpenNovaMissionEntityProperties &properties) {
	return {
		properties.group_id,
		properties.waypoint_id,
		properties.wp_number,
		properties.team,
		properties.ai_flags,
		properties.perception,
		properties.accuracy,
		properties.alert_state,
		properties.min_engagement_distance,
		properties.max_engagement_distance,
		properties.max_attack_distance,
		properties.spawn_count,
		properties.max_simultaneous,
	};
}

void copy_record(OpenNovaMissionEntityRecord &out, const EntityRecord &record) {
	out.kind = static_cast<int>(record.kind);
	out.index = record.index;
	out.item_id = record.item_id;
	out.bms_type_id = record.bms_type_id;
	out.bms_id = record.bms_id;
	out.transform = to_c_transform(record.transform);
	out.group_id = record.group_id;
	out.waypoint_id = record.waypoint_id;
	out.wp_number = record.wp_number;
	out.team = record.team;
	out.ai_flags = record.ai_flags;
	out.perception = record.perception;
	out.accuracy = record.accuracy;
	out.alert_state = record.alert_state;
	out.min_engagement_distance = record.min_engagement_distance;
	out.max_engagement_distance = record.max_engagement_distance;
	out.max_attack_distance = record.max_attack_distance;
	out.spawn_count = record.spawn_count;
	out.max_simultaneous = record.max_simultaneous;
}

void copy_waypoint_summary(OpenNovaMissionWaypointSummary &out, const WaypointSummary &summary) {
	out.index = summary.index;
	out.flags = summary.flags;
	out.marker_count = summary.marker_count;
}

void copy_waypoint_path(OpenNovaMissionWaypointPath &out, const WaypointPath &path) {
	std::memset(&out, 0, sizeof(out));
	out.index = path.index;
	out.flags = path.flags;
	out.marker_count = std::min<size_t>(path.marker_indices.size(), kMaxWaypointPathMarkers);
	for (size_t i = 0; i < out.marker_count; ++i) {
		out.marker_indices[i] = static_cast<uint32_t>(path.marker_indices[i]);
	}
}

void copy_area_trigger(OpenNovaMissionAreaTriggerRecord &out, const AreaTriggerRecord &record) {
	out.index = record.index;
	out.wp_number = record.wp_number;
	out.min_x = record.min_x;
	out.min_y = record.min_y;
	out.min_z = record.min_z;
	out.max_x = record.max_x;
	out.max_y = record.max_y;
	out.max_z = record.max_z;
	out.reserved = record.reserved;
	out.active = record.active ? 1 : 0;
	out.constrain_z = record.constrain_z ? 1 : 0;
}

AreaTriggerRecord from_c_area_trigger(const OpenNovaMissionAreaTriggerRecord &in) {
	AreaTriggerRecord out;
	out.index = in.index;
	out.wp_number = in.wp_number;
	out.min_x = in.min_x;
	out.min_y = in.min_y;
	out.min_z = in.min_z;
	out.max_x = in.max_x;
	out.max_y = in.max_y;
	out.max_z = in.max_z;
	out.reserved = in.reserved;
	out.active = in.active != 0;
	out.constrain_z = in.constrain_z != 0;
	return out;
}

void copy_event(OpenNovaMissionEventRecord &out, const MissionEventRecord &record) {
	out.index = record.index;
	out.flags = record.flags;
	out.trigger_index = record.trigger_index;
	out.action_index = record.action_index;
	out.trigger_count = record.trigger_count;
	out.action_count = record.action_count;
	out.reset_after = record.reset_after;
	out.delay = record.delay;
	out.unknown5 = record.unknown5;
	out.unknown6 = record.unknown6;
}

void copy_trigger(OpenNovaMissionTriggerRecord &out, const MissionTriggerRecord &record) {
	std::memset(&out, 0, sizeof(out));
	out.index = record.index;
	out.condition_flags = record.condition_flags;
	out.main_type = record.main_type;
	copy_cstr(out.main_type_name, sizeof(out.main_type_name), record.main_type_name);
	out.sub_type = record.sub_type;
	copy_cstr(out.sub_type_name, sizeof(out.sub_type_name), record.sub_type_name);
	out.param1 = record.param1;
	out.param2 = record.param2;
	out.param3 = record.param3;
	out.param4 = record.param4;
	out.unknown7 = record.unknown7;
	out.negated = record.negated ? 1 : 0;
	out.logic_or = record.logic_or ? 1 : 0;
	out.logic_xor = record.logic_xor ? 1 : 0;
	copy_cstr(out.logic_operator, sizeof(out.logic_operator), record.logic_operator);
}

void copy_action(OpenNovaMissionActionRecord &out, const MissionActionRecord &record) {
	std::memset(&out, 0, sizeof(out));
	out.index = record.index;
	out.action_type = record.action_type;
	copy_cstr(out.action_type_name, sizeof(out.action_type_name), record.action_type_name);
	out.action_sub_type = record.action_sub_type;
	copy_cstr(out.action_sub_type_name, sizeof(out.action_sub_type_name), record.action_sub_type_name);
	out.param1 = record.param1;
	out.param2 = record.param2;
	out.param3 = record.param3;
	out.param4 = record.param4;
	out.reserved0 = record.reserved0;
	out.reserved1 = record.reserved1;
}

void copy_logic_summary(OpenNovaMissionLogicSummary &out, const MissionLogicSummary &summary) {
	out.event_count = summary.event_count;
	out.trigger_count = summary.trigger_count;
	out.action_count = summary.action_count;
	out.area_trigger_count = summary.area_trigger_count;
	out.diagnostic_count = summary.diagnostic_count;
}

} // namespace

struct MissionDocument::Impl {
	bms::File file;
	std::string source_path;
	std::string last_error;
	bool loaded = false;
};

MissionDocument::MissionDocument() : impl_(std::make_unique<Impl>()) {}
MissionDocument::~MissionDocument() = default;
MissionDocument::MissionDocument(MissionDocument &&) noexcept = default;
MissionDocument &MissionDocument::operator=(MissionDocument &&) noexcept = default;

bool MissionDocument::load_bms_file(const std::string &path) {
	clear();
	std::string error;
	if (!bms::parse_file(path, impl_->file, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->source_path = path;
	impl_->loaded = true;
	sync_counts();
	return true;
}

bool MissionDocument::load_bms_bytes(const uint8_t *data, size_t size) {
	clear();
	if (data == nullptr || size == 0) {
		impl_->last_error = "No BMS bytes provided";
		return false;
	}
	std::string error;
	if (!bms::parse(data, size, impl_->file, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->loaded = true;
	sync_counts();
	return true;
}

bool MissionDocument::save_bms_file(const std::string &path) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path.empty()) {
		impl_->last_error = "No output path provided";
		return false;
	}
	sync_counts();
	std::string error;
	if (!bms::write_file(impl_->file, path, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->source_path = path;
	return true;
}

bool MissionDocument::write_bms_bytes(std::vector<uint8_t> &out) {
	out.clear();
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	sync_counts();
	std::string error;
	if (!bms::write(impl_->file, out, error)) {
		impl_->last_error = error;
		return false;
	}
	return true;
}

bool MissionDocument::save_mis_file(const std::string &path) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path.empty()) {
		impl_->last_error = "No output path provided";
		return false;
	}
	std::string text;
	if (!write_mis_text(text)) {
		return false;
	}
	std::ofstream file(path, std::ios::binary);
	if (!file.good()) {
		impl_->last_error = "Cannot create MIS file: " + path;
		return false;
	}
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!file.good()) {
		impl_->last_error = "Failed writing MIS file: " + path;
		return false;
	}
	return true;
}

bool MissionDocument::write_mis_text(std::string &out) {
	out.clear();
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	sync_counts();
	std::string error;
	if (!opennova::mission::write_mis_text(impl_->file, out, error)) {
		impl_->last_error = error;
		return false;
	}
	return true;
}

void MissionDocument::clear() {
	impl_->file = {};
	impl_->source_path.clear();
	impl_->last_error.clear();
	impl_->loaded = false;
}

bool MissionDocument::is_loaded() const {
	return impl_->loaded;
}

const std::string &MissionDocument::source_path() const {
	return impl_->source_path;
}

const std::string &MissionDocument::last_error() const {
	return impl_->last_error;
}

MissionInfo MissionDocument::info() const {
	MissionInfo out;
	if (!impl_->loaded) {
		return out;
	}
	const bms::Header &header = impl_->file.header;
	out.mission_name = fixed_string(header.mission_name, sizeof(header.mission_name));
	out.designer = fixed_string(header.designer, sizeof(header.designer));
	out.briefing = fixed_string(header.mission_briefing, sizeof(header.mission_briefing));
	out.terrain = fixed_string(header.terrain, sizeof(header.terrain));
	out.environment = fixed_string(header.environment, sizeof(header.environment));
	out.climate = static_cast<int>(header.climate);
	out.weather = static_cast<int>(header.weather_type);
	out.mission_type = static_cast<int>(header.mission_type);
	out.attrib_flags = static_cast<int>(header.attrib_flags);
	out.start_time = header.start_time;
	out.minutes_per_day = header.minutes_per_day;
	out.player_health = static_cast<int>(header.health);
	out.max_saves = header.max_saves;
	out.music = static_cast<int>(header.music);
	out.reverb = static_cast<int>(header.reverb);
	out.wind_speed = static_cast<int>(header.wind_speed);
	out.wind_direction = static_cast<int>(header.wind_direction);
	out.map_zoom = header.map_zoom;
	return out;
}

bool MissionDocument::set_header_string(const std::string &field, const std::string &value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Header &header = impl_->file.header;
	if (field == "mission_name") {
		copy_cstr(header.mission_name, sizeof(header.mission_name), value);
	} else if (field == "designer") {
		copy_cstr(header.designer, sizeof(header.designer), value);
	} else if (field == "briefing") {
		copy_cstr(header.mission_briefing, sizeof(header.mission_briefing), value);
	} else if (field == "terrain") {
		// header.terrain[48] is three 16-byte NUL-terminated slots: terrain@+0, cnv_file@+16,
		// tt_file@+32 (see write_mis_general_information). Write only the first slot so a terrain
		// edit does not zero-fill (and lose) the cnv_file / tt_file references.
		copy_cstr(header.terrain, 16, value);
	} else if (field == "environment") {
		copy_cstr(header.environment, sizeof(header.environment), value);
	} else {
		impl_->last_error = "Unknown header string field: " + field;
		return false;
	}
	return true;
}

bool MissionDocument::set_header_int(const std::string &field, int value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Header &header = impl_->file.header;
	if (field == "climate") {
		header.climate = static_cast<bms::ClimateType>(value);
	} else if (field == "weather") {
		header.weather_type = static_cast<bms::WeatherType>(value);
	} else if (field == "mission_type") {
		header.mission_type = static_cast<bms::MissionType>(static_cast<uint8_t>(value));
	} else if (field == "attrib_flags") {
		header.attrib_flags = static_cast<bms::AttribFlags>(static_cast<uint32_t>(value));
	} else if (field == "start_time") {
		header.start_time = static_cast<uint16_t>(value);
	} else if (field == "minutes_per_day") {
		header.minutes_per_day = static_cast<uint16_t>(value);
	} else if (field == "player_health") {
		header.health = static_cast<uint32_t>(value);
	} else if (field == "max_saves") {
		header.max_saves = static_cast<uint8_t>(value);
	} else if (field == "music") {
		header.music = static_cast<uint32_t>(value);
	} else if (field == "reverb") {
		header.reverb = static_cast<uint32_t>(value);
	} else if (field == "wind_speed") {
		header.wind_speed = static_cast<uint32_t>(value);
	} else if (field == "wind_direction") {
		header.wind_direction = static_cast<uint32_t>(value);
	} else {
		impl_->last_error = "Unknown header int field: " + field;
		return false;
	}
	return true;
}

bool MissionDocument::set_header_flag(int bit, bool on) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	uint32_t flags = static_cast<uint32_t>(impl_->file.header.attrib_flags);
	if (on) {
		flags |= static_cast<uint32_t>(bit);
	} else {
		flags &= ~static_cast<uint32_t>(bit);
	}
	impl_->file.header.attrib_flags = static_cast<bms::AttribFlags>(flags);
	return true;
}

bool MissionDocument::set_header_float(const std::string &field, float value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (field == "map_zoom") {
		impl_->file.header.map_zoom = value;
	} else {
		impl_->last_error = "Unknown header float field: " + field;
		return false;
	}
	return true;
}

size_t MissionDocument::entity_count(EntityKind kind) const {
	if (!impl_->loaded) {
		return 0;
	}
	const std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	return entities ? entities->size() : 0;
}

bool MissionDocument::get_entity(EntityKind kind, size_t index, EntityRecord &out) const {
	if (!impl_->loaded) {
		return false;
	}
	const std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		return false;
	}
	out = to_record((*entities)[index], kind, index);
	return true;
}

bool MissionDocument::set_entity_transform(EntityKind kind, size_t index, const EntityTransform &transform) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	apply_transform((*entities)[index], transform);
	return true;
}

bool MissionDocument::set_entity_properties(EntityKind kind, size_t index, const EntityProperties &properties, EntityRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	apply_properties((*entities)[index], properties);
	if (out != nullptr) {
		*out = to_record((*entities)[index], kind, index);
	}
	return true;
}

bool MissionDocument::add_entity(EntityKind kind, int item_id, const EntityTransform &transform, EntityRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr) {
		impl_->last_error = "Invalid entity kind";
		return false;
	}
	bms::Entity entity = make_default_entity(impl_->file, kind, item_id, transform);
	entities->push_back(entity);
	sync_counts();
	if (out != nullptr) {
		*out = to_record(entities->back(), kind, entities->size() - 1);
	}
	return true;
}

bool MissionDocument::remove_entity(EntityKind kind, size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	entities->erase(entities->begin() + static_cast<std::ptrdiff_t>(index));
	if (kind == EntityKind::Marker) {
		repair_waypoint_marker_references(impl_->file, index);
	}
	sync_counts();
	return true;
}

std::vector<WaypointSummary> MissionDocument::waypoint_summaries() const {
	std::vector<WaypointSummary> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.waypoint_records.size());
	for (size_t i = 0; i < impl_->file.waypoint_records.size(); ++i) {
		const bms::WaypointRecord &record = impl_->file.waypoint_records[i];
		WaypointSummary summary;
		summary.index = i;
		summary.flags = static_cast<int>(record.flags);
		summary.marker_count = static_cast<int>(record.marker_count);
		out.push_back(summary);
	}
	return out;
}

size_t MissionDocument::waypoint_path_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.waypoint_records.size();
}

bool MissionDocument::get_waypoint_path(size_t index, WaypointPath &out) const {
	if (!impl_->loaded) {
		return false;
	}
	if (index >= impl_->file.waypoint_records.size()) {
		return false;
	}
	out = to_path(impl_->file.waypoint_records[index], index);
	return true;
}

std::vector<WaypointPath> MissionDocument::waypoint_paths() const {
	std::vector<WaypointPath> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.waypoint_records.size());
	for (size_t i = 0; i < impl_->file.waypoint_records.size(); ++i) {
		out.push_back(to_path(impl_->file.waypoint_records[i], i));
	}
	return out;
}

bool MissionDocument::set_waypoint_path(size_t index, const std::vector<int> &marker_indices, int flags, WaypointPath *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::string error;
	if (!validate_waypoint_path(impl_->file, index, marker_indices, error)) {
		impl_->last_error = error;
		return false;
	}
	apply_waypoint_path_to_record(impl_->file.waypoint_records[index], marker_indices, flags);
	if (out != nullptr) {
		*out = to_path(impl_->file.waypoint_records[index], index);
	}
	return true;
}

bool MissionDocument::clear_waypoint_path(size_t index, WaypointPath *out) {
	return set_waypoint_path(index, {}, 0, out);
}

bool MissionDocument::add_waypoint_marker(size_t path_index,
                                          int marker_item_id,
                                          const EntityTransform &transform,
                                          int insert_index,
                                          EntityRecord *out_marker,
                                          WaypointPath *out_path) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path_index >= impl_->file.waypoint_records.size()) {
		impl_->last_error = "Waypoint path index out of range";
		return false;
	}
	WaypointPath current = to_path(impl_->file.waypoint_records[path_index], path_index);
	if (current.marker_indices.size() >= kMaxWaypointPathMarkers) {
		impl_->last_error = "Waypoint path marker count exceeds 32";
		return false;
	}
	std::string error;
	if (!validate_waypoint_path(impl_->file, path_index, current.marker_indices, error)) {
		impl_->last_error = error;
		return false;
	}

	bms::Entity marker = make_default_entity(impl_->file, EntityKind::Marker, marker_item_id, transform);
	marker.bmsi_attributes |= static_cast<uint32_t>(bms::BmsiAttributeFlags::NavigationWaypoint);
	impl_->file.markers.push_back(marker);
	const int new_marker_index = static_cast<int>(impl_->file.markers.size() - 1);
	size_t insertion = current.marker_indices.size();
	if (insert_index >= 0) {
		insertion = std::min<size_t>(static_cast<size_t>(insert_index), current.marker_indices.size());
	}
	current.marker_indices.insert(current.marker_indices.begin() + static_cast<std::ptrdiff_t>(insertion), new_marker_index);
	apply_waypoint_path_to_record(impl_->file.waypoint_records[path_index], current.marker_indices, current.flags);
	sync_counts();
	if (out_marker != nullptr) {
		*out_marker = to_record(impl_->file.markers.back(), EntityKind::Marker, impl_->file.markers.size() - 1);
	}
	if (out_path != nullptr) {
		*out_path = to_path(impl_->file.waypoint_records[path_index], path_index);
	}
	return true;
}

size_t MissionDocument::area_trigger_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.area_triggers.size();
}

bool MissionDocument::get_area_trigger(size_t index, AreaTriggerRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.area_triggers.size()) {
		return false;
	}
	out = to_area_trigger_record(impl_->file.area_triggers[index], index);
	return true;
}

std::vector<AreaTriggerRecord> MissionDocument::area_triggers() const {
	std::vector<AreaTriggerRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.area_triggers.size());
	for (size_t i = 0; i < impl_->file.area_triggers.size(); ++i) {
		out.push_back(to_area_trigger_record(impl_->file.area_triggers[i], i));
	}
	return out;
}

bool MissionDocument::add_area_trigger(const AreaTriggerRecord &record, AreaTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	impl_->file.area_triggers.push_back(from_area_trigger_record(record));
	sync_counts();
	if (out != nullptr) {
		*out = to_area_trigger_record(impl_->file.area_triggers.back(), impl_->file.area_triggers.size() - 1);
	}
	return true;
}

bool MissionDocument::set_area_trigger(size_t index, const AreaTriggerRecord &record, AreaTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.area_triggers.size()) {
		impl_->last_error = "Area trigger index out of range";
		return false;
	}
	impl_->file.area_triggers[index] = from_area_trigger_record(record);
	if (out != nullptr) {
		*out = to_area_trigger_record(impl_->file.area_triggers[index], index);
	}
	return true;
}

bool MissionDocument::remove_area_trigger(size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.area_triggers.size()) {
		impl_->last_error = "Area trigger index out of range";
		return false;
	}
	// Repair *IsWithinArea references. Phase-5 RE confirmed param2 is the area-trigger ARRAY INDEX
	// (Entity_IsTeamInTriggerBounds @0x43c730: &unk_A32D10 + 32*param2), so removing a zone shifts every
	// higher index down by one. A reference to the removed zone becomes -1 (dangling), which
	// get_event_chain then flags. [orig: zone bounds consumers @0x43c730 / @0x43e510]
	const int removed = static_cast<int>(index);
	for (bms::Trigger &t : impl_->file.triggers) {
		const bool area_trigger =
				(t.main_type == bms::TriggerMainType::Group &&
						t.sub_type == static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea)) ||
				(t.main_type == bms::TriggerMainType::Single &&
						t.sub_type == static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea));
		if (!area_trigger) {
			continue;
		}
		if (t.param2 == removed) {
			t.param2 = -1;  // the referenced zone is gone
		} else if (t.param2 > removed) {
			--t.param2;     // zones above the hole shifted down
		}
	}
	impl_->file.area_triggers.erase(impl_->file.area_triggers.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts();
	return true;
}

// Append one NUL-terminated string to a loadout byte buffer.
static void append_loadout_field(std::vector<uint8_t> &raw, const std::string &value) {
	raw.insert(raw.end(), value.begin(), value.end());
	raw.push_back(0);
}

std::vector<WeaponLoadoutEntry> MissionDocument::weapon_loadout() const {
	std::vector<WeaponLoadoutEntry> out;
	if (!impl_->loaded) {
		return out;
	}
	const std::vector<uint8_t> &raw = impl_->file.loadout.raw_data;
	size_t pos = 0;
	// Walk records until an empty record (a leading NUL = the terminator) or the buffer is exhausted,
	// reading exactly three NUL-terminated strings each (name, value1, value2). A short/unterminated
	// tail stops the walk without emitting a partial record. [orig: Mission_LoadBMSFile @0x40f7b6;
	// see notes for the loader's 4th-string over-read, which is a filter quirk, not the file format.]
	while (pos < raw.size() && raw[pos] != 0) {
		WeaponLoadoutEntry entry;
		std::string *fields[3] = { &entry.name, &entry.value1, &entry.value2 };
		bool complete = true;
		for (int f = 0; f < 3; ++f) {
			const size_t start = pos;
			while (pos < raw.size() && raw[pos] != 0) {
				++pos;
			}
			if (pos >= raw.size()) {
				complete = false;
				break;
			}
			fields[f]->assign(reinterpret_cast<const char *>(raw.data() + start), pos - start);
			++pos;  // skip the NUL terminator
		}
		if (!complete) {
			break;
		}
		out.push_back(std::move(entry));
	}
	return out;
}

bool MissionDocument::set_weapon_loadout(const std::vector<WeaponLoadoutEntry> &entries) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<uint8_t> raw;
	for (const WeaponLoadoutEntry &entry : entries) {
		// An empty name serializes to a leading NUL, which the loader reads as the chunk terminator,
		// dropping this entry AND every entry after it. The format cannot represent a nameless
		// weapon, so skip it rather than corrupt the chunk (the editor seeds new rows with a name).
		if (entry.name.empty()) {
			continue;
		}
		append_loadout_field(raw, entry.name);
		append_loadout_field(raw, entry.value1);
		append_loadout_field(raw, entry.value2);
	}
	// The loader walks records while the next byte is non-NUL, so a non-empty chunk needs a trailing
	// empty record (one extra NUL) to terminate. An empty loadout serializes to an empty chunk (len 0),
	// which the loader treats as "no restrictions" (it installs the WPN_KNIFE default at runtime).
	if (!raw.empty()) {
		raw.push_back(0);
	}
	impl_->file.loadout.raw_data = std::move(raw);
	sync_counts();
	return true;
}

size_t MissionDocument::group_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.group_records.size();
}

bool MissionDocument::get_group(size_t index, GroupFields &out) const {
	if (!impl_->loaded || index >= impl_->file.group_records.size()) {
		return false;
	}
	const uint8_t *raw = impl_->file.group_records[index].raw_data;
	out.index = index;
	out.field0 = read_i32_at(raw, 0);
	out.field8 = read_i32_at(raw, 8);
	out.field12 = read_i32_at(raw, 12);
	return true;
}

std::vector<GroupFields> MissionDocument::groups() const {
	std::vector<GroupFields> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.group_records.size());
	for (size_t i = 0; i < impl_->file.group_records.size(); ++i) {
		GroupFields g;
		get_group(i, g);
		out.push_back(g);
	}
	return out;
}

bool MissionDocument::set_group(size_t index, int field0, int field8, int field12) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.group_records.size()) {
		impl_->last_error = "Group index out of range";
		return false;
	}
	// Only the three loader-consumed ints are written; the other 20 bytes are preserved so the record
	// round-trips. [orig: Mission_LoadBMSFile @0x40fbbb keeps offsets 0/8/12]
	uint8_t *raw = impl_->file.group_records[index].raw_data;
	write_i32_at(raw, 0, static_cast<int32_t>(field0));
	write_i32_at(raw, 8, static_cast<int32_t>(field8));
	write_i32_at(raw, 12, static_cast<int32_t>(field12));
	return true;
}

size_t MissionDocument::event_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.events.size();
}

bool MissionDocument::get_event(size_t index, MissionEventRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.events.size()) {
		return false;
	}
	out = to_event_record(impl_->file.events[index], index);
	return true;
}

std::vector<MissionEventRecord> MissionDocument::events() const {
	std::vector<MissionEventRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.events.size());
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		out.push_back(to_event_record(impl_->file.events[i], i));
	}
	return out;
}

bool MissionDocument::set_event(size_t index, const MissionEventRecord &record, MissionEventRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	apply_event_record(impl_->file.events[index], record);
	if (out != nullptr) {
		*out = to_event_record(impl_->file.events[index], index);
	}
	return true;
}

size_t MissionDocument::trigger_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.triggers.size();
}

bool MissionDocument::get_trigger(size_t index, MissionTriggerRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.triggers.size()) {
		return false;
	}
	out = to_trigger_record(impl_->file.triggers[index], index);
	return true;
}

std::vector<MissionTriggerRecord> MissionDocument::triggers() const {
	std::vector<MissionTriggerRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.triggers.size());
	for (size_t i = 0; i < impl_->file.triggers.size(); ++i) {
		out.push_back(to_trigger_record(impl_->file.triggers[i], i));
	}
	return out;
}

bool MissionDocument::set_trigger(size_t index, const MissionTriggerRecord &record, MissionTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.triggers.size()) {
		impl_->last_error = "Mission trigger index out of range";
		return false;
	}
	impl_->file.triggers[index] = trigger_from_record(record);
	if (out != nullptr) {
		*out = to_trigger_record(impl_->file.triggers[index], index);
	}
	return true;
}

size_t MissionDocument::action_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.actions.size();
}

bool MissionDocument::get_action(size_t index, MissionActionRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.actions.size()) {
		return false;
	}
	out = to_action_record(impl_->file.actions[index], index);
	return true;
}

std::vector<MissionActionRecord> MissionDocument::actions() const {
	std::vector<MissionActionRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.actions.size());
	for (size_t i = 0; i < impl_->file.actions.size(); ++i) {
		out.push_back(to_action_record(impl_->file.actions[i], i));
	}
	return out;
}

bool MissionDocument::set_action(size_t index, const MissionActionRecord &record, MissionActionRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.actions.size()) {
		impl_->last_error = "Mission action index out of range";
		return false;
	}
	impl_->file.actions[index] = action_from_record(record);
	if (out != nullptr) {
		*out = to_action_record(impl_->file.actions[index], index);
	}
	return true;
}

bool MissionDocument::insert_event_trigger(size_t event_index, size_t local_index, const MissionTriggerRecord &record, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (event.trigger_count >= kMaxEventChainEntries) {
		impl_->last_error = "Mission event trigger count exceeds 20";
		return false;
	}
	if (local_index > event.trigger_count) {
		impl_->last_error = "Mission event trigger insert index out of range";
		return false;
	}
	if (event.trigger_count > 0 && !valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = event.trigger_count == 0 ? impl_->file.triggers.size() : static_cast<size_t>(event.trigger_index) + local_index;
	impl_->file.triggers.insert(impl_->file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index), trigger_from_record(record));
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.trigger_count > 0 && other.trigger_index >= static_cast<int32_t>(global_index)) {
			other.trigger_index += 1;
		}
	}
	if (event.trigger_count == 0) {
		event.trigger_index = static_cast<int32_t>(global_index);
	}
	event.trigger_count = static_cast<uint8_t>(event.trigger_count + 1);
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::remove_event_trigger(size_t event_index, size_t local_index, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (local_index >= event.trigger_count) {
		impl_->last_error = "Mission event trigger index out of range";
		return false;
	}
	if (!valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(event.trigger_index) + local_index;
	impl_->file.triggers.erase(impl_->file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index));
	event.trigger_count = static_cast<uint8_t>(event.trigger_count - 1);
	if (event.trigger_count == 0) {
		event.trigger_index = 0;
	}
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.trigger_count > 0 && other.trigger_index > static_cast<int32_t>(global_index)) {
			other.trigger_index -= 1;
		}
	}
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::move_event_trigger(size_t event_index, size_t local_index, int delta, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= event.trigger_count || next_local < 0 || next_local >= event.trigger_count) {
		impl_->last_error = "Mission event trigger move index out of range";
		return false;
	}
	if (!valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(event.trigger_index);
	std::swap(impl_->file.triggers[first + local_index], impl_->file.triggers[first + static_cast<size_t>(next_local)]);
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::insert_event_action(size_t event_index, size_t local_index, const MissionActionRecord &record, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (event.action_count >= kMaxEventChainEntries) {
		impl_->last_error = "Mission event action count exceeds 20";
		return false;
	}
	if (local_index > event.action_count) {
		impl_->last_error = "Mission event action insert index out of range";
		return false;
	}
	if (event.action_count > 0 && !valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = event.action_count == 0 ? impl_->file.actions.size() : static_cast<size_t>(event.action_index) + local_index;
	impl_->file.actions.insert(impl_->file.actions.begin() + static_cast<std::ptrdiff_t>(global_index), action_from_record(record));
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.action_count > 0 && other.action_index >= static_cast<int32_t>(global_index)) {
			other.action_index += 1;
		}
	}
	if (event.action_count == 0) {
		event.action_index = static_cast<int32_t>(global_index);
	}
	event.action_count = static_cast<uint8_t>(event.action_count + 1);
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::remove_event_action(size_t event_index, size_t local_index, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (local_index >= event.action_count) {
		impl_->last_error = "Mission event action index out of range";
		return false;
	}
	if (!valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(event.action_index) + local_index;
	impl_->file.actions.erase(impl_->file.actions.begin() + static_cast<std::ptrdiff_t>(global_index));
	event.action_count = static_cast<uint8_t>(event.action_count - 1);
	if (event.action_count == 0) {
		event.action_index = 0;
	}
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.action_count > 0 && other.action_index > static_cast<int32_t>(global_index)) {
			other.action_index -= 1;
		}
	}
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::move_event_action(size_t event_index, size_t local_index, int delta, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= event.action_count || next_local < 0 || next_local >= event.action_count) {
		impl_->last_error = "Mission event action move index out of range";
		return false;
	}
	if (!valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(event.action_index);
	std::swap(impl_->file.actions[first + local_index], impl_->file.actions[first + static_cast<size_t>(next_local)]);
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::add_event(const MissionEventRecord &record, MissionEventRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Event event = {};
	apply_event_record(event, record);
	// A fresh event owns no triggers/actions; the index/count fields stay zero until the caller adds
	// entries via insert_event_trigger/insert_event_action (each assigns the global index on first add).
	event.trigger_index = 0;
	event.action_index = 0;
	event.trigger_count = 0;
	event.action_count = 0;
	impl_->file.events.push_back(event);
	sync_counts();
	if (out != nullptr) {
		*out = to_event_record(impl_->file.events.back(), impl_->file.events.size() - 1);
	}
	return true;
}

bool MissionDocument::remove_event(size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	// Drain the event's triggers and actions through the single-element removers, which fix up every
	// other event's trigger_index / action_index exactly as a normal trigger/action delete would. The
	// break is an infinite-loop guard: a remover only fails on an already-malformed (invalid-range) event.
	while (impl_->file.events[index].trigger_count > 0) {
		if (!remove_event_trigger(index, 0)) {
			break;
		}
	}
	while (impl_->file.events[index].action_count > 0) {
		if (!remove_event_action(index, 0)) {
			break;
		}
	}
	// Repair ResetEvent action references (param1 = event index, the one proven cross-reference): events
	// after the hole shift down by one; a reference to the removed event becomes dangling (-1), which
	// get_event_chain then flags as out-of-range. (Area-trigger refs are left alone because their index
	// semantics are still under RE; here the semantics are proven, so the repair is safe.)
	for (bms::Action &action : impl_->file.actions) {
		if (action.action_type != bms::ActionType::ResetEvent) {
			continue;
		}
		if (action.param1 > static_cast<int32_t>(index)) {
			action.param1 -= 1;
		} else if (action.param1 == static_cast<int32_t>(index)) {
			action.param1 = -1;
		}
	}
	impl_->file.events.erase(impl_->file.events.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts();
	return true;
}

bool MissionDocument::get_event_chain(size_t index, MissionEventChain &out) const {
	out = {};
	if (!impl_->loaded || index >= impl_->file.events.size()) {
		return false;
	}
	out.event = to_event_record(impl_->file.events[index], index);
	if (!valid_range(out.event.trigger_index, out.event.trigger_count, impl_->file.triggers.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.trigger_range_out_of_range",
				"Event trigger range is outside the mission trigger table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.trigger_count; ++i) {
			const size_t trigger_index = static_cast<size_t>(out.event.trigger_index + i);
			MissionTriggerRecord trigger = to_trigger_record(impl_->file.triggers[trigger_index], trigger_index);
			out.triggers.push_back(trigger);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "trigger", static_cast<int>(trigger_index), 0, static_cast<int>(trigger_index), "trigger", true));
			add_trigger_area_reference(trigger, impl_->file.area_triggers.size(), out);
		}
	}
	if (!valid_range(out.event.action_index, out.event.action_count, impl_->file.actions.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.action_range_out_of_range",
				"Event action range is outside the mission action table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.action_count; ++i) {
			const size_t action_index = static_cast<size_t>(out.event.action_index + i);
			MissionActionRecord action = to_action_record(impl_->file.actions[action_index], action_index);
			out.actions.push_back(action);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "action", static_cast<int>(action_index), 0, static_cast<int>(action_index), "action", true));
			if (action.action_type == static_cast<int>(bms::ActionType::ResetEvent)) {
				const bool valid = action.param1 >= 0 && static_cast<size_t>(action.param1) < impl_->file.events.size();
				out.references.push_back(logic_reference("action", static_cast<int>(action_index), "event", action.param1, 1, action.param1, "reset event", valid));
				if (!valid) {
					out.diagnostics.push_back(logic_diagnostic(
							"logic.event_reference_out_of_range",
							"Action references an event index outside the mission event table.",
							"action",
							static_cast<int>(action_index)));
				}
			}
		}
	}
	return true;
}

MissionLogicSummary MissionDocument::logic_summary() const {
	MissionLogicSummary out;
	if (!impl_->loaded) {
		return out;
	}
	out.event_count = impl_->file.events.size();
	out.trigger_count = impl_->file.triggers.size();
	out.action_count = impl_->file.actions.size();
	out.area_trigger_count = impl_->file.area_triggers.size();
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		MissionEventChain chain;
		if (get_event_chain(i, chain)) {
			out.diagnostic_count += chain.diagnostics.size();
		}
	}
	return out;
}

namespace {

// True when a name-mapping switch named the probed value (anything other than the "Unknown(N)" fallback
// from unknown_label). The reflectors below keep only named values, so the dropdowns track bms.h.
bool is_named_enum_value(const std::string &name) {
	return name.rfind("Unknown(", 0) != 0;
}

} // namespace

std::vector<MissionEnumEntry> MissionDocument::trigger_main_types() const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 15; ++value) {
		std::string name = trigger_main_type_name(value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::trigger_sub_types(int main_type) const {
	std::vector<MissionEnumEntry> out;
	// Sub-type values are non-contiguous (e.g. SingleTriggerType jumps 17 -> 42); probe wide and keep
	// the named ones so the editor lists exactly the engine's accepted sub-types for this main type.
	for (int value = 0; value <= 63; ++value) {
		std::string name = trigger_sub_type_name(main_type, value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::action_types() const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 63; ++value) {
		std::string name = action_type_name(value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::action_sub_types(int action_type) const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 63; ++value) {
		std::string name = action_sub_type_name(action_type, value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::event_flag_bits() const {
	// EventFlags (bms.h): ResetAfter=1, PreMission=2, PostMission=4, plus two unnamed bits surfaced raw so
	// a mission's unknown event flags survive a round-trip edit.
	return {
			{static_cast<int>(bms::EventFlags::ResetAfter), "Reset after"},
			{static_cast<int>(bms::EventFlags::PreMission), "Pre-mission"},
			{static_cast<int>(bms::EventFlags::PostMission), "Post-mission"},
			{static_cast<int>(bms::EventFlags::Unknown4), "Unknown (16)"},
			{static_cast<int>(bms::EventFlags::Unknown5), "Unknown (32)"},
	};
}

const bms::File &MissionDocument::bms_file() const {
	return impl_->file;
}

bms::File &MissionDocument::bms_file() {
	return impl_->file;
}

void MissionDocument::sync_counts() {
	impl_->file.header.num_items = static_cast<uint32_t>(impl_->file.items.size());
	impl_->file.header.num_buildings = static_cast<uint32_t>(impl_->file.buildings.size());
	impl_->file.header.num_markers = static_cast<uint32_t>(impl_->file.markers.size());
	impl_->file.header.num_people = static_cast<uint32_t>(impl_->file.organics.size());
	impl_->file.header.num_events = static_cast<uint32_t>(impl_->file.events.size());
	impl_->file.header.area_trigger_count = static_cast<int16_t>(impl_->file.area_triggers.size());
	impl_->file.header.weapon_loadout_chunk_len = static_cast<uint16_t>(impl_->file.loadout.raw_data.size());
	impl_->file.header.secondary_chunk_len = static_cast<uint16_t>(impl_->file.secondary_chunk.size());
	impl_->file.events_count = static_cast<int32_t>(impl_->file.events.size());
	impl_->file.trigger_count = static_cast<int32_t>(impl_->file.triggers.size());
	impl_->file.action_count = static_cast<int32_t>(impl_->file.actions.size());
	impl_->file.bounding_box_count = static_cast<int32_t>(impl_->file.bounding_boxes.size());

	if (impl_->file.waypoint_records.empty()) {
		impl_->file.waypoint_records.resize(bms::kWaypointRecordCount);
	}
	if (impl_->file.group_records.empty()) {
		impl_->file.group_records.resize(bms::kGroupRecordCount);
	}
	if (impl_->file.layer_records.empty()) {
		impl_->file.layer_records.resize(bms::kLayerRecordCount);
	}
}

} // namespace opennova::mission

struct OpenNovaMissionDocument {
	opennova::mission::MissionDocument document;
};

namespace opennova::mission {

extern "C" {

MISSION_EXPORT OpenNovaMissionDocument *opennova_mission_create(void) {
	return new (std::nothrow) OpenNovaMissionDocument();
}

MISSION_EXPORT void opennova_mission_destroy(OpenNovaMissionDocument *document) {
	delete document;
}

void opennova_mission_clear(OpenNovaMissionDocument *document) {
	if (document != nullptr) {
		document->document.clear();
	}
}

MISSION_EXPORT int opennova_mission_load_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	return document->document.load_bms_file(path) ? 1 : 0;
}

int opennova_mission_load_bytes(OpenNovaMissionDocument *document, const uint8_t *data, size_t size) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.load_bms_bytes(data, size) ? 1 : 0;
}

int opennova_mission_save_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	return document->document.save_bms_file(path) ? 1 : 0;
}

int opennova_mission_write_bytes(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes) {
	if (document == nullptr || out_bytes == nullptr) {
		return 0;
	}
	out_bytes->data = nullptr;
	out_bytes->size = 0;
	std::vector<uint8_t> bytes;
	if (!document->document.write_bms_bytes(bytes)) {
		return 0;
	}
	if (!bytes.empty()) {
		out_bytes->data = new (std::nothrow) uint8_t[bytes.size()];
		if (out_bytes->data == nullptr) {
			return 0;
		}
		std::memcpy(out_bytes->data, bytes.data(), bytes.size());
	}
	out_bytes->size = bytes.size();
	return 1;
}

MISSION_EXPORT int opennova_mission_save_mis_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	return document->document.save_mis_file(path) ? 1 : 0;
}

MISSION_EXPORT int opennova_mission_write_mis_text(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes) {
	if (document == nullptr || out_bytes == nullptr) {
		return 0;
	}
	out_bytes->data = nullptr;
	out_bytes->size = 0;
	std::string text;
	if (!document->document.write_mis_text(text)) {
		return 0;
	}
	uint8_t *data = new (std::nothrow) uint8_t[text.size()];
	if (data == nullptr && !text.empty()) {
		return 0;
	}
	if (!text.empty()) {
		std::memcpy(data, text.data(), text.size());
	}
	out_bytes->data = data;
	out_bytes->size = text.size();
	return 1;
}

MISSION_EXPORT void opennova_mission_free_bytes(OpenNovaMissionBytes *bytes) {
	if (bytes == nullptr) {
		return;
	}
	delete[] bytes->data;
	bytes->data = nullptr;
	bytes->size = 0;
}

int opennova_mission_is_loaded(const OpenNovaMissionDocument *document) {
	return document != nullptr && document->document.is_loaded() ? 1 : 0;
}

const char *opennova_mission_source_path(const OpenNovaMissionDocument *document) {
	if (document == nullptr) {
		return "";
	}
	return document->document.source_path().c_str();
}

MISSION_EXPORT const char *opennova_mission_last_error(const OpenNovaMissionDocument *document) {
	if (document == nullptr) {
		return "Invalid mission document";
	}
	return document->document.last_error().c_str();
}

int opennova_mission_get_info(const OpenNovaMissionDocument *document, OpenNovaMissionInfo *out_info) {
	if (document == nullptr || out_info == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	std::memset(out_info, 0, sizeof(*out_info));
	const opennova::mission::MissionInfo info = document->document.info();
	copy_cstr(out_info->mission_name, sizeof(out_info->mission_name), info.mission_name);
	copy_cstr(out_info->designer, sizeof(out_info->designer), info.designer);
	copy_cstr(out_info->briefing, sizeof(out_info->briefing), info.briefing);
	copy_cstr(out_info->terrain, sizeof(out_info->terrain), info.terrain);
	copy_cstr(out_info->environment, sizeof(out_info->environment), info.environment);
	out_info->climate = info.climate;
	out_info->weather = info.weather;
	out_info->attrib_flags = info.attrib_flags;
	out_info->start_time = info.start_time;
	out_info->minutes_per_day = info.minutes_per_day;
	out_info->player_health = info.player_health;
	out_info->max_saves = info.max_saves;
	out_info->music = info.music;
	out_info->reverb = info.reverb;
	return 1;
}

size_t opennova_mission_entity_count(const OpenNovaMissionDocument *document, int kind) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.entity_count(entity_kind);
}

int opennova_mission_get_entity(const OpenNovaMissionDocument *document,
                                int kind,
                                size_t index,
                                OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.get_entity(entity_kind, index, record)) {
		return 0;
	}
	copy_record(*out_record, record);
	return 1;
}

int opennova_mission_set_entity_transform(OpenNovaMissionDocument *document,
                                          int kind,
                                          size_t index,
                                          const OpenNovaMissionEntityTransform *transform) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.set_entity_transform(entity_kind, index, from_c_transform(*transform)) ? 1 : 0;
}

int opennova_mission_set_entity_properties(OpenNovaMissionDocument *document,
                                           int kind,
                                           size_t index,
                                           const OpenNovaMissionEntityProperties *properties,
                                           OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || properties == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.set_entity_properties(entity_kind, index, from_c_properties(*properties), &record)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_record(*out_record, record);
	}
	return 1;
}

int opennova_mission_add_entity(OpenNovaMissionDocument *document,
                                int kind,
                                int item_id,
                                const OpenNovaMissionEntityTransform *transform,
                                OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.add_entity(entity_kind, item_id, from_c_transform(*transform), &record)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_record(*out_record, record);
	}
	return 1;
}

int opennova_mission_remove_entity(OpenNovaMissionDocument *document, int kind, size_t index) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.remove_entity(entity_kind, index) ? 1 : 0;
}

size_t opennova_mission_waypoint_summary_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.waypoint_summaries().size();
}

int opennova_mission_get_waypoint_summary(const OpenNovaMissionDocument *document,
                                          size_t index,
                                          OpenNovaMissionWaypointSummary *out_summary) {
	if (document == nullptr || out_summary == nullptr) {
		return 0;
	}
	const std::vector<opennova::mission::WaypointSummary> summaries = document->document.waypoint_summaries();
	if (index >= summaries.size()) {
		return 0;
	}
	copy_waypoint_summary(*out_summary, summaries[index]);
	return 1;
}

size_t opennova_mission_waypoint_path_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.waypoint_path_count();
}

int opennova_mission_get_waypoint_path(const OpenNovaMissionDocument *document,
                                       size_t index,
                                       OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || out_path == nullptr) {
		return 0;
	}
	opennova::mission::WaypointPath path;
	if (!document->document.get_waypoint_path(index, path)) {
		return 0;
	}
	copy_waypoint_path(*out_path, path);
	return 1;
}

int opennova_mission_set_waypoint_path(OpenNovaMissionDocument *document,
                                       size_t index,
                                       const uint32_t *marker_indices,
                                       size_t marker_count,
                                       int flags,
                                       OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || (marker_indices == nullptr && marker_count > 0)) {
		return 0;
	}
	std::vector<int> indices;
	indices.reserve(marker_count);
	for (size_t i = 0; i < marker_count; ++i) {
		if (marker_indices[i] > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
			return 0;
		}
		indices.push_back(static_cast<int>(marker_indices[i]));
	}
	opennova::mission::WaypointPath path;
	if (!document->document.set_waypoint_path(index, indices, flags, &path)) {
		return 0;
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

int opennova_mission_clear_waypoint_path(OpenNovaMissionDocument *document,
                                         size_t index,
                                         OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::WaypointPath path;
	if (!document->document.clear_waypoint_path(index, &path)) {
		return 0;
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

int opennova_mission_add_waypoint_marker(OpenNovaMissionDocument *document,
                                         size_t path_index,
                                         int marker_item_id,
                                         const OpenNovaMissionEntityTransform *transform,
                                         int insert_index,
                                         OpenNovaMissionEntityRecord *out_marker,
                                         OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityRecord marker;
	opennova::mission::WaypointPath path;
	if (!document->document.add_waypoint_marker(
				path_index,
				marker_item_id,
				from_c_transform(*transform),
				insert_index,
				&marker,
				&path)) {
		return 0;
	}
	if (out_marker != nullptr) {
		copy_record(*out_marker, marker);
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

size_t opennova_mission_area_trigger_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.area_trigger_count();
}

int opennova_mission_get_area_trigger(const OpenNovaMissionDocument *document,
                                      size_t index,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord record;
	if (!document->document.get_area_trigger(index, record)) {
		return 0;
	}
	copy_area_trigger(*out_record, record);
	return 1;
}

int opennova_mission_add_area_trigger(OpenNovaMissionDocument *document,
                                      const OpenNovaMissionAreaTriggerRecord *record,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord out;
	if (!document->document.add_area_trigger(from_c_area_trigger(*record), &out)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_area_trigger(*out_record, out);
	}
	return 1;
}

int opennova_mission_set_area_trigger(OpenNovaMissionDocument *document,
                                      size_t index,
                                      const OpenNovaMissionAreaTriggerRecord *record,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord out;
	if (!document->document.set_area_trigger(index, from_c_area_trigger(*record), &out)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_area_trigger(*out_record, out);
	}
	return 1;
}

int opennova_mission_remove_area_trigger(OpenNovaMissionDocument *document, size_t index) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.remove_area_trigger(index) ? 1 : 0;
}

size_t opennova_mission_weapon_loadout_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.weapon_loadout().size();
}

int opennova_mission_get_weapon_loadout_entry(const OpenNovaMissionDocument *document,
                                              size_t index,
                                              OpenNovaMissionWeaponLoadoutEntry *out_entry) {
	if (document == nullptr || out_entry == nullptr) {
		return 0;
	}
	const std::vector<WeaponLoadoutEntry> entries = document->document.weapon_loadout();
	if (index >= entries.size()) {
		return 0;
	}
	const WeaponLoadoutEntry &entry = entries[index];
	copy_cstr(out_entry->name, sizeof(out_entry->name), entry.name);
	copy_cstr(out_entry->value1, sizeof(out_entry->value1), entry.value1);
	copy_cstr(out_entry->value2, sizeof(out_entry->value2), entry.value2);
	return 1;
}

int opennova_mission_set_weapon_loadout(OpenNovaMissionDocument *document,
                                        const OpenNovaMissionWeaponLoadoutEntry *entries,
                                        size_t count) {
	if (document == nullptr || (entries == nullptr && count > 0)) {
		return 0;
	}
	std::vector<WeaponLoadoutEntry> records;
	records.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		WeaponLoadoutEntry record;
		record.name = fixed_string(entries[i].name, sizeof(entries[i].name));
		record.value1 = fixed_string(entries[i].value1, sizeof(entries[i].value1));
		record.value2 = fixed_string(entries[i].value2, sizeof(entries[i].value2));
		records.push_back(std::move(record));
	}
	return document->document.set_weapon_loadout(records) ? 1 : 0;
}

size_t opennova_mission_group_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.group_count();
}

int opennova_mission_get_group(const OpenNovaMissionDocument *document,
                               size_t index,
                               OpenNovaMissionGroupRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	GroupFields fields;
	if (!document->document.get_group(index, fields)) {
		return 0;
	}
	out_record->index = fields.index;
	out_record->field0 = fields.field0;
	out_record->field8 = fields.field8;
	out_record->field12 = fields.field12;
	return 1;
}

int opennova_mission_set_group(OpenNovaMissionDocument *document,
                               size_t index,
                               int field0, int field8, int field12) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.set_group(index, field0, field8, field12) ? 1 : 0;
}

size_t opennova_mission_event_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.event_count();
}

int opennova_mission_get_event(const OpenNovaMissionDocument *document,
                               size_t index,
                               OpenNovaMissionEventRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionEventRecord record;
	if (!document->document.get_event(index, record)) {
		return 0;
	}
	copy_event(*out_record, record);
	return 1;
}

size_t opennova_mission_trigger_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.trigger_count();
}

int opennova_mission_get_trigger(const OpenNovaMissionDocument *document,
                                 size_t index,
                                 OpenNovaMissionTriggerRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionTriggerRecord record;
	if (!document->document.get_trigger(index, record)) {
		return 0;
	}
	copy_trigger(*out_record, record);
	return 1;
}

size_t opennova_mission_action_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.action_count();
}

int opennova_mission_get_action(const OpenNovaMissionDocument *document,
                                size_t index,
                                OpenNovaMissionActionRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionActionRecord record;
	if (!document->document.get_action(index, record)) {
		return 0;
	}
	copy_action(*out_record, record);
	return 1;
}

int opennova_mission_get_logic_summary(const OpenNovaMissionDocument *document,
                                       OpenNovaMissionLogicSummary *out_summary) {
	if (document == nullptr || out_summary == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	copy_logic_summary(*out_summary, document->document.logic_summary());
	return 1;
}

} // extern "C"

} // namespace opennova::mission
