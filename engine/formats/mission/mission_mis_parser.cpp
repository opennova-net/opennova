#include "mission_mis.h"

// Reads the .mis text form back into a bms::File. Tokenizer and per-section parsers
// are private here; mission_mis_writer.cpp is the inverse.

#include "mission_detail.h"
#include "mission_records.h"
#include "bms_edit.h"

#include <formats/mission/mission_chains.h>

#include <formats/mission/authoring.h> // entity_kind_for_item_type: items.def TYPE -> BMS pool

#include <algorithm>
#include <cstring>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
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
		std::vector<std::string> tokens = tokenize_mis_line(trim_copy(raw));
		if (!tokens.empty()) {
			// Both stock readers match a key and a section's name without case (_stricmp) [orig: MIS_ParseToken
			// @0x450e50, JOTACmed.exe; MisLdr_ParseMisLine @0x100017b0, misldr.dll]: the first word is read
			// lowered, and the second after `begin` or `end`; a value keeps its case.
			tokens[0] = strutil::to_lower(tokens[0]);
			if (tokens.size() > 1 && (tokens[0] == "begin" || tokens[0] == "end")) tokens[1] = strutil::to_lower(tokens[1]);
			lines.push_back({line_no, std::move(tokens)});
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

// The keys the original editor's reader takes in a section; any other it refuses, failing the load [orig:
// JOTACmed.exe MIS_ParseToken @0x450e50: item @0x4536e7, event @0x453a7a, group @0x453de4, layer @0x453f37,
// areatrig @0x454144, waypoint @0x4538e9]. Ours reads past such a key and reports it.
bool key_in(const std::string &key, std::initializer_list<const char *> keys) {
	for (const char *known : keys)
		if (key == known) return true;
	return false;
}
bool original_item_key(const std::string &key) {
	return key_in(key, {"type_id", "name", "iai_name", "ai_textfile", "id", "position", "facing", "alignment", "team",
	                    "ai_name", "name_index", "team_budget", "map_symbol", "bmsi_attributes", "nolessthan",
	                    "nomorethan", "weapon_type", "sweapon_type", "grenades", "group_id", "group_rel", "waypoint_id",
	                    "wpnumber", "wpdistance", "wp_adv_trigger", "wpgoal0", "wpgoal1", "wpgoal2", "wpgoal3",
	                    "ttoolindex", "alert_state", "obliqueness", "waccuracy1", "waccuracy2", "perception",
	                    "perception2", "perfectionist", "perfectionist2", "movetimer", "crouchtimer", "shoottimer",
	                    "attention", "advancetimer", "edistances", "max_attack_distance", "attributes", "layer", "value",
	                    "loadoutfile", "bumper", "camo_type", "speed", "ai", "advance", "formation", "icon_value",
	                    "ammo_50_cal", "ammo_tow", "ammo_stinger", "ammo_grenade", "delay", "smoke_time",
	                    "name_override", "blink_group", "blink_group_a", "blink_group_b", "blink_parent",
	                    "blink_parent_a", "blink_parent_b", "extra_mlink", "extra_val1", "extra_val2", "extra_val3",
	                    "extra_val4", "extra_val5", "extra_valmode", "extra_bheight", "gen_string", "next_ssn",
	                    "lfp_group", "mission_critical", "height_lock", "color_override"});
}
void note(std::vector<MisIssue> *issues, size_t line, std::string message) {
	if (issues != nullptr) issues->push_back({line, std::move(message)});
}
void unknown_key(std::vector<MisIssue> *issues, const MisLine &line, const std::string &where) {
	note(issues, line.line_no,
	     where + "'s key '" + line.tokens[0] + "' is none the original editor reads there (its reader refuses the file); skipped");
}

void initialize_mis_file(bms::File &file) {
	file = {};
	file.header.magic[0] = 'B';
	file.header.magic[1] = 'M';
	file.header.magic[2] = 'S';
	file.header.magic[3] = static_cast<char>(bms::kMinVersion);
	file.waypoint_records.resize(bms::kWaypointRecordCount);
	file.group_records.resize(bms::kGroupRecordCount);
	file.wav_list_records.resize(bms::kWavListRecordCount);
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

bool parse_mis_general_information(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, MisDefaultKit &kit,
                                   std::string &error) {
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
			if (t.size() > 1) copy_fixed_field(header.mission_name, sizeof(header.mission_name), t[1]);
		} else if (key == "designer") {
			if (t.size() > 1) copy_fixed_field(header.designer, sizeof(header.designer), t[1]);
		} else if (key == "terrain") {
			if (t.size() > 1) copy_fixed_field(header.terrain, 16, t[1]);
		} else if (key == "cnv_file") {
			if (t.size() > 1) copy_fixed_field(header.terrain + 16, 16, t[1]);
		} else if (key == "tt_file") {
			if (t.size() > 1) copy_fixed_field(header.terrain + 32, 16, t[1]);
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
			if (t.size() > 1) copy_fixed_field(header.environment, sizeof(header.environment), t[1]);
		} else if (key == "start_time") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.start_time = hhmm_to_header_time(i32);
		} else if (key == "minutes_per_day") {
			if (!token_i32(line, 1, i32, error)) return false;
			header.minutes_per_day = static_cast<uint16_t>(i32);
		} else if (key == "terrain_tile_tga") {
			if (t.size() > 1) copy_fixed_field(header.terrain_tile, sizeof(header.terrain_tile), t[1]);
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
		} else if (key == "default_primary" || key == "default_secondary" || key == "default_accessory") {
			// The value as it stands, "" where the line gives none [orig: JOTACmed.exe MIS_ParseToken @ 0x451caa..0x451d4c].
			kit[key == "default_primary" ? 0 : key == "default_secondary" ? 1 : 2] = t.size() > 1 ? t[1] : std::string();
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
			copy_fixed_field(file.header.mission_briefing, sizeof(file.header.mission_briefing), briefing);
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

bool parse_mis_waypoint(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error,
                        std::vector<MisIssue> *issues) {
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
		// `noloop` and `attrib` set the one word [orig: JOTACmed.exe MIS_ParseToken @0x450e50, the test @0x4538a5]; the
		// description is the names' (parse_mis_names), the color and the speed the editor's alone.
		if (t.size() >= 2 && (t[0] == "attrib" || t[0] == "noloop")) {
			uint32_t flags = 0;
			if (!token_u32(line, 1, flags, error)) return false;
			record.flags = static_cast<bms::WaypointFlags>(flags);
		} else if (!key_in(t[0], {"description", "color", "speed"})) {
			unknown_key(issues, line, "waypoint " + std::to_string(index));
		}
	}
	error = "MIS waypoint section is missing its end marker";
	return false;
}

bool parse_mis_group(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error,
                     std::vector<MisIssue> *issues) {
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
		if (t.size() >= 2 && t[0] == "description") {
			// The group's name (bms.h GroupRecord), by set_group_name's rules; a description that is no name
			// is skipped, never stored as bytes.
			std::string why;
			if (!bms::set_group_name(record, t[1], why) && issues != nullptr)
				issues->push_back({line.line_no, "group " + std::to_string(index) + "'s description \"" + t[1] +
				                                         "\" is no group name: " + why});
		} else if (t.size() >= 2 && t[0] == "attrib") {
			// The record's flags @0 and its value @8, as the original editor's .bms writer stores a group's
			// attrib and commander [orig: JOTACmed.exe MIS_ExportBmsFile @0x44f920, the group @0x450940..0x4509b4].
			if (!token_i32(line, 1, value, error)) return false;
			record.flags = value;
		} else if (t.size() >= 2 && t[0] == "commander") {
			if (!token_i32(line, 1, value, error)) return false;
			record.value = value;
		} else if (t[0] != "color") {
			unknown_key(issues, line, "group " + std::to_string(index));
		}
	}
	error = "MIS group section is missing its end marker";
	return false;
}

