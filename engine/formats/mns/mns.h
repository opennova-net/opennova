#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::mns {

// The variable references the game's expansion finds in a menu [orig:
// NapiXML_ExpandVariablesInText @ 0x63a000, which UIScene_LoadAndParseContent @ 0x63c830
// runs over the menu's whole text @ 0x63c980, before the XML parse @ 0x63c9a4]: a '%' opens
// a name that runs to the next '%'; a space, '<', '>', '/', '\' or the end stops it first
// [orig: @ 0x63a2aa..0x63a2c8]. An empty name ("%%") or one a stop ends is no reference:
// that '%' stays as written and the scan goes on from the character after it (so the second
// '%' of "%%" may open one) [orig: @ 0x63a2d1..0x63a2e7].
//
// The length of the reference ("%NAME%", both '%'s) the '%' at text[at] opens; 0 when
// text[at] opens none.
size_t variable_reference_at(const std::string &text, size_t at);
// Whether `value` is exactly one reference: the form the menus' per-field expansion
// resolves (ADR 0005; D-MNU-1).
bool is_variable_reference(const std::string &value);
// The name a whole reference holds ("%def_text_fg%" -> "def_text_fg"); `value` as it is
// when it is not one.
std::string variable_name(const std::string &value);
// Whether a reference stands anywhere in `text`.
bool holds_variable_reference(const std::string &text);
// The variables a menu's text names: every reference the game's expansion finds in it
// (variable_reference_at, the scan the game runs over a menu's whole text before its
// parse), its name upper case as the shell's list keys it (KeyValueList::sheet), sorted,
// each once. Every value the frame compiler resolves through the list is one of them, and
// so is a %NAME% inside a longer text, which the game expands too.
std::vector<std::string> variables_named(const std::string &text);
// The variables of two readings of the shell's list (both keyed as the list keys them,
// upper case) that one has and the other lacks, or that hold another value, sorted.
std::vector<std::string> changed_variables(const std::map<std::string, std::string> &before,
                                           const std::map<std::string, std::string> &after);

struct StyleSheet {
	std::unordered_map<std::string, std::string> variables;  // uppercase keys

	// Get a variable value by name (case-insensitive).
	// Returns empty string if not found.
	std::string get(const std::string &name) const;

	// Check if a variable exists (case-insensitive).
	bool has(const std::string &name) const;

	// `text` with each reference (variable_reference_at) the sheet defines replaced by its
	// value, the name compared case-blind; an unknown one is kept whole, and a value is not
	// expanded again. A menu's <RAW_TEXT> sections, which the game's pass copies as they
	// are, are not told apart here.
	std::string substitute(const std::string &text) const;
};

// The shell's variable list: the {name, value, next} nodes at CGameMenu+0x54 [orig:
// CGameMenu_ctor @ 0x63e060 zeroes it @ 0x63e0e5]. One node per name, compared case-blind;
// a later definition replaces the value and keeps the first spelling. Kept here in
// creation order (retail prepends, so its head is the node made last).
struct KeyValue {
	std::string name;
	std::string value;
};
struct KeyValueList {
	std::vector<KeyValue> nodes;
	// The %VAR% table the menus expand through (upper-case keys).
	StyleSheet sheet() const;
};

enum class ReadStatus : uint8_t {
	Read,   // the whole buffer (retail returns 0)
	Failed, // E_FAIL: a define name ended on NUL, CR, LF, '#', '%', '/', '<' or '>'
	Hangs,  // retail loops here forever without advancing; the port stops instead
};
struct ReadResult {
	ReadStatus status = ReadStatus::Read;
	size_t offset = 0; // where the read stopped (Failed, Hangs), a byte offset into the buffer
};

// One stylesheet buffer into `list`, as the game reads it [orig: NapiConfigMap_ParseKeyValueBuffer
// @ 0x639870]; the pairs read before a failure stay in the list (every caller ignores the
// result). See docs/mnu/menu-re.md "The stylesheet reader".
ReadResult parse_key_value_buffer(const char *text, size_t size, KeyValueList &list);

// The 1-based line of a byte offset (CRLF, LF and CR each end a line).
int line_at_offset(const char *text, size_t size, size_t offset);

// Parse MNS stylesheet from memory buffer: the game's own read of it (the lossless
// Document's evaluate()). False, with the first error's line and message, when the game
// stops before the end or would stop responding; `out` holds what it read either way.
bool parse(const char *data, size_t size, StyleSheet &out, std::string &error);

// Write stylesheet to binary buffer.
bool write(const StyleSheet &sheet, std::vector<uint8_t> &out, std::string &error);

}  // namespace opennova::mns
