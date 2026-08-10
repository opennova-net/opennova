#include "mission_mis.h"

// Split out of mission.cpp (quality campaign W3-1). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// Reads the .mis text form back into a bms::File. Tokenizer and per-section parsers
// are private here; mission_mis_writer.cpp is the inverse.

#include "mission_detail.h"
#include "mission_records.h"

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace opennova::mission::detail {

namespace {

struct MisLine {
	size_t line_no = 0;
	std::vector<std::string> tokens;
};

std::string trim_copy(const std::string &value) { return opennova::strutil::trim(value); }

std::vector<std::string> tokenize_mis_line(const std::string &line) {
	std::vector<std::string> tokens;
	std::string current;
	bool in_quote = false;
	for (size_t i = 0; i < line.size(); ++i) {
		const char ch = line[i];
		if (!in_quote && ch == '/' && i + 1 < line.size() && line[i + 1] == '/') {
			break;
		}
		if (ch == '"') {
			if (in_quote) {
				tokens.push_back(current);
				current.clear();
				in_quote = false;
			} else {
				if (!current.empty()) {
					tokens.push_back(current);
					current.clear();
				}
				in_quote = true;
			}
			continue;
		}
		if (!in_quote && std::isspace(static_cast<unsigned char>(ch))) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
			continue;
		}
		current.push_back(ch);
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

std::vector<MisLine> tokenize_mis_text(const std::string &text) {
	std::vector<MisLine> lines;
	size_t line_start = 0;
	size_t line_no = 1;
	while (line_start <= text.size()) {
		const size_t line_end = text.find('\n', line_start);
		std::string raw = line_end == std::string::npos
				? text.substr(line_start)
				: text.substr(line_start, line_end - line_start);
		if (!raw.empty() && raw.back() == '\r') {
			raw.pop_back();
		}
		const std::vector<std::string> tokens = tokenize_mis_line(trim_copy(raw));
		if (!tokens.empty()) {
			lines.push_back({line_no, tokens});
		}
		if (line_end == std::string::npos) {
			break;
		}
		line_start = line_end + 1;
		++line_no;
	}
	return lines;
}

// All .mis numerics parse base-10, matching the original importer's plain atol (a base-0 strtol
// read zero-padded values as OCTAL: "0120" -> 80, degrading to 0 across round-trips). Our writer
// never emits hex, so there is no hex special case to keep.
// [orig: j__atol callers throughout MisLdr_ParseMisLine @ 0x100017b0, misldr.dll]
bool parse_i32_token(const std::string &token, int32_t &out) {
	char *end = nullptr;
	const long value = std::strtol(token.c_str(), &end, 10);
	if (end == token.c_str() || *end != '\0') {
		return false;
	}
	out = static_cast<int32_t>(value);
	return true;
}

bool parse_u32_token(const std::string &token, uint32_t &out) {
	char *end = nullptr;
	const unsigned long value = std::strtoul(token.c_str(), &end, 10);
	if (end == token.c_str() || *end != '\0') {
		return false;
	}
	out = static_cast<uint32_t>(value);
	return true;
}

bool parse_float_token(const std::string &token, float &out) {
	char *end = nullptr;
	const float value = std::strtof(token.c_str(), &end);
	if (end == token.c_str() || *end != '\0') {
		return false;
	}
	out = value;
	return true;
}

bool token_i32(const MisLine &line, size_t index, int32_t &out, std::string &error) {
	if (index >= line.tokens.size() || !parse_i32_token(line.tokens[index], out)) {
		error = "Invalid integer in MIS line " + std::to_string(line.line_no);
		return false;
	}
	return true;
}

bool token_u32(const MisLine &line, size_t index, uint32_t &out, std::string &error) {
	if (index >= line.tokens.size() || !parse_u32_token(line.tokens[index], out)) {
		error = "Invalid unsigned integer in MIS line " + std::to_string(line.line_no);
		return false;
	}
	return true;
}

bool token_float(const MisLine &line, size_t index, float &out, std::string &error) {
	if (index >= line.tokens.size() || !parse_float_token(line.tokens[index], out)) {
		error = "Invalid float in MIS line " + std::to_string(line.line_no);
		return false;
	}
	return true;
}

void mis_copy_fixed(char *dest, size_t dest_size, const std::string &value) {
	const size_t copy_len = std::min(dest_size, value.size());
	if (copy_len > 0) {
		std::memcpy(dest, value.data(), copy_len);
	}
	if (copy_len < dest_size) {
		std::memset(dest + copy_len, 0, dest_size - copy_len);
	}
}

void initialize_mis_file(bms::File &file) {
	file = {};
	file.header.magic[0] = 'B';
	file.header.magic[1] = 'M';
	file.header.magic[2] = 'S';
	file.header.magic[3] = static_cast<char>(bms::kMinVersion);
	file.waypoint_records.resize(bms::kWaypointRecordCount);
	file.group_records.resize(bms::kGroupRecordCount);
	file.layer_records.resize(bms::kLayerRecordCount);
	for (bms::WaypointRecord &record : file.waypoint_records) {
		resize_waypoint_padding(record, /*preserve_over_count=*/false);
	}
}

uint16_t hhmm_to_header_time(int32_t hhmm) {
	const int hours = std::clamp<int>(hhmm / 100, 0, 255);
	const int minutes = std::clamp<int>(hhmm % 100, 0, 59);
	const int frac = std::clamp<int>((minutes * 256 + 30) / 60, 0, 255);
	return static_cast<uint16_t>((hours << 8) | frac);
}

void set_header_packed_rgb(uint8_t rgb[3], uint32_t packed) {
	rgb[0] = static_cast<uint8_t>((packed >> 16) & 0xFF);
	rgb[1] = static_cast<uint8_t>((packed >> 8) & 0xFF);
	rgb[2] = static_cast<uint8_t>(packed & 0xFF);
}

bool parse_mis_general_information(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	bms::Header &header = file.header;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "general_information") {
			++pos;
			return true;
		}
		if (t.empty()) {
			continue;
		}
		const std::string &key = t[0];
		int32_t i32 = 0;
		uint32_t u32 = 0;
		float f32 = 0.0f;
		if (key == "file_version") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.magic[3] = static_cast<char>(std::clamp<int32_t>(i32, 0, 255));
		} else if (key == "name") {
			if (t.size() > 1) mis_copy_fixed(header.mission_name, sizeof(header.mission_name), t[1]);
		} else if (key == "designer") {
			if (t.size() > 1) mis_copy_fixed(header.designer, sizeof(header.designer), t[1]);
		} else if (key == "terrain") {
			if (t.size() > 1) mis_copy_fixed(header.terrain, 16, t[1]);
		} else if (key == "cnv_file") {
			if (t.size() > 1) mis_copy_fixed(header.terrain + 16, 16, t[1]);
		} else if (key == "tt_file") {
			if (t.size() > 1) mis_copy_fixed(header.terrain + 32, 16, t[1]);
		} else if (key == "terrain_color") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.climate = static_cast<bms::ClimateType>(i32);
		} else if (key == "attrib") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.attrib_flags = static_cast<bms::AttribFlags>(u32);
		} else if (key == "water_color") {
			if (!token_u32(line, 1, u32, error)) return false;
			set_header_packed_rgb(header.water_color, u32);
		} else if (key == "murk") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.murk = static_cast<uint16_t>(u32);
		} else if (key == "water_level") {
			// Symmetric with write_mis_general_information, which emits the RAW u32s at header
			// offsets 152/156. Those u32s straddle the named u16 fields (water_override @152,
			// unknown1 @154, fog_override @158), so store back through the same raw-offset helper
			// the writer reads with — the old u16 truncation zeroed real values (fog 45875200 ->
			// 0 on reload). The u16 override semantics themselves are untouched: the raw bytes
			// restored here are exactly the ones the overrides live in. (D-MIS-5)
			if (!token_u32(line, 1, u32, error)) return false;
			write_i32_at(reinterpret_cast<uint8_t *>(&header), 152, static_cast<int32_t>(u32));
		} else if (key == "fog_level") {
			if (!token_u32(line, 1, u32, error)) return false;
			write_i32_at(reinterpret_cast<uint8_t *>(&header), 156, static_cast<int32_t>(u32));
		} else if (key == "gen_def_val1") {
			// gen_def_val1..4 are the header u32s @264/268/272/276 the writer emits (bms.h names
			// them health/mana/music/reverb); they were write-only before, zeroing on reload. The
			// original importer parses the same key names [orig: MisLdr_ParseMisLine gen_def_val
			// handlers @ 0x1000257d..0x10002640 -> doc+0x40EEB0..BC, misldr.dll]. (D-MIS-5)
			if (!token_u32(line, 1, u32, error)) return false;
			header.health = u32;
		} else if (key == "gen_def_val2") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.mana = u32;
		} else if (key == "gen_def_val3") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.music = u32;
		} else if (key == "gen_def_val4") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.reverb = u32;
		} else if (key == "weather_type") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.weather_type = static_cast<bms::WeatherType>(i32);
		} else if (key == "sunset") {
			if (t.size() > 1) mis_copy_fixed(header.environment, sizeof(header.environment), t[1]);
		} else if (key == "start_time") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.start_time = hhmm_to_header_time(i32);
		} else if (key == "minutes_per_day") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.minutes_per_day = static_cast<uint16_t>(i32);
		} else if (key == "terrain_tile_tga") {
			if (t.size() > 1) mis_copy_fixed(header.terrain_tile, sizeof(header.terrain_tile), t[1]);
		} else if (key == "wind") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.wind_speed = u32;
			if (!token_u32(line, 2, u32, error)) return false;
			header.wind_direction = u32;
		} else if (key == "player_type") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.mission_type = static_cast<bms::MissionType>(static_cast<uint8_t>(i32));
		} else if (key == "max_saves") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.max_saves = static_cast<uint8_t>(std::clamp<int32_t>(i32, 0, 255));
		} else if (key == "map_zoom") {
			if (!token_float(line, 1, f32, error)) return false;
			header.map_zoom = f32;
		} else if (key == "bonus_expiration") {
			if (!token_u32(line, 1, u32, error)) return false;
			header.bonus_expiration = static_cast<uint16_t>(u32);
		} else if (key == "subgoals_win" && t.size() >= 9) {
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i + 1, i32, error)) return false;
				header.win_conditions[i] = static_cast<uint8_t>(std::clamp<int32_t>(i32, 0, 255));
			}
		} else if (key == "subgoals_lose" && t.size() >= 9) {
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i + 1, i32, error)) return false;
				header.lose_conditions[i] = static_cast<uint8_t>(std::clamp<int32_t>(i32, 0, 255));
			}
		} else if (key == "win_scores" && t.size() >= 9) {
			uint8_t *raw = reinterpret_cast<uint8_t *>(&header);
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i + 1, i32, error)) return false;
				raw[556 + i] = static_cast<uint8_t>(std::clamp<int32_t>(i32 / 100, 0, 255));
			}
		} else if (key == "lose_scores" && t.size() >= 9) {
			uint8_t *raw = reinterpret_cast<uint8_t *>(&header);
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i + 1, i32, error)) return false;
				raw[564 + i] = static_cast<uint8_t>(std::clamp<int32_t>(i32 / 100, 0, 255));
			}
		}
	}
	error = "MIS general_information section is missing its end marker";
	return false;
}