// `begin wav_list N`: the original editor's wav list slot N, its `description` (no .bms byte holds
// it) and its `file_name`, which its .bms export copies into the table's record N at +4 [orig:
// JOTACmed.exe MIS_ParseToken
// @0x450e50, the section @ 0x450faa -> @ 0x4510b3 (N unchecked), the description @ 0x453e61, the
// file name
// @ 0x453eb0, any other key failing the load @ 0x453e88; MIS_ExportBmsFile @ 0x4509bc..0x450a14]. A slot
// outside 0..31 is none of the table's (the original's copy runs past its 32), and a file name past
// the record's 16 bytes (the original's export runs into its stack; 16 characters fill it, no NUL
// kept): each reported, the one skipped, the other cut.
bool parse_mis_wav_list(const std::vector<MisLine> &lines, size_t &pos, bms::File &file,
		std::string &error, std::vector<MisIssue> *issues) {
	int32_t index = 0;
	if (!token_i32(lines[pos], 2, index, error)) return false;
	const bool slotted = index >= 0 && index < bms::kWavListRecordCount;
	if (!slotted)
		note(issues, lines[pos].line_no,
				"wav_list " + std::to_string(index) + " is no slot of 0..31; skipped");
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "wav_list") {
			++pos;
			return true;
		}
		if (t.size() >= 2 && t[0] == "file_name") {
			if (!slotted)
				continue;
			if (t[1].size() > bms::kWavListNameSize)
				note(issues, line.line_no,
						"wav_list " + std::to_string(index) +
								"'s file name is past 16 characters; cut");
			copy_fixed_field(file.wav_list_records[static_cast<size_t>(index)].file_name,
					bms::kWavListNameSize, t[1]);
		} else if (t[0] != "description") {
			unknown_key(issues, line, "wav_list " + std::to_string(index));
		}
	}
	error = "MIS wav_list section is missing its end marker";
	return false;
}

