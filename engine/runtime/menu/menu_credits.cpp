// A MARQUEE_WND's credits [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 over the
// ConfigFile text reader; CMarqueeWnd_RenderScrollingCredits @ 0x65ca00 for the node text].

#include <runtime/menu/menu_credits.h>

#include <base/io/strutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::menu {

namespace {

// ---- the ConfigFile text reader ----------------------------------------------
// [orig: ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @ 0x7608a0]:
// CR LF ends a line (a lone CR or LF does not), a tab reads as a space, and the
// buffer gains a trailing LF. A line opening with '[' and one or more of
// A-Z, '_' and the digits is a section, its label lowercased
// [orig: ConfigFile_BuildSectionLabels @ 0x75df20]. A section's entries are its
// lines up to the next line opening with '[' that match
// "%[^;\n\r=]=%[^\n\r;]" with both parts: the key with its spaces trimmed, the value
// up to ';' or the line end [orig: ini_parse_section_entries @ 0x75db80]; the
// value's tokens split on ',' and ' ' [orig: ConfigFile_CountCommaSeparatedValues
// @ 0x75de30], each an integer, a float or a string by String_ClassifyNumeric
// @ 0x75d830 [orig: ConfigFile_ParseValues @ 0x7606f0]. (Retail reads each entry's
// values back through its line walker, whose key keeps leading spaces: a key
// written with leading spaces is not modeled.)

struct ConfigValue {
	int type = 4; // 1 integer, 2 float, 4 string
	int32_t integer = 0;
	float real = 0.0f;
	std::string text;
};

struct ConfigEntry {
	std::string key;
	std::vector<ConfigValue> values;
};

struct ConfigSection {
	std::string label; // lowercased
	std::vector<ConfigEntry> entries;
	// The read cursor [orig: section +16 (the current entry) / +20 (the next)].
	size_t current = 0;
	size_t next = SIZE_MAX;
};

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

// sscanf "%[^<stop>]": one or more characters not in `stop`.
size_t scan_run(const std::string &line, size_t at, const char *stop) {
	size_t end = at;
	while (end < line.size() && std::strchr(stop, line[end]) == nullptr) ++end;
	return end;
}

std::vector<ConfigSection> parse_config_text(const uint8_t *data, size_t size) {
	std::vector<std::string> lines(1);
	for (size_t i = 0; i < size; ++i) {
		const char c = static_cast<char>(data[i]);
		if (c == '\r' && i + 1 < size && data[i + 1] == '\n') {
			lines.back() += '\n';
			lines.emplace_back();
			++i;
			continue;
		}
		lines.back() += c == '\t' ? ' ' : c;
	}
	lines.back() += '\n';
	std::vector<ConfigSection> sections;
	ConfigSection *section = nullptr;
	for (const std::string &line : lines) {
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
		const std::string value = text.substr(key_end + 1, value_end - key_end - 1);
		size_t p = 0;
		while (p < value.size()) {
			while (p < value.size() && (value[p] == ',' || value[p] == ' ')) ++p;
			const size_t start = p;
			while (p < value.size() && value[p] != ',' && value[p] != ' ') ++p;
			if (p == start) break;
			ConfigValue v;
			v.text = value.substr(start, p - start);
			v.type = classify_numeric(v.text);
			if (v.type == 1) {
				v.integer = static_cast<int32_t>(std::strtol(v.text.c_str(), nullptr, 10));
			} else if (v.type == 2) {
				v.real = static_cast<float>(std::atof(v.text.c_str()));
			} else {
				v.type = 4;
			}
			entry.values.push_back(std::move(v));
		}
		section->entries.push_back(std::move(entry));
	}
	return sections;
}

// [orig: ConfigFile_FindSection @ 0x75eeb0 — the lowercased label; the structured
// cursor reset (+16 = the first entry, +20 = 0)]
ConfigSection *find_section(std::vector<ConfigSection> &sections, const char *name) {
	const std::string label = strutil::to_lower(name);
	for (ConfigSection &s : sections) {
		if (s.label != label) continue;
		s.current = 0;
		s.next = SIZE_MAX;
		return &s;
	}
	return nullptr;
}

// A read the way the structured accessor reads [orig: effect_get_param_value_0
// @ 0x75fa00]: from the next entry (else the current one) to the first whose key
// matches, stopping at an entry with no values; `index` is 1-based. A number read
// as text is printed "%d" or "%f".
bool read_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
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
	const ConfigValue &v = entry.values[static_cast<size_t>(index - 1)];
	if (v.type == 1) {
		if (real != nullptr) *real = static_cast<float>(v.integer);
		if (integer != nullptr) *integer = v.integer;
		if (text != nullptr) *text = std::to_string(v.integer);
	} else if (v.type == 2) {
		if (real != nullptr) *real = v.real;
		if (integer != nullptr) *integer = static_cast<int32_t>(v.real);
		if (text != nullptr) {
			char buffer[64];
			std::snprintf(buffer, sizeof buffer, "%f", static_cast<double>(v.real));
			*text = buffer;
		}
	} else if (text != nullptr) {
		*text = v.text;
	}
	return true;
}