bool parse_mis_briefing(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	std::string briefing;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (!t.empty() && t[0] == "endbriefing") {
			mis_copy_fixed(file.header.mission_briefing, sizeof(file.header.mission_briefing), briefing);
			++pos;
			return true;
		}
		if (t.size() >= 2 && t[0] == "brief") {
			briefing += t[1];
		}
	}
	error = "MIS briefing section is missing endbriefing";
	return false;
}

bool skip_mis_section(const std::vector<MisLine> &lines, size_t &pos, const std::string &section, std::string &error) {
	for (++pos; pos < lines.size(); ++pos) {
		const std::vector<std::string> &t = lines[pos].tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == section) {
			++pos;
			return true;
		}
	}
	error = "MIS " + section + " section is missing its end marker";
	return false;
}

bool parse_mis_waypoint(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	int32_t index = 0;
	if (!token_i32(lines[pos], 2, index, error)) return false;
	if (index < 0 || index >= bms::kWaypointRecordCount) {
		error = "MIS waypoint index out of range in line " + std::to_string(lines[pos].line_no);
		return false;
	}
	bms::WaypointRecord &record = file.waypoint_records[static_cast<size_t>(index)];
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "waypoint") {
			resize_waypoint_padding(record, /*preserve_over_count=*/false);
			++pos;
			return true;
		}
		if (t.size() >= 2 && t[0] == "attrib") {
			uint32_t flags = 0;
			if (!token_u32(line, 1, flags, error)) return false;
			record.flags = static_cast<bms::WaypointFlags>(flags);
		}
	}
	error = "MIS waypoint section is missing its end marker";
	return false;
}