// `begin layer N`: the original editor's own layer (its description and colour, an item's `layer`
// naming it), which no .bms byte holds [orig: JOTACmed.exe MIS_ParseToken @0x450e50, the keys @
// 0x453f12..0x453f37]: walked, its keys checked, nothing kept.
bool parse_mis_layer(const std::vector<MisLine> &lines, size_t &pos, std::string &error,
		std::vector<MisIssue> *issues) {
	int32_t index = 0;
	if (!token_i32(lines[pos], 2, index, error))
		return false;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "layer") {
			++pos;
			return true;
		}
		if (t[0] != "description" && t[0] != "color")
			unknown_key(issues, line, "layer " + std::to_string(index));
	}
	error = "MIS layer section is missing its end marker";
	return false;
}

// `begin areatrig N`: N is the area's zone id, the original editor's slot, and `attrib` its flags
// (MISSION_AREA 1, constrain-Z 2), which the original's .bms writer stores at @28; `color` is the
// editor's alone [orig: JOTACmed.exe MIS_ParseToken @0x450e50, the section @0x4510a9, its keys
// @0x4540ac..0x454144; MIS_ExportBmsFile
// @0x44f920, the area @0x450a16..0x450ab3]. The original's .mis has a slot per zone id 1..99 (its
// writer's loop @0x454d11..0x454daf, its rebuild's range MissionDocument_RebuildAreasFromMarkers @0x44d2a9): a number outside
// them makes no area, and a second section of a number goes to the first's area, as the original's
// slot, each reported. The area has its bounds from the 6013 items of its id (rebuild_mis_areas,
// D-MIS-14); one of none is made none.
bool parse_mis_area_trigger(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error,
                            std::vector<MisIssue> *issues) {
	const size_t begun = lines[pos].line_no;
	int32_t id = 0;
	if (!token_i32(lines[pos], 2, id, error)) return false;
	const bool slotted = id >= 1 && id < static_cast<int32_t>(kMisAreaSlots);
	bms::AreaTrigger made = {};
	made.id = id;
	bms::AreaTrigger *trigger = &made;
	if (!slotted) {
		note(issues, begun, "areatrig " + std::to_string(id) + " is no zone id of 1..99, the original editor's slots; no area made");
	} else {
		for (bms::AreaTrigger &area : file.area_triggers)
			if (area.id == id) {
				trigger = &area;
				note(issues, begun, "areatrig " + std::to_string(id) + " is a second section of its zone id; its lines go to the first's area, as the original editor's slot");
				break;
			}
	}
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "areatrig") {
			if (slotted && trigger == &made) file.area_triggers.push_back(made);
			++pos;
			return true;
		}
		if (t.size() >= 2 && t[0] == "attrib") {
			if (!token_u32(line, 1, trigger->flags, error)) return false;
		} else if (!key_in(t[0], {"description", "color"})) {
			unknown_key(issues, line, "areatrig " + std::to_string(id));
		}
	}
	error = "MIS areatrig section is missing its end marker";
	return false;
}