// The current entry's value `index`, when its key still matches [orig:
// effect_get_param_value @ 0x75f580, reached through ini_read_value_with_context].
bool read_current_value(ConfigSection &s, const char *key, int index, std::string *text) {
	if (s.current >= s.entries.size() || !strutil::iequals(s.entries[s.current].key, key)) return false;
	const ConfigEntry &entry = s.entries[s.current];
	if (index < 1 || static_cast<size_t>(index) > entry.values.size()) {
		s.next = s.current + 1;
		return false;
	}
	const ConfigValue &v = entry.values[static_cast<size_t>(index - 1)];
	if (v.type == 1) {
		*text = std::to_string(v.integer);
	} else if (v.type == 2) {
		char buffer[64];
		std::snprintf(buffer, sizeof buffer, "%f", static_cast<double>(v.real));
		*text = buffer;
	} else {
		*text = v.text;
	}
	return true;
}

// [orig: String_CopyN — at most size - 1 characters]
std::string copy_n(const std::string &s, size_t size) {
	return s.size() < size ? s : s.substr(0, size - 1);
}

// strtol base 16 over a narrow string, saturating at retail's 32-bit long.
int32_t strtol16(const char *text) {
	const long long v = std::strtoll(text, nullptr, 16);
	if (v > INT32_MAX) return INT32_MAX;
	if (v < INT32_MIN) return INT32_MIN;
	return static_cast<int32_t>(v);
}

// The '|' tokens after the first two characters [orig: strtok with the "|" @ 0x7E1788].
std::vector<std::string> bar_tokens(const std::string &line) {
	std::vector<std::string> out;
	size_t p = 2;
	while (p < line.size()) {
		while (p < line.size() && line[p] == '|') ++p;
		const size_t start = p;
		while (p < line.size() && line[p] != '|') ++p;
		if (p > start) out.push_back(line.substr(start, p - start));
	}
	return out;
}

} // namespace

