// The .mnu XML reader: a structural translation of retail's reader
// [orig: NapiXML_ParseElementTree @ 0x769d70; XML_ParseCharEntity @ 0x769cc0]
// over the wide text the loader decodes (mnu::parse does the decode, as
// XML_ParseWithBOMDetection @ 0x76a690 does). It is not an XML parser: it builds
// exactly the tree retail's reader builds, quirks included (docs/mnu/menu-re.md,
// "The reader"):
// - A tag name runs to whitespace or '>', so <X/> is an element named "X/" that
//   stays open. Any </...> closes one level without checking the name.
//   <?xml, <!DOCTYPE and <![CDATA[ are ordinary elements.
// - Attribute values are copied raw: a quoted value ends at the first quote of
//   either kind and keeps it (every consumer then takes wcstok(value, L"\"")), an
//   unquoted one ends at whitespace, '>' or '"'. A bare attribute has no value.
//   '=' must follow the name directly. Attributes attach to the element created
//   last; the list is kept here in authored order (retail prepends).
// - Entities are decoded in element text only; text is kept whole (no trim, no
//   collapse), the text on both sides of a child element joined into one string.
// - Non-whitespace text outside any element fails the file; so does the end of the
//   text inside a tag name or a RAW_TEXT body (retail's E_FAIL: the file loads
//   nothing). Where retail crashes or hangs instead, the reader stops gracefully:
//   parse() fails, or keeps what it built with a note, as each case says.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace opennova::mnu_xml {

// The reader works on wide characters, as retail does after the decode.
using Text = std::u32string;

struct Attribute {
	Text name;
	Text value;             // raw, as retail stores it: a quoted value keeps its closing quote
	bool has_value = false; // false for a bare attribute (retail's NULL value)
	mutable bool read = false; // set when the typed layer consumes it (mnu::parse's notes)

	// wcstok(value, L"\"") [orig: every attribute consumer, the wcstok @ 0x76e883
	// with the one-character delimiter @ 0x7e0728]: the first run of characters that
	// are not '"'. False when there is none (an empty or bare value), where retail's
	// comparison or conversion faults (docs/mnu/menu-re.md, "Crash and hang cases").
	bool token(Text &out) const;
};

struct Node {
	Text tag;
	Text text;                          // every character between tags, joined, entities decoded
	std::vector<Attribute> attributes;  // authored order
	std::vector<std::unique_ptr<Node>> children;
	Node *parent = nullptr;
	size_t line = 0;                    // 1-based line of the '<' that opened it
	int screen_ordinal = -1;            // a top-level SCREEN's index among the top-level SCREENs
	mutable bool read = false;          // set when the typed layer consumes it

	// The attribute a consumer that walks the whole list and overwrites keeps: the
	// first authored of that name (retail's list is in reverse order). Null when absent.
	const Attribute *attr(const char *name) const;
	// The attribute a consumer that stops at the first list entry keeps: the last
	// authored (the widget factory's TYPE [orig: CUIScene_CreateWidgetByType @ 0x64f630]).
	const Attribute *last_attr(const char *name) const;
};

// What the reader kept going past: a retail crash or hang it stops at gracefully.
struct Note {
	size_t line = 0;
	std::string message;
};

struct Document {
	std::vector<std::unique_ptr<Node>> roots;
	std::vector<Note> notes;
};

// Read wide text (the loader's decode; the first NUL ends it, as the loader's
// NUL-terminated buffer does). False, with `error`, where retail's reader returns
// E_FAIL or where it would crash; `out` then holds nothing.
bool parse(const Text &text, Document &out, std::string &error);

// The CRT primitives retail's reader and its consumers use, on wide characters.
// iswspace [orig: CRT_iswctype @ 0x77f5b1]: the CRT's table below 0x100 (0x09-0x0D,
// 0x20 and 0xA0), the platform's Unicode spaces above it.
bool is_space(char32_t c);
// CRT_wcsicmp @ 0x77085b's keyword compare: equal after folding A-Z only.
bool iequals(const Text &a, const char *ascii);
// CRT_wcstoxl @ 0x76e93b (wcstol / wcstoul): leading iswspace skipped, a sign, "0x"
// for base 16, CRT_wchartodigit @ 0x77fd43's digits and Latin letters, saturating
// on overflow (wcstol to LONG_MAX / LONG_MIN, wcstoul to ULONG_MAX), 0 when no digit.
long wcstol(const Text &s, int base);
unsigned long wcstoul(const Text &s, int base);
// ASCII text as wide text, and wide text as ASCII for messages (others as '?').
Text widen(const char *ascii);
std::string ascii(const Text &text);

// An element's (or one of its attributes') path, upper-cased: a top-level SCREEN
// numbered among the top-level SCREENs, a WINDOW named by its NAME token,
// "/SCREEN[0]/WINDOW[MAIN]/STRING@TRIGGER". mnu::ParseNote and the round-trip
// key-coverage test share it.
std::string path_key(const Node &node, const Attribute *attribute = nullptr);

} // namespace opennova::mnu_xml