// A triggers or actions section of an event: its rows up to its `end`, whatever its declared count. The original
// editor's reader stores the declared count and fills a slot per row until the `end`, never comparing the two
// [orig: JOTACmed.exe MIS_ParseToken, the count @ 0x453ac5, the slot index reset @ 0x453acd, a row's slot
// @ 0x453b87, the `end` @ 0x453b59], and its .bms export writes the count's low byte and the full count's records
// from the event's slots [orig: MIS_ExportBmsFile, the byte @ 0x450b78, the records @ 0x450c08]. So the run the event
// keeps is the one the game reads of that byte (triggers_read, actions_read): the rows past it left out, the slots
// past the rows as a fresh document's event holds them, zero. Its event holds 20 slots of each, 32 bytes apiece
// [orig: MIS_ParseToken, the slot @ 0x453b84, the triggers' @ 0x453b99, the actions' 640 bytes on @ 0x453c6f, an
// event's 1372 bytes @ 0x453b6a..0x453b8d], and a row's slot is never bounded (@ 0x453b81): a section past 20 runs
// over the slots after it (a trigger's over the event's actions, an action's past the event), which the editor's
// edits never make (kMaxEventChainEntries). `issues` (optional) names a section whose rows are not that run, and
// one of more rows than the 20 slots.
bool parse_mis_nested_records(const std::vector<MisLine> &lines,
                              size_t &pos,
                              const std::string &section,
                              int32_t declared,
                              bms::File &file,
                              std::string &error,
                              std::vector<MisIssue> *issues) {
	const bool triggers = section == "triggers";
	std::vector<bms::Trigger> trigger_rows;
	std::vector<bms::Action> action_rows;
	const size_t begun = pos < lines.size() ? lines[pos].line_no : 0;
	for (++pos; pos < lines.size(); ++pos) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == section) {
			const uint8_t byte = static_cast<uint8_t>(declared);
			const size_t keep = triggers ? triggers_read(byte) : actions_read(byte);
			const size_t rows = triggers ? trigger_rows.size() : action_rows.size();
			if (rows != keep)
				note(issues, begun,
				     "the " + section + " section declares " + std::to_string(declared) + " and holds " +
				             std::to_string(rows) + (rows == 1 ? " row" : " rows") + ": the original editor keeps the count, the "
				             "game reading " + std::to_string(keep) + (keep == 1 ? " record" : " records") +
				             (rows > keep ? " (the rows past them left out)" : " (the records past the rows zero)"));
			if (rows > kMaxEventChainEntries)
				note(issues, begun,
				     "the " + section + " section runs past the original editor's 20 " +
				             std::string(triggers ? "trigger" : "action") + " slots: it writes "
				             "a row past them over " + (triggers ? "the event's actions" : "the event after it") +
				             ", and its export the records from there; read here as the rows are written");
			if (triggers) {
				trigger_rows.resize(keep, bms::Trigger{});
				file.triggers.insert(file.triggers.end(), trigger_rows.begin(), trigger_rows.end());
			} else {
				action_rows.resize(keep, bms::Action{});
				file.actions.insert(file.actions.end(), action_rows.begin(), action_rows.end());
			}
			++pos;
			return true;
		}
		if (triggers) {
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
			trigger_rows.push_back(trigger);
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
			action_rows.push_back(action);
		}
	}
	error = "MIS " + section + " block is missing its end marker";
	return false;
}

