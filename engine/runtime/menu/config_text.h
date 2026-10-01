#pragma once

// A ConfigFile's text form read as the engine reads it [orig: ConfigFile_LoadFromFile @ 0x760a10 ->
// ConfigFile_ParseText @ 0x7608a0]: its sections and each section's entries, a key and its values
// each an integer, a float or a string. A marquee's credits read it (menu_credits.h), and the
// editor's credits document writes a CBIN file's text form so that this reader reads it back as the
// file holds it. Witness record: docs/mnu/menu-re.md ("Marquee credits").

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

struct ConfigValue {
	int type = 4; // 1 integer, 2 float, 4 string
	int32_t integer = 0;
	float real = 0.0f;
	std::string text; // the value as written
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

// [orig: ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @ 0x7608a0]: CR LF ends a line
// (a lone CR or LF does not), a tab reads as a space, and the buffer gains a trailing LF. A line
// opening with '[' and one or more of A-Z, '_' and the digits is a section, its label lowercased
// [orig: ConfigFile_BuildSectionLabels @ 0x75df20]. A section's entries are its lines up to the next
// line opening with '[' that match "%[^;\n\r=]=%[^\n\r;]" with both parts: the key with its spaces
// trimmed, the value up to ';' or the line end [orig: ini_parse_section_entries @ 0x75db80]; the
// value's tokens split on ',' and ' ' [orig: ConfigFile_CountCommaSeparatedValues @ 0x75de30], each
// an integer, a float or a string by String_ClassifyNumeric @ 0x75d830 [orig: ConfigFile_ParseValues
// @ 0x7606f0]. (Retail reads each entry's values back through its line walker, whose key keeps
// leading spaces: a key written with leading spaces is not modeled.)
std::vector<ConfigSection> parse_config_text(const uint8_t *data, size_t size);

// [orig: String_ClassifyNumeric @ 0x75d830]: 0 string, 1 integer, 2 float.
int classify_numeric(const std::string &text);

} // namespace opennova::menu