bool parse_mis_group(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	int32_t index = 0;
	if (!token_i32(lines[pos], 2, index, error)) return false;
	if (index < 0 || index >= bms::kGroupRecordCount) {
		error = "MIS group index out of range in line " + std::to_string(lines[pos].line_no);
		return false;
	}
	bms::GroupRecord &record = file.group_records[static_cast<size_t>(index)];
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "group") {
			++pos;
			return true;
		}
		int32_t value = 0;
		if (t.size() >= 2 && t[0] == "flags") {
			if (!token_i32(line, 1, value, error)) return false;
			record.flags = value;
		} else if (t.size() >= 2 && t[0] == "value") {
			if (!token_i32(line, 1, value, error)) return false;
			record.value = value;
		}
	}
	error = "MIS group section is missing its end marker";
	return false;
}

bool parse_mis_layer(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	int32_t index = 0;
	if (!token_i32(lines[pos], 2, index, error)) return false;
	if (index < 0 || index >= bms::kLayerRecordCount) {
		error = "MIS layer index out of range in line " + std::to_string(lines[pos].line_no);
		return false;
	}
	bms::LayerRecord &record = file.layer_records[static_cast<size_t>(index)];
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "layer") {
			++pos;
			return true;
		}
		if (t.size() >= 2 && t[0] == "description") {
			mis_copy_fixed(record.name, sizeof(record.name), t[1]);
		}
	}
	error = "MIS layer section is missing its end marker";
	return false;
}

