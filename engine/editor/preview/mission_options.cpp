#include <editor/preview/mission_options.h>

#include <cmath>
#include <iterator>

namespace opennova::editor {

namespace {

using io::JsonValue;

struct Flag {
	const char *key;
	bool MissionViewportOptions::*member;
};
constexpr Flag kShow[] = {
	{ "terrain", &MissionViewportOptions::terrain }, { "sky", &MissionViewportOptions::sky },
	{ "water", &MissionViewportOptions::water }, { "models", &MissionViewportOptions::models },
	{ "shadows", &MissionViewportOptions::shadows },
};
constexpr Flag kMarks[] = {
	{ "items", &MissionViewportOptions::items }, { "buildings", &MissionViewportOptions::buildings },
	{ "markers", &MissionViewportOptions::markers }, { "organics", &MissionViewportOptions::organics },
	{ "areas", &MissionViewportOptions::areas }, { "paths", &MissionViewportOptions::paths },
	{ "labels", &MissionViewportOptions::labels },
};

template <size_t N>
JsonValue flags_json(const MissionViewportOptions &options, const Flag (&flags)[N]) {
	JsonValue out = JsonValue::make_object();
	for (const Flag &flag : flags) out.set(flag.key, JsonValue::make_bool(options.*flag.member));
	return out;
}

// An object of true-or-false members among `flags`, each set on `held`.
template <size_t N>
bool read_flags(const JsonValue &json, const char *group, const Flag (&flags)[N], MissionViewportOptions &held,
		std::string &error) {
	if (!json.is_object()) {
		error = std::string("options.") + group + " is an object of true or false members.";
		return false;
	}
	for (const io::JsonMember &member : json.object) {
		const Flag *found = nullptr;
		for (const Flag &flag : flags)
			if (member.key == flag.key) found = &flag;
		if (!found) {
			std::string takes;
			for (const Flag &flag : flags) takes += std::string(takes.empty() ? "" : ", ") + flag.key;
			error = std::string("Unknown options.") + group + " member \"" + member.key + "\" (it takes " + takes + ").";
			return false;
		}
		if (!member.value.is_bool()) {
			error = std::string("options.") + group + "." + member.key + " is true or false.";
			return false;
		}
		held.*found->member = member.value.boolean;
	}
	return true;
}

constexpr const char *kTools[] = { "select", "place", "path", "area" };

// The path numbers a stop is added to: 1 to 122 (0 and 123 to 127 name no route).
constexpr int64_t kLastRoutePath = 122;

} // namespace

const char *mission_tool_token(MissionTool tool) {
	return size_t(tool) < std::size(kTools) ? kTools[size_t(tool)] : "select";
}

bool mission_tool_from_token(const std::string &token, MissionTool &out) {
	for (size_t i = 0; i < std::size(kTools); ++i)
		if (token == kTools[i]) {
			out = MissionTool(i);
			return true;
		}
	return false;
}

bool operator==(const MissionViewportOptions &a, const MissionViewportOptions &b) {
	for (const Flag &flag : kShow)
		if (a.*flag.member != b.*flag.member) return false;
	for (const Flag &flag : kMarks)
		if (a.*flag.member != b.*flag.member) return false;
	return a.mark_range == b.mark_range && a.stick == b.stick && a.time == b.time && a.tool == b.tool &&
			a.item == b.item && a.path == b.path;
}

io::JsonValue mission_options_to_json(const MissionViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("show", flags_json(options, kShow));
	out.set("marks", flags_json(options, kMarks));
	out.set("mark_range", io::json_number(double(options.mark_range)));
	out.set("stick", JsonValue::make_bool(options.stick));
	out.set("time", options.time < 0.0 ? JsonValue::make_null() : io::json_number(options.time));
	out.set("tool", io::json_string(mission_tool_token(options.tool)));
	out.set("item", io::json_number(double(options.item)));
	out.set("path", io::json_number(double(options.path)));
	return out;
}

bool mission_options_from_json(const JsonValue &json, MissionViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "\"options\" is an object, {show, marks, mark_range, stick, time}.";
		return false;
	}
	MissionViewportOptions read = held;
	for (const io::JsonMember &member : json.object) {
		const JsonValue &value = member.value;
		if (member.key == "show") {
			if (!read_flags(value, "show", kShow, read, error)) return false;
		} else if (member.key == "marks") {
			if (!read_flags(value, "marks", kMarks, read, error)) return false;
		} else if (member.key == "mark_range") {
			float range = 0.0f;
			if (!io::json_float(value, range) || range < 0.0f) {
				error = "options.mark_range is a number of metres, 0 or more (0: no limit).";
				return false;
			}
			read.mark_range = range;
		} else if (member.key == "stick") {
			if (!value.is_bool()) {
				error = "options.stick is true or false.";
				return false;
			}
			read.stick = value.boolean;
		} else if (member.key == "time") {
			if (value.is_null()) {
				read.time = -1.0;
			} else if (!value.is_number() || !std::isfinite(value.number) || value.number < 0.0 || value.number > 24.0) {
				error = "options.time is an hour of the day, 0 to 24, or null for the mission's start time.";
				return false;
			} else {
				read.time = value.number;
			}
		} else if (member.key == "tool") {
			if (!value.is_string() || !mission_tool_from_token(value.string, read.tool)) {
				error = "options.tool is select, place, path or area.";
				return false;
			}
		} else if (member.key == "item") {
			if (!value.is_number() || value.number < 0.0 || value.number != std::floor(value.number) || value.number > 2147483647.0) {
				error = "options.item is an item's id, a whole number (0: none picked).";
				return false;
			}
			read.item = int64_t(value.number);
		} else if (member.key == "path") {
			if (!value.is_number() || value.number < 0.0 || value.number != std::floor(value.number) ||
					value.number > double(kLastRoutePath)) {
				error = "options.path is a path's number, 1 to 122 (0: none picked; 123 to 127 are commands).";
				return false;
			}
			read.path = int(value.number);
		} else {
			error = "Unknown options member \"" + member.key +
					"\" (it takes show, marks, mark_range, stick, time, tool, item, path).";
			return false;
		}
	}
	held = read;
	return true;
}

} // namespace opennova::editor