// [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0]
bool marquee_load_credits(const uint8_t *data, size_t size, MarqueeCredits &io,
		const std::function<bool(const std::string &)> &texture_loads) {
	// "CBIN" (0x4E494243) takes ConfigFile_ParseBinary [orig: ConfigFile_LoadFromFile
	// @ 0x760a10].
	if (size >= 4 && std::memcmp(data, "CBIN", 4) == 0) return false;
	std::vector<ConfigSection> sections = parse_config_text(data, size);
	// The file loaded: the values and the running offset start over
	// [orig: @ 0x65c605..0x65c63d].
	io.scroll_rate = 1.0f;
	io.center_x = 400;
	io.space_mark = '_';
	io.comma_mark = '@';
	io.vertical_space = 0;
	int offset = 0;
	if (ConfigSection *env = find_section(sections, "ENV")) {
		// A missing key reads 0 (the accessor writes 0 first).
		read_value(*env, "SCROLL_RATE", 1, nullptr, &io.scroll_rate, nullptr);
		find_section(sections, "ENV");
		int32_t value = 0;
		read_value(*env, "CENTER_X", 1, nullptr, nullptr, &value);
		io.center_x = value;
		find_section(sections, "ENV");
		read_value(*env, "VERTICAL_SPACE", 1, nullptr, nullptr, &value);
		io.vertical_space = value;
	}
	ConfigSection *text = find_section(sections, "TEXT");
	if (text == nullptr) return true;
	uint32_t color = 0xFFFFFFFFu; // the 8-dword block, 0xFF bytes [orig: memset(v18, 255)]
	int justify = 1;
	std::string font; // the buffer's +128 keeps the last font a line set
	const auto append = [&](bool advance) -> MarqueeCreditNode & {
		// [orig: CMarqueeWnd_AppendCreditNode @ 0x65c4d0 — y = bottom + offset; a
		// spacing node advances the offset by VERTICAL_SPACE]
		MarqueeCreditNode node;
		node.offset = offset;
		if (advance) offset += io.vertical_space;
		io.nodes.push_back(std::move(node));
		return io.nodes.back();
	};
	std::string line;
	bool more = read_value(*text, "TEXT", 1, &line, nullptr, nullptr);
	while (more) {
		line = copy_n(line, 0x80);
		if (strutil::iequals(line, "<CR>")) {
			offset += io.vertical_space;
		} else if (!line.empty() && line[0] == '~') {
			const char code = line.size() > 1 ? line[1] : '\0';
			if (code == 'C' || code == 'c') {
				color = static_cast<uint32_t>(strtol16(line.c_str() + 2));
			} else if (code == 'F' || code == 'f') {
				// x|y|texture: a fixed image that fades at the edges, no advance.
				const std::vector<std::string> parts = bar_tokens(line);
				if (parts.size() >= 3 && texture_loads && texture_loads(parts[2])) {
					MarqueeCreditNode &node = append(false);
					node.fixed_x = static_cast<int>(std::atol(parts[0].c_str()));
					node.fixed_y = static_cast<int>(std::atol(parts[1].c_str()));
					node.image = parts[2];
					node.fades = true;
					node.font = font;
					node.color = color;
					node.justify = justify;
				}
			} else if (code == 'I' || code == 'i') {
				const std::string image = line.substr(2);
				if (texture_loads && texture_loads(image)) {
					MarqueeCreditNode &node = append(true);
					node.image = image;
					node.font = font;
					node.color = color;
					node.justify = justify;
				}
			} else if (code == 'J' || code == 'j') {
				const std::vector<std::string> parts = bar_tokens(line);
				const char first = parts.empty() ? '\0' : parts[0][0];
				justify = first == 'L' || first == 'l' ? 0 : first == 'R' || first == 'r' ? 2 : 1;
			}
		} else {
			std::string second;
			if (read_current_value(*text, "TEXT", 2, &second)) font = copy_n(second, 0x20);
			MarqueeCreditNode &node = append(true);
			node.text = true;
			node.line = line;
			node.font = font;
			node.color = color;
			node.justify = justify;
		}
		more = read_value(*text, "TEXT", 1, &line, nullptr, nullptr);
	}
	return true;
}

// [orig: CMarqueeWnd_RenderScrollingCredits @ 0x65ca00 — sprintf(text_buffer, node), then the
// 128-byte remap loop]
std::string marquee_node_text(const MarqueeCredits &credits, const MarqueeCreditNode &node) {
	std::string out;
	for (size_t i = 0; i < node.line.size(); ++i) {
		if (node.line[i] == '%' && i + 1 < node.line.size() && node.line[i + 1] == '%') {
			out += '%';
			++i;
		} else {
			out += node.line[i];
		}
	}
	for (char &c : out) {
		if (c == credits.space_mark) c = ' ';
		else if (c == credits.comma_mark) c = ',';
	}
	return out;
}

} // namespace opennova::menu