bool parse_mis_event(const std::vector<MisLine> &lines, size_t &pos, bms::File &file, std::string &error,
                     std::vector<MisIssue> *issues) {
	const size_t number = file.events.size();
	bms::Event event = {};
	event.trigger_index = static_cast<int32_t>(file.triggers.size());
	event.action_index = static_cast<int32_t>(file.actions.size());
	int32_t declared_triggers = 0, declared_actions = 0; // a section's count as written
	bool triggers_declared = false, actions_declared = false;
	for (++pos; pos < lines.size();) {
		const MisLine &line = lines[pos];
		const std::vector<std::string> &t = line.tokens;
		if (t.size() >= 2 && t[0] == "end" && t[1] == "event") {
			event.trigger_count = static_cast<uint8_t>(file.triggers.size() - static_cast<size_t>(event.trigger_index));
			event.action_count = static_cast<uint8_t>(file.actions.size() - static_cast<size_t>(event.action_index));
			// A section's declared count is the event's byte, as the original editor stores it and its export writes
			// its low byte (parse_mis_nested_records): a byte of 128..255 over the run the game reads of it too (D-10:
			// the zone resolvers read a trigger byte of 128..255 as no run, where 1 resolves the first trigger's zone
			// [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000, @0x453029]).
			if (triggers_declared) event.trigger_count = static_cast<uint8_t>(declared_triggers);
			if (actions_declared) event.action_count = static_cast<uint8_t>(declared_actions);
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
			if (!parse_mis_nested_records(lines, pos, "triggers", value, file, error, issues)) return false;
			declared_triggers = value;
			triggers_declared = true;
		} else if (t.size() >= 3 && t[0] == "begin" && t[1] == "actions") {
			if (!token_i32(line, 2, value, error)) return false;
			if (!parse_mis_nested_records(lines, pos, "actions", value, file, error, issues)) return false;
			declared_actions = value;
			actions_declared = true;
		} else {
			if (t[0] != "name") unknown_key(issues, line, "event " + std::to_string(number));
			++pos;
		}
	}
	error = "MIS event section is missing its end marker";
	return false;
}

// (next_entity_id used to be forward-declared here; it now comes from mission_records.h.)

bool parse_mis_item(const std::vector<MisLine> &lines, size_t &pos,
                    const MisItemTypeResolver &resolve_item_type, bms::File &file,
                    std::string &error, std::vector<MisIssue> *issues) {
	const size_t begun = lines[pos].line_no;
	bms::Entity entity = {};
	int32_t extra_valmode = 0;
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
			// An area marker is no record of the pools: its box is its zone's area
			// (rebuild_mis_areas).
			if (entity.type_id == kMisAreaMarkerType) {
				file.mis_area_markers.push_back({ entity.wp_number, entity.x, entity.y, entity.z,
						entity.mis_height_lock, extra_valmode, entity.mis_extra_bheight });
				++pos;
				return true;
			}
			if (entity.id == 0) {
				entity.id = next_entity_id(file);
			}
			// Pool-kind classification: the .mis text does not carry the BMS pool, so
			// the record's kind is derived from the items.def TYPE of its item id,
			// exactly like the Nile importer classifies each record into its scene
			// branch [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. The
			// TYPE -> pool mapping is authoring::entity_kind_for_item_type (1:1 across
			// 185,325 entities in 114 shipping JO missions). With no resolver — no
			// items.def loaded — or an id the table does not carry, the record stays in
			// the generic item pool (the kind is unknowable without the item table).
			EntityKind kind = EntityKind::Item;
			if (resolve_item_type) {
				kind = authoring::entity_kind_for_item_type(
						resolve_item_type(bms_type_id_to_item_id(entity.type_id)));
			}
			entity.type = to_bms_type(kind);
			entities(file, kind)->push_back(entity);
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
			if (t.size() > 1) copy_fixed_field(entity.name1, sizeof(entity.name1), t[1]);
		} else if (key == "ai_textfile") {
			if (t.size() > 1) copy_fixed_field(entity.name2, sizeof(entity.name2), t[1]);
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
		} else if (key == "team" || key == "alignment") { // one word to the original [orig: JOTACmed.exe MIS_ParseToken @0x4523bb]
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
		} else if (key == "perception2" || key == "perception") { // one word each [orig: JOTACmed.exe MIS_ParseToken @0x452aad]
			if (!token_i32(line, 1, entity.perception2, error)) return false;
		} else if (key == "perfectionist2" || key == "perfectionist") { // [orig: JOTACmed.exe MIS_ParseToken @0x452b08]
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
			// .mis-interchange transient fields (never serialized to .bms): the item's base and its
			// lock, read as written [orig: MisLdr_ParseMisLine @ 0x100017b0 (extra_bheight->rec+292,
			// height_lock->rec+356), misldr.dll]: an unlocked item's z and base relative to the
			// terrain, a locked one's absolute (the file's frame, bms::MisFrame::Mis); an absent
			// height_lock stays 0, as the original editor writes none for an unlocked item.
			// mis_items_to_bms_frame takes the file to the .bms's frame (mission_mis.h).
			if (!token_i32(line, 1, entity.mis_extra_bheight, error)) return false;
		} else if (key == "height_lock") {
			// Raw, as the original editor reads it into an int and tests it nonzero (-1 is a lock)
			// [orig: JOTACmed.exe MIS_ParseToken @ 0x4536c9 (atol); sub_44E640 @ 0x44e657].
			if (!token_i32(line, 1, entity.mis_height_lock, error)) return false;
		} else if (key == "extra_valmode") {
			// An area marker's (the export's test of the least-x one); no .bms field of another
			// item's.
			if (!token_i32(line, 1, extra_valmode, error))
				return false;
		} else if (key == "gen_string") {
			if (t.size() > 1 && t[1] != "null") {
				copy_fixed_field(entity.gen_string, sizeof(entity.gen_string), t[1]);
			}
		} else if (!original_item_key(key)) {
			unknown_key(issues, line, "the item begun in line " + std::to_string(begun));
		}
	}
	error = "MIS item section is missing its end marker";
	return false;
}
} // namespace