bool parse_mis_area_trigger(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	int32_t id = 0;
	if (!token_i32(lines[pos], 2, id, error)) return false;
	bms::AreaTrigger trigger = {};
	trigger.id = id;
	for (++pos; pos < lines.size(); ++pos) {
		const std::vector<std::string> &t = lines[pos].tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "areatrig") {
			file.area_triggers.push_back(trigger);
			++pos;
			return true;
		}
	}
	error = "MIS areatrig section is missing its end marker";
	return false;
}

bool parse_mis_nested_records(const std::vector<MisLine> &lines,
                              size_t &pos,
                              const std::string &section,
                              int32_t expected_count,
                              bms::File &file,
                              std::string &error) {
	int32_t parsed = 0;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == section) {
			if (parsed != expected_count) {
				error = "MIS " + section + " count mismatch in line " + std::to_string(line.line_no);
				return false;
			}
			++pos;
			return true;
		}
		if (section == "triggers") {
			if (t.size() < 8) {
				error = "MIS trigger row is too short in line " + std::to_string(line.line_no);
				return false;
			}
			int32_t values[8] = {};
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i, values[i], error)) return false;
			}
			bms::Trigger trigger = {};
			trigger.condition_flags = values[0];
			trigger.main_type = static_cast<bms::TriggerMainType>(values[1]);
			trigger.sub_type = values[2];
			trigger.param1 = values[3];
			trigger.param2 = values[4];
			trigger.param3 = values[5];
			trigger.param4 = values[6];
			trigger.unknown7 = values[7];
			file.triggers.push_back(trigger);
		} else {
			if (t.size() < 8) {
				error = "MIS action row is too short in line " + std::to_string(line.line_no);
				return false;
			}
			int32_t values[8] = {};
			for (size_t i = 0; i < 8; ++i) {
				if (!token_i32(line, i, values[i], error)) return false;
			}
			bms::Action action = {};
			action.reserved0 = values[0];
			action.action_type = static_cast<bms::ActionType>(values[1]);
			action.param1 = values[2];
			action.param2 = values[3];
			action.param3 = values[4];
			action.param4 = values[5];
			action.action_sub_type = values[6];
			action.reserved1 = values[7];
			file.actions.push_back(action);
		}
		++parsed;
	}
	error = "MIS " + section + " block is missing its end marker";
	return false;
}

