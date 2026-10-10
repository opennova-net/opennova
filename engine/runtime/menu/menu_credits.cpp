// A MARQUEE_WND's credits [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0 over the
// ConfigFile text reader; CMarqueeWnd_RenderScrollingCredits @ 0x65ca00 for the node text].

#include <runtime/menu/menu_credits.h>

#include <formats/configfile/config_file.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::menu {

namespace {

using configfile::ConfigSection;

// The ConfigFile's accessors (formats/configfile/config_file.h) [orig: ConfigFile_FindSection @ 0x75eeb0;
// effect_get_param_value_0 @ 0x75fa00; effect_get_param_value @ 0x75f580].
ConfigSection *find_section(std::vector<ConfigSection> &sections, const char *name) {
	return configfile::find_config_section(sections, name);
}

bool read_value(ConfigSection &s, const char *key, int index, std::string *text, float *real,
		int32_t *integer) {
	return configfile::read_config_value(s, key, index, text, real, integer);
}

bool read_current_value(ConfigSection &s, const char *key, int index, std::string *text) {
	return configfile::read_current_config_value(s, key, index, text, nullptr, nullptr);
}

// [orig: String_CopyN — at most size - 1 characters]
std::string copy_n(const std::string &s, size_t size) {
	return s.size() < size ? s : s.substr(0, size - 1);
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
	std::vector<ConfigSection> sections = configfile::parse_config_text(data, size);
	// The file loaded: the values and the running offset start over
	// [orig: @ 0x65c605..0x65c63d].
	io.scroll_rate = kMarqueeScrollRate;
	io.center_x = kMarqueeCenterX;
	io.space_mark = kMarqueeSpaceMark;
	io.comma_mark = kMarqueeCommaMark;
	io.vertical_space = kMarqueeVerticalSpace;
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
				// The CRT strtol at radix 16 (io::retail_strtol: the locale's white space,
				// 0xA0 included, saturating at 32 bits; D-NET-384) [orig: strtol @0x65c8a4].
				color = static_cast<uint32_t>(io::retail_strtol(line.c_str() + 2, 16));
			} else if (code == 'F' || code == 'f') {
				// x|y|texture: a fixed image that fades at the edges, no advance.
				const std::vector<std::string> parts = bar_tokens(line);
				if (parts.size() >= 3 && texture_loads && texture_loads(parts[2])) {
					MarqueeCreditNode &node = append(false);
					// [orig: _atol @0x65c7a3 / @0x65c7bc] (io::retail_atol; D-NET-384)
					node.fixed_x = io::retail_atol(parts[0].c_str());
					node.fixed_y = io::retail_atol(parts[1].c_str());
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

std::string marquee_marked_line(const std::string &shown) {
	std::string out;
	for (const char c : shown) {
		if (c == '%') out += "%%";
		else if (c == ' ') out += kMarqueeSpaceMark;
		else if (c == ',') out += kMarqueeCommaMark;
		else out += c;
	}
	return out;
}

} // namespace opennova::menu