bool parse_mis_text_to_bms(const std::string &text, const MisItemTypeResolver &resolve_item_type,
                           bms::File &out, std::string &error, std::vector<MisIssue> *issues,
                           const MisWeaponTable *weapons) {
	initialize_mis_file(out);
	MisDefaultKit kit = kMisResetKit; // a slot no line names keeps the original's reset kit
	// The heights as the file holds them (bms::MisFrame::Mis): mis_items_to_bms_frame takes them
	// to the .bms's over the terrain.
	out.mis_frame = bms::MisFrame::Mis;
	const std::vector<MisLine> lines = tokenize_mis_text(text);
	for (size_t pos = 0; pos < lines.size();) {
		const std::vector<std::string> &t = lines[pos].tokens;
		if (t.size() < 2 || t[0] != "begin") {
			++pos;
			continue;
		}
		const std::string &section = t[1];
		if (section == "general_information") {
			if (!parse_mis_general_information(lines, pos, out, kit, error)) return false;
		} else if (section == "weapon_availability") {
			if (!skip_mis_section(lines, pos, "weapon_availability", error)) return false;
		} else if (section == "briefing") {
			if (!parse_mis_briefing(lines, pos, out, error)) return false;
		} else if (section == "areatrig") {
			if (!parse_mis_area_trigger(lines, pos, out, error, issues)) return false;
		} else if (section == "waypoint") {
			if (!parse_mis_waypoint(lines, pos, out, error, issues)) return false;
		} else if (section == "group") {
			if (!parse_mis_group(lines, pos, out, error, issues)) return false;
		} else if (section == "layer") {
			if (!parse_mis_layer(lines, pos, error, issues))
				return false;
		} else if (section == "wav_list") {
			if (!parse_mis_wav_list(lines, pos, out, error, issues))
				return false;
		} else if (section == "event") {
			if (!parse_mis_event(lines, pos, out, error, issues)) return false;
		} else if (section == "item") {
			if (!parse_mis_item(lines, pos, resolve_item_type, out, error, issues)) return false;
		} else if (section == "teleport_target" || section == "wind") {
			// Sections the original editor writes and reads that the .bms has no room for [orig: JOTACmed.exe MIS_ParseToken
			// @0x450e50, the sections @0x450faa, @0x451004, @0x451119; MIS_SaveDocument @0x454f28..0x455015].
			note(issues, lines[pos].line_no, "the " + section + " section is the original editor's; not kept here");
			if (!skip_mis_section(lines, pos, section, error)) return false;
		} else {
			error = "Unsupported MIS section '" + section + "' in line " + std::to_string(lines[pos].line_no);
			return false;
		}
	}
	rebuild_mis_areas(out);
	// The loadout chunk the original's .bms export builds from the kit (D-MIS-19).
	out.loadout = mis_default_loadout(kit, weapons);
	return true;
}