bool parse_mis_event(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	bms::Event event = {};
	event.trigger_index = static_cast<int32_t>(file.triggers.size());
	event.action_index = static_cast<int32_t>(file.actions.size());
	for (++pos; pos < lines.size();) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "event") {
			event.trigger_count = static_cast<uint8_t>(file.triggers.size() - static_cast<size_t>(event.trigger_index));
			event.action_count = static_cast<uint8_t>(file.actions.size() - static_cast<size_t>(event.action_index));
			file.events.push_back(event);
			++pos;
			return true;
		}
		int32_t value = 0;
		if (t.size() >= 2 && t[0] == "attrib") {
			if (!token_i32(line, 1, value, error)) return false;
			event.flags = static_cast<bms::EventFlags>(value);
			++pos;
		} else if (t.size() >= 2 && t[0] == "reset_value") {
			if (!token_i32(line, 1, value, error)) return false;
			event.reset_after = value;
			++pos;
		} else if (t.size() >= 2 && t[0] == "delay_value") {
			if (!token_i32(line, 1, value, error)) return false;
			event.delay = value;
			++pos;
		} else if (t.size() >= 3 && t[0] == "begin" && t[1] == "triggers") {
			if (!token_i32(line, 2, value, error)) return false;
			if (!parse_mis_nested_records(lines, pos, "triggers", value, file, error)) return false;
		} else if (t.size() >= 3 && t[0] == "begin" && t[1] == "actions") {
			if (!token_i32(line, 2, value, error)) return false;
			if (!parse_mis_nested_records(lines, pos, "actions", value, file, error)) return false;
		} else {
			++pos;
		}
	}
	error = "MIS event section is missing its end marker";
	return false;
}

// (next_entity_id used to be forward-declared here; it now comes from mission_records.h.)

bool parse_mis_item(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error) {
	bms::Entity entity = {};
	entity.type = bms::ItemType::Item;
	entity.perception2 = 100;
	entity.perfectionist2 = 100;
	entity.min_engagement_distance = 20;
	entity.max_engagement_distance = 200;
	entity.w_accuracy1 = 50;
	entity.w_accuracy2 = 50;
	entity.spawns = 1;
	entity.no_more_than = 1;
	entity.max_attack_distance = 100;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "item") {
			if (entity.id == 0) {
				entity.id = next_entity_id(file);
			}
			file.items.push_back(entity);
			++pos;
			return true;
		}
		if (t.empty()) {
			continue;
		}
		const std::string &key = t[0];
		int32_t v = 0;
		if (key == "type_id") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.type_id = v;
		} else if (key == "iai_name") {
			if (t.size() > 1) mis_copy_fixed(entity.name1, sizeof(entity.name1), t[1]);
		} else if (key == "ai_textfile") {
			if (t.size() > 1) mis_copy_fixed(entity.name2, sizeof(entity.name2), t[1]);
		} else if (key == "id") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.id = v;
		} else if (key == "position") {
			if (!token_i32(line, 1, entity.x, error)) return false;
			if (!token_i32(line, 2, entity.y, error)) return false;
			if (!token_i32(line, 3, entity.z, error)) return false;
		} else if (key == "facing") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.yaw = static_cast<int16_t>(v);
			if (!token_i32(line, 2, v, error)) return false;
			entity.pitch = static_cast<int16_t>(v);
			if (!token_i32(line, 3, v, error)) return false;
			entity.roll = static_cast<int16_t>(v);
		} else if (key == "team") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.team = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "map_symbol") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.map_symbol = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "name_index") {
			if (!token_i32(line, 1, entity.name_index, error)) return false;
		} else if (key == "team_budget") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.team_budget = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "bmsi_attributes") {
			uint32_t flags = 0;
			if (!token_u32(line, 1, flags, error)) return false;
			entity.bmsi_attributes = flags;
		} else if (key == "nolessthan") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.no_less_than = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "nomorethan") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.no_more_than = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "weapon_type") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.weapon_types = static_cast<int16_t>((entity.weapon_types & 0xFF00) | (v & 0xFF));
		} else if (key == "sweapon_type") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.weapon_types = static_cast<int16_t>((entity.weapon_types & 0x00FF) | ((v & 0xFF) << 8));
		} else if (key == "group_id") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.group_id = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "group_rel") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.group_rel_lo = static_cast<int16_t>(v & 0xFFFF);
			entity.group_rel_hi = static_cast<int16_t>((static_cast<uint32_t>(v) >> 16) & 0xFFFF);
		} else if (key == "waypoint_id") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.waypoint_id = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "wpnumber") {
			if (!token_i32(line, 1, entity.wp_number, error)) return false;
		} else if (key == "wpdistance") {
			if (!token_i32(line, 1, entity.wp_distance, error)) return false;
		} else if (key == "wp_adv_trigger") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.wp_adv_trigger = static_cast<int16_t>(v);
		} else if (key.rfind("wpgoal", 0) == 0 && key.size() == 7) {
			const int idx = key[6] - '0';
			if (idx >= 0 && idx < 4) {
				if (!token_i32(line, 1, v, error)) return false;
				const uint32_t mask = ~(0xFFu << (idx * 8));
				entity.wp_goals = static_cast<int32_t>((static_cast<uint32_t>(entity.wp_goals) & mask) |
				                                       ((static_cast<uint32_t>(v) & 0xFFu) << (idx * 8)));
			}
		} else if (key == "ttoolindex") {
			if (!token_i32(line, 1, entity.ttool_index, error)) return false;
		} else if (key == "alert_state") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.alert_state = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "obliqueness") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.obliqueness = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "waccuracy1") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.w_accuracy1 = static_cast<int16_t>(v);
		} else if (key == "waccuracy2") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.w_accuracy2 = static_cast<int16_t>(v);
		} else if (key == "perception2") {
			if (!token_i32(line, 1, entity.perception2, error)) return false;
		} else if (key == "perfectionist2") {
			if (!token_i32(line, 1, entity.perfectionist2, error)) return false;
		} else if (key == "movetimer") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.spawns = static_cast<int16_t>(v);
		} else if (key == "crouchtimer") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.crouch_timer = static_cast<uint8_t>(v & 0xFF);
			entity.unk15a = static_cast<uint8_t>((v >> 8) & 0xFF);
		} else if (key == "shoottimer") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.shoot_timer = static_cast<int16_t>(v);
		} else if (key == "attention") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.attention = static_cast<int16_t>(v);
		} else if (key == "advancetimer") {
			if (!token_i32(line, 1, entity.advancetimer, error)) return false;
		} else if (key == "max_attack_distance") {
			if (!token_i32(line, 1, entity.max_attack_distance, error)) return false;
		} else if (key == "edistances") {
			if (!token_i32(line, 1, entity.min_engagement_distance, error)) return false;
			if (!token_i32(line, 2, entity.max_engagement_distance, error)) return false;
		} else if (key == "next_ssn") {
			if (!token_i32(line, 1, entity.next_ssn, error)) return false;
		} else if (key == "color_override") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.color_override = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "grenades") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.grenades = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "mission_critical") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.mission_critical = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "lfp_group") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.lfp_group = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "extra_bheight") {
			// .mis-interchange transient fields (never serialized to .bms): height_lock declares
			// the position z ABSOLUTE and extra_bheight carries the baked base height under the
			// item [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292,
			// height_lock->rec+356), misldr.dll]. A .mis-parsed entity round-trips its own values;
			// an absent height_lock stays 0 (terrain-relative z in the original editor's frame).
			if (!token_i32(line, 1, entity.mis_extra_bheight, error)) return false;
		} else if (key == "height_lock") {
			if (!token_i32(line, 1, v, error)) return false;
			entity.mis_height_lock = static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255));
		} else if (key == "gen_string") {
			if (t.size() > 1 && t[1] != "null") {
				mis_copy_fixed(entity.gen_string, sizeof(entity.gen_string), t[1]);
			}
		}
	}
	error = "MIS item section is missing its end marker";
	return false;
}
} // namespace

