#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::def {

// hudpos.def's lines as the game's parser is handed them, and the one it takes a key from: the source-side
// sibling of the writer (def_hudpos_write.h), which says where in a text a key's values are written.

// One line of a hudpos.def text that the game's parser is handed: its key and its first value as
// written (a stance's id, a static frame's texture), and where it is in the text (its line's start
// and its characters, its line end left out). A line is cut at LF, a CR before the LF left out of it
// (the game's walk cuts at CR LF [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5]: the two agree on a
// text whose lines all end CR LF), then into tokens by the game's tokenizer (io::tokenize_config_line,
// the shared ASCII walk's): a line with no token, or whose first token starts with '/', is none
// [orig: File_ParseASCIIFile @ 0x53D915 / @ 0x53D91E].
struct HudLayoutLine {
	std::string key;
	std::string first;
	size_t offset = 0;
	size_t length = 0;
};
std::vector<HudLayoutLine> hud_layout_lines(const std::string &text);
// The line the game takes `key` from (compared without case, as the parser's _stricmp): the last
// one, since each line of a key writes the same globals over the one before [orig:
// HUD_ParseHudposToken @ 0x59F370, e.g. the StaticFrame copy @ 0x5a0a4e..0x5a0a8a]; with `first`
// given, the last whose first value is it (a HUDSTANCE of that id: a number compared by its value, as
// the parser's atof reads it, anything else without case). Null for none.
const HudLayoutLine *hud_layout_line(const std::vector<HudLayoutLine> &lines, const char *key,
		const char *first = nullptr);

} // namespace opennova::def
