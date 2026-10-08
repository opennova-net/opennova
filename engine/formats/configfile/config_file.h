#pragma once

// The ConfigFile: NovaLogic's sectioned key = value configuration, read as the engine reads it. Its text
// form [orig: ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @ 0x7608a0] parses into
// sections and each section's entries, a key and its values each an integer, a float or a string; the
// accessors read them back as the engine's do, a section found by its label and a key from the section's
// read cursor. A marquee's credits read it (runtime/menu/menu_credits.h), charattr.def is one
// (formats/charattr/charattr.h), and the editor's credits document writes a CBIN file's text form so that
// this reader reads it back as the file holds it. The binary form (CBIN) is formats/cbin/binary_config.h.
// Witness records: docs/mnu/menu-re.md ("Marquee credits"), docs/net/novaworld-net-re.md (charattr.def).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::configfile {

struct ConfigValue {
	int type = 4; // 1 integer, 2 float, 4 string
	int32_t integer = 0;
	float real = 0.0f;
	std::string text;  // the value as written
	size_t offset = 0; // where it is written: its first character's byte offset into the text
};

struct ConfigEntry {
	std::string key;
	std::vector<ConfigValue> values;
	size_t offset = 0; // its line's first byte offset into the text
};

struct ConfigSection {
	std::string label; // lowercased
	std::vector<ConfigEntry> entries;
	size_t offset = 0; // its '[' line's first byte offset into the text
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
// @ 0x7606f0; an integer through strtol, a float through atof]. A line is read up to its first NUL.
// (Retail reads each entry's values back through its line walker, whose key keeps leading spaces: a
// key written with leading spaces is not modeled.)
std::vector<ConfigSection> parse_config_text(const uint8_t *data, size_t size);

// [orig: String_ClassifyNumeric @ 0x75d830]: 0 string, 1 integer, 2 float.
int classify_numeric(const std::string &text);

// The first section of the label, without case, its read cursor reset to its first entry; null for
// none. The label table is searched in its order (the sorted bsearch arm is dead: its switch,
// dword_3342A24, has no caller), so of two sections of a label the first is found.
// [orig: ConfigFile_FindSection @ 0x75eeb0 -> ConfigFile_FindLabelLinear @ 0x75ee50; the cursor reset
//  +16 = the first entry, +20 = 0 @ 0x75ef41..0x75ef44]
ConfigSection *find_config_section(std::vector<ConfigSection> &sections, const char *name);

// A read the way the section's accessor reads [orig: effect_get_param_value_0 @ 0x75fa00, which
// ConfigFile_ReadKeyValue @ 0x75fc90 takes for a parsed file]: from the next entry (else the current
// one) to the first whose key matches without case, stopping at an entry with no values; `index` is
// 1-based. Each output is set first (a number 0, a text left as it is); a number read as text is
// printed "%d" or "%f"; an integer read as a float converts, a float read as an integer truncates
// (_ftol2_sse), a string read as a number is 0. False for no such key, or no value `index` of it.
bool read_config_value(ConfigSection &section, const char *key, int index, std::string *text, float *real,
		int32_t *integer);

// The current entry's value `index`, when its key still matches [orig: effect_get_param_value @ 0x75f580,
// which ini_read_key_value_from_current_line @ 0x75f780 takes for a parsed file]: a key's further values
// after read_config_value found it.
bool read_current_config_value(ConfigSection &section, const char *key, int index, std::string *text,
		float *real, int32_t *integer);

// The text reader's "data strings" pool and the clear that runs past it (docs/mnu/menu-re.md, the ConfigFile
// text reader). The parse counts, allocates, then fills [orig: ConfigFile_ParseText @ 0x7608a0]:
// ConfigFile_CountValuesAndStringLengths @ 0x7605d0 totals the values of every section's entries (+0x30) and,
// for each value it reads back as text, its length plus one (+0x3C); ParseText allocates the pool at +0x3C
// bytes [orig: @ 0x7609d7, AudioMem_AllocWithLabel @ 0x759da0 -> FastMem_Alloc @ 0x7697b0, which takes a size
// under 1 as 1 and rounds it up to 64, a 36-byte block header before each block] and clears it with
// memset(pool, 0, +0x30) [orig: @ 0x7609e8]: one byte per value, so a text with more values than the
// rounded pool zeroes the bytes past it, the next heap block's header first (a crash later, at a
// mission start in the witness). The count reads each value back from its entry's own line as the
// accessors' line walk does [orig: ConfigFile_CountCommaSeparatedValues @ 0x75de30 sets the walk's cursor to
// the line; ConfigFile_ReadKeyValue @ 0x75fc90 for the first value, ini_read_key_value_from_current_line @
// 0x75f780 after it]: the line's first 255 bytes [orig: String_CopyN @ 0x75eca0], the key matched as written
// (its first character exactly, the rest without case [orig: @ 0x75fdfd]; a key written with leading spaces
// does not match its own line, and the walk goes on through the lines after it to a '[' line), a value
// split on ',' and ' ' from the line's '=' to ';' or the line's end. A value the walk cannot read (past
// those 255 bytes, or of a key that matched no line) leaves the buffer holding the last one read, which is
// counted again (the buffer's first contents, uninitialised in retail, taken as empty). A file in the
// CBIN form goes to ConfigFile_ParseBinary instead [orig: ConfigFile_LoadFromFile @ 0x760aa3], whose pools
// are each cleared at their own size, and a file of no byte is not parsed [orig: @ 0x760a74].
struct DataStringsPool {
	bool binary = false;       // a CBIN file: no text parse, no such pool
	uint32_t values = 0;       // +0x30: the clear's length
	uint32_t string_bytes = 0; // +0x3C: the pool's size asked
	uint32_t pool_bytes = 0;   // the block FastMem_Alloc gives for it (0 for a file not parsed as text)
	// The bytes the clear writes past the pool; 0 for none.
	uint32_t overrun() const { return values > pool_bytes ? values - pool_bytes : 0; }
};
inline constexpr uint32_t kFastMemStep = 64;        // [orig: FastMem_Alloc @ 0x7697d7]
inline constexpr uint32_t kFastMemHeaderBytes = 36; // [orig: FastMem_Alloc @ 0x769840, block + 9 dwords]
// The bytes FastMem_Alloc gives a request of `size` [orig: FastMem_Alloc @ 0x7697c4..0x7697d7].
uint32_t fastmem_block_bytes(uint32_t size);
// The pool and its clear for a file of these bytes, as ConfigFile_LoadFromFile -> ConfigFile_ParseText sizes
// and clears it.
DataStringsPool data_strings_pool(const uint8_t *data, size_t size);

} // namespace opennova::configfile
