#include <formats/def/def_hudpos_text.h>

#include <cstdlib>

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>

namespace opennova::def {

namespace {

// A first value compared as the parser reads it: a number by its value (a HUDSTANCE's id through
// atof), anything else without case [orig: HUD_ParseHudposToken @ 0x59F370].
bool same_value(const std::string &written, const char *wanted) {
	char *end = nullptr;
	const double a = std::strtod(written.c_str(), &end);
	const bool number = !written.empty() && end && *end == '\0';
	char *wanted_end = nullptr;
	const double b = std::strtod(wanted, &wanted_end);
	if (number && wanted[0] && wanted_end && *wanted_end == '\0') return a == b;
	return strutil::iequals(written, wanted);
}

} // namespace

std::vector<HudLayoutLine> hud_layout_lines(const std::string &text) {
	std::vector<HudLayoutLine> out;
	io::ConfigTokens tokens;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		size_t length = end - start;
		if (length > 0 && text[start + length - 1] == '\r') --length;
		// The line as the reader's string, up to its first NUL.
		const std::string line = text.substr(start, length);
		io::tokenize_config_line(line.c_str(), tokens);
		// No token, or a first token opening with '/': no line [orig: File_ParseASCIIFile @ 0x53D915 / @ 0x53D91E].
		if (tokens.count > 0 && tokens.tokens[0][0] != '/') {
			HudLayoutLine row;
			row.key = tokens.tokens[0];
			if (tokens.count > 1) row.first = tokens.tokens[1];
			row.offset = start;
			row.length = length;
			out.push_back(std::move(row));
		}
		if (end == text.size()) break;
		start = end + 1;
	}
	return out;
}

const HudLayoutLine *hud_layout_line(const std::vector<HudLayoutLine> &lines, const char *key, const char *first) {
	// The last line of the key: each writes the same globals over the one before
	// [orig: HUD_ParseHudposToken @ 0x59F370, the StaticFrame copy @ 0x5a0a4e..0x5a0a8a].
	const HudLayoutLine *found = nullptr;
	for (const HudLayoutLine &line : lines)
		if (strutil::iequals(line.key, key) && (!first || same_value(line.first, first))) found = &line;
	return found;
}

} // namespace opennova::def