bool parse_mis_text_to_bms(const std::string &text, bms::File &out, std::string &error) {
	initialize_mis_file(out);
	const std::vector<MisLine> lines = tokenize_mis_text(text);
	for (size_t pos = 0; pos < lines.size();) {
		const std::vector<std::string> &t = lines[pos].tokens;
		if (t.size() < 2 || t[0] != "begin") {
			++pos;
			continue;
		}
		const std::string &section = t[1];
		if (section == "general_information") {
			if (!parse_mis_general_information(lines, pos, out, error)) return false;
		} else if (section == "weapon_availability") {
			if (!skip_mis_section(lines, pos, "weapon_availability", error)) return false;
		} else if (section == "briefing") {
			if (!parse_mis_briefing(lines, pos, out, error)) return false;
		} else if (section == "areatrig") {
			if (!parse_mis_area_trigger(lines, pos, out, error)) return false;
		} else if (section == "waypoint") {
			if (!parse_mis_waypoint(lines, pos, out, error)) return false;
		} else if (section == "group") {
			if (!parse_mis_group(lines, pos, out, error)) return false;
		} else if (section == "layer") {
			if (!parse_mis_layer(lines, pos, out, error)) return false;
		} else if (section == "event") {
			if (!parse_mis_event(lines, pos, out, error)) return false;
		} else if (section == "item") {
			if (!parse_mis_item(lines, pos, out, error)) return false;
		} else {
			error = "Unsupported MIS section '" + section + "' in line " + std::to_string(lines[pos].line_no);
			return false;
		}
	}
	return true;
}

} // namespace opennova::mission::detail