bool parse_mis_names(const std::string &text, MisNames &out, std::string &error, std::vector<MisIssue> *issues) {
	out = MisNames{};
	// The variables' names: lines both original readers skip (mission_mis.h kMisVariableLine), read
	// from the text before the tokenizer, which skips them too: `//var <index> "<name>"`, an index
	// of 0..255, a second line of an index replacing the first's; any other such line reported.
	{
		size_t line_no = 0;
		for (size_t at = 0; at <= text.size();) {
			const size_t end = std::min(text.find('\n', at), text.size());
			std::string line = text.substr(at, end - at);
			at = end + 1;
			++line_no;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t lead = line.find_first_not_of(" \t");
			if (lead == std::string::npos ||
					line.compare(lead, std::strlen(kMisVariableLine), kMisVariableLine) != 0)
				continue;
			const size_t open = line.find('"'), close = line.rfind('"');
			char *digits_end = nullptr;
			const char *digits = line.c_str() + lead + std::strlen(kMisVariableLine);
			const long index = std::strtol(digits, &digits_end, 10);
			if (open == std::string::npos || close <= open || digits_end == digits || index < 0 ||
					index > kVariableNameIndexMax) {
				note(issues, line_no,
						"a variable's name line is none of `//var <0..255> \"<name>\"`; skipped");
				continue;
			}
			out.variables[static_cast<size_t>(index)] = line.substr(open + 1, close - open - 1);
		}
	}
	const std::vector<MisLine> lines = tokenize_mis_text(text);
	// The section a line is in: the three that carry a name kept, any other walked to its end (an item's
	// `name` and general_information's are not these). An event by its place in the file, an area by its
	// zone id (`begin areatrig N`, 1..99 as parse_mis_text reads it: another number's name skipped, a second
	// section's replacing the first's), a path by its record index (`begin waypoint N`).
	enum class Kind { None, Event, Area, Path, Other };
	Kind kind = Kind::None;
	std::string section;
	size_t index = 0;
	size_t begun_at = 0;
	size_t events = 0;
	std::array<bool, kMisAreaSlots> area_seen{};
	for (const MisLine &line : lines) {
		const std::vector<std::string> &t = line.tokens;
		if (kind == Kind::None) {
			if (t.size() < 2 || t[0] != "begin") continue;
			section = t[1];
			begun_at = line.line_no;
			if (section == "event") {
				kind = Kind::Event;
				index = events++;
			} else if (section == "areatrig") {
				int32_t id = 0;
				if (!token_i32(line, 2, id, error)) return false;
				if (id < 1 || id >= static_cast<int32_t>(kMisAreaSlots)) {
					note(issues, line.line_no, "areatrig " + std::to_string(id) + " is no zone id of 1..99, the original editor's slots; its name skipped");
					kind = Kind::Other;
					continue;
				}
				if (area_seen[static_cast<size_t>(id)])
					note(issues, line.line_no, "areatrig " + std::to_string(id) + " is a second section of its zone id; its description replaces the first's, as the original editor's slot");
				area_seen[static_cast<size_t>(id)] = true;
				kind = Kind::Area;
				index = static_cast<size_t>(id);
			} else if (section == "waypoint") {
				int32_t number = 0;
				if (!token_i32(line, 2, number, error)) return false;
				if (number < 0 || number >= bms::kWaypointRecordCount) {
					error = "MIS waypoint index out of range in line " + std::to_string(line.line_no);
					return false;
				}
				kind = Kind::Path;
				index = static_cast<size_t>(number);
			} else {
				kind = Kind::Other;
			}
			continue;
		}
		const bool ends = (t.size() >= 2 && t[0] == "end" && t[1] == section) ||
		                  (section == "briefing" && !t.empty() && t[0] == "endbriefing");
		if (ends) {
			kind = Kind::None;
			continue;
		}
		if (t.size() < 2) continue;
		if (kind == Kind::Event && t[0] == "name") {
			if (out.events.size() <= index) out.events.resize(index + 1);
			out.events[index] = t[1];
		} else if (kind == Kind::Area && t[0] == "description") {
			out.areas[index] = t[1];
		} else if (kind == Kind::Path && t[0] == "description") {
			out.paths[index] = t[1];
		}
	}
	if (kind != Kind::None) {
		error = "MIS " + section + " section begun in line " + std::to_string(begun_at) + " is missing its end marker";
		return false;
	}
	out.events.resize(events);
	return true;
}

} // namespace opennova::mission::detail
