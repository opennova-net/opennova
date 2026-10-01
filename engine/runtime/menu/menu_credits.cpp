// A MARQUEE_WND's credits [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 over the
// ConfigFile text reader; CMarqueeWnd_RenderScrollingCredits @ 0x65ca00 for the node text].

#include <runtime/menu/menu_credits.h>

#include <runtime/menu/config_text.h>

#include <base/io/strutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::menu {

namespace {

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
