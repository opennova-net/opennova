// A ConfigFile's text form [orig: ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @
// 0x7608a0] (config_text.h).

#include <runtime/menu/config_text.h>

#include <base/io/strutil.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace opennova::menu {

namespace {

// sscanf "%[^<stop>]": one or more characters not in `stop`.
size_t scan_run(const std::string &line, size_t at, const char *stop) {
	size_t end = at;
	while (end < line.size() && std::strchr(stop, line[end]) == nullptr) ++end;
	return end;
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

} // namespace opennova::menu
