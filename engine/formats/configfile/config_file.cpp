// The ConfigFile's text form and its accessors (config_file.h) [orig: ConfigFile_LoadFromFile @ 0x760a10 ->
// ConfigFile_ParseText @ 0x7608a0; ConfigFile_FindSection @ 0x75eeb0; effect_get_param_value_0 @ 0x75fa00;
// effect_get_param_value @ 0x75f580].

#include <formats/configfile/config_file.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::configfile {

namespace {

// strtol over retail's 32-bit long, saturating at its range whatever this platform's long is (a
// 64-bit long would let a value past it wrap in the cast).
int32_t strtol32(const char *text) {
	const long long v = std::strtoll(text, nullptr, 10);
	if (v > INT32_MAX) return INT32_MAX;
	if (v < INT32_MIN) return INT32_MIN;
	return static_cast<int32_t>(v);
}

// sscanf "%[^<stop>]": one or more characters not in `stop`.
size_t scan_run(const std::string &line, size_t at, const char *stop) {
	size_t end = at;
	while (end < line.size() && std::strchr(stop, line[end]) == nullptr) ++end;
	return end;
}

// A value as the accessors print it [orig: sprintf "%d" @ 0x75fc11 / "%f" @ 0x75fbae].
std::string printed(const ConfigValue &v) {
	if (v.type == 1) return std::to_string(v.integer);
	if (v.type == 2) {
		char buffer[512];
		std::snprintf(buffer, sizeof buffer, "%f", static_cast<double>(v.real));
		return buffer;
	}
	return v.text;
}

// One value into the accessor's outputs [orig: @ 0x75fb2e..0x75fc36]: an integer read as a float
// converts (fild), a float read as an integer truncates (_ftol2_sse), a string reads as 0.
void store(const ConfigValue &v, std::string *text, float *real, int32_t *integer) {
	if (v.type == 1) {
		if (real != nullptr) *real = static_cast<float>(v.integer);
		if (integer != nullptr) *integer = v.integer;
	} else if (v.type == 2) {
		if (real != nullptr) *real = v.real;
		if (integer != nullptr) *integer = io::retail_ftol_sse2(static_cast<double>(v.real));
	}
	if (text != nullptr) *text = printed(v);
}

} // namespace

// [orig: String_ClassifyNumeric @ 0x75d830]: 0 string, 1 integer, 2 float.
int classify_numeric(const std::string &s) {
	bool has_dot = false;
	bool has_sign = false;
	size_t p = 0;
	if (s.empty()) return 0;
	while (!std::isdigit(static_cast<unsigned char>(s[p]))) {
		switch (s[p]) {
			case ' ':
				if (has_dot) return 0;
				break;
			case '+':
			case '-':
				if (has_sign) return 0;
				has_sign = true;
				break;
			case '.':
				if (has_dot) return 0;
				has_dot = true;
				break;
			default:
				return 0;
		}
		if (++p >= s.size()) return 0;
	}
	while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
		++p;
		if (p < s.size() && s[p] == '.') {
			if (has_dot) return 0;
			has_dot = true;
			++p;
		} else if (p < s.size() && (s[p] == '-' || s[p] == '+')) {
			return 0;
		}
	}
	return p >= s.size() ? (has_dot ? 2 : 1) : 0;
}

std::vector<ConfigSection> parse_config_text(const uint8_t *data, size_t size) {
	// The lines, each with its first byte's offset into the text.
	std::vector<std::pair<std::string, size_t>> lines(1, {std::string(), 0});
	for (size_t i = 0; i < size; ++i) {
		const char c = static_cast<char>(data[i]);
		if (c == '\r' && i + 1 < size && data[i + 1] == '\n') {
			lines.back().first += '\n';
			lines.emplace_back(std::string(), i + 2);
			++i;
			continue;
		}
		lines.back().first += c == '\t' ? ' ' : c;
	}
	lines.back().first += '\n';
	std::vector<ConfigSection> sections;
	ConfigSection *section = nullptr;
	for (const auto &[line, line_offset] : lines) {
		const std::string text = line.substr(0, line.find('\0'));
		if (!text.empty() && text[0] == '[') {
			size_t end = 1;
			while (end < text.size() && (std::isupper(static_cast<unsigned char>(text[end])) ||
												std::isdigit(static_cast<unsigned char>(text[end])) ||
												text[end] == '_'))
				++end;
			if (end > 1) {
				sections.emplace_back();
				sections.back().label = strutil::to_lower(text.substr(1, end - 1));
				sections.back().offset = line_offset;
				section = &sections.back();
			} else {
				section = nullptr; // a '[' line ends the entries without opening a section
			}
			continue;
		}
		if (section == nullptr) continue;
		const size_t key_end = scan_run(text, 0, ";\n\r=");
		if (key_end == 0 || key_end >= text.size() || text[key_end] != '=') continue;
		const size_t value_end = scan_run(text, key_end + 1, "\n\r;");
		if (value_end == key_end + 1) continue;
		ConfigEntry entry;
		entry.key = std::string(strutil::trim_view(text.substr(0, key_end)));
		entry.offset = line_offset;
		const size_t value_start = key_end + 1;
		const std::string value = text.substr(value_start, value_end - value_start);
		size_t p = 0;
		while (p < value.size()) {
			while (p < value.size() && (value[p] == ',' || value[p] == ' ')) ++p;
			const size_t start = p;
			while (p < value.size() && value[p] != ',' && value[p] != ' ') ++p;
			if (p == start) break;
			ConfigValue v;
			v.text = value.substr(start, p - start);
			v.offset = line_offset + value_start + start;
			v.type = classify_numeric(v.text);
			if (v.type == 1) {
				v.integer = strtol32(v.text.c_str());
			} else if (v.type == 2) {
				v.real = static_cast<float>(io::retail_atof(v.text.c_str()));
			} else {
				v.type = 4;
			}
			entry.values.push_back(std::move(v));
		}
		section->entries.push_back(std::move(entry));
	}
	return sections;
}

ConfigSection *find_config_section(std::vector<ConfigSection> &sections, const char *name) {
	const std::string label = strutil::to_lower(name);
	for (ConfigSection &s : sections) {
		if (!strutil::iequals(s.label, label)) continue;
		s.current = 0;
		s.next = SIZE_MAX;
		return &s;
	}
	return nullptr;
}

bool read_config_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
		int32_t *integer) {
	if (real != nullptr) *real = 0.0f;
	if (integer != nullptr) *integer = 0;
	size_t at = s.next != SIZE_MAX ? s.next : s.current;
	if (index < 1 || at >= s.entries.size()) return false;
	while (!strutil::iequals(s.entries[at].key, key)) {
		s.current = at;
		if (s.entries[at].values.empty()) return false;
		s.next = ++at;
		if (at >= s.entries.size()) return false;
	}
	s.current = at;
	s.next = at + 1;
	const ConfigEntry &entry = s.entries[at];
	if (static_cast<size_t>(index) > entry.values.size()) return false;
	store(entry.values[static_cast<size_t>(index - 1)], text, real, integer);
	return true;
}

bool read_current_config_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
		int32_t *integer) {
	if (real != nullptr) *real = 0.0f;
	if (integer != nullptr) *integer = 0;
	if (index < 1 || s.current >= s.entries.size() || !strutil::iequals(s.entries[s.current].key, key))
		return false;
	const ConfigEntry &entry = s.entries[s.current];
	s.next = s.current + 1;
	if (static_cast<size_t>(index) > entry.values.size()) return false;
	store(entry.values[static_cast<size_t>(index - 1)], text, real, integer);
	return true;
}

} // namespace opennova::configfile
