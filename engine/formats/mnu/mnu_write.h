// The writer's own words for a menu (mnu_write.cpp): each record as the element the writer puts
// down for it, before any layout. The writer's own layout renders them (render_written); the text
// layout (mnu_text_layout.h) models a file's tokens against them and generates the file's look
// from them. Internal to the mnu library: mnu.cpp and the text layout use it, nothing else does.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>

namespace opennova::mnu {

// What a written element is to the model: the record it writes, which decides the attribute
// keys its parse reads (written_vocabulary) and the rules the text layout applies to it.
enum class WrittenKind : uint8_t {
	Screen, Window, Part, Appearance, Sound, Action, Hotkey, String, ToggleString, Items, Item, Row,
	Stencil, Column, Header, Body, Subst, Position, Edge, Element,
	Plain, // an element whose parse reads no attribute (NAME, GROUP, FRAME, FONT, CURSOR, ...)
};

// One attribute the writer puts down: ` name="value"`, or ` name` when bare.
struct WrittenAttribute {
	// What it is to its element: its name upper-cased; an ACTION's FIELD / SOURCE / NAME slot
	// "@FIELD"; a HEADER's table sort key "@SORT0".."@SORT2"; an attribute a parse keeps in
	// order under a name that may repeat (an extra element's, a window's PLAYERLIST /
	// SERVERLIST) its name upper-cased and its place among those of its name, "NAME#0".
	std::string key;
	std::string name;
	std::string value;
	bool bare = false;
	// What follows the name: `="value"`, or nothing when bare.
	std::string value_token() const { return bare ? std::string() : "=\"" + value + "\""; }
};

// How the writer's own layout puts an element down.
enum class WrittenForm : uint8_t {
	Leaf,      // its text and its close tag on its line (the text is the model's)
	Container, // each element in it on a line of its own, then its close tag on its own line
	Compact,   // its text, then its elements, all on its line (an extra element holding both)
};

struct WrittenElement {
	WrittenKind kind = WrittenKind::Plain;
	// What it is within its owner: "#<source>" for a record of a list (Appearance, Window, ...),
	// the singleton's name for one of a kind ("POSITION", "LIST_BOX", "SCROLL_EXTENT"), and
	// "DATASOURCE#<n>" for a DATASOURCE (its text alone, no record).
	std::string key;
	// The list it is a record of within its owner ("" for a singleton): retail reads each list
	// in document order, so the writer keeps each one's order.
	std::string list;
	std::string tag;
	WrittenForm form = WrittenForm::Leaf;
	std::vector<WrittenAttribute> attributes;
	std::string text; // the model's text (escape_text writes it)
	std::vector<WrittenElement> children;
	uint64_t source = 0; // the record's Document::text_layout name (0: none)
	// Digests of what the writer puts down: of its tag, form, attributes and text (`own`), and of
	// that with everything in it (`tree`). The text layout's "as read" checks compare them.
	uint64_t own_digest = 0, tree_digest = 0;
};

// The writer's words for each screen of a document, digests filled.
std::vector<WrittenElement> written_screens(const Document &doc);
// The writer's words for one window's child elements (the issue check's "writes an element").
std::vector<WrittenElement> written_window_elements(const Window &w);

// Whether the parse of an element of `kind` reads an attribute of `key` (its key without a
// "#n" place); an extra element's parse reads every one.
bool written_vocabulary(WrittenKind kind, const std::string &key);
// The key an attribute named `name` has on an element of `kind` (WrittenAttribute::key);
// `place` is its place among the element's attributes of that name (the "#n" of a kept one).
std::string written_key(WrittenKind kind, const std::string &name, size_t place);

// The writer's own layout: a line ending and the step each level of elements is indented by
// (both empty for one line, no indentation).
struct WrittenStyle {
	std::string eol;
	std::string unit;
};
// An element in the writer's own layout; `indent` is what stands before its own first line, so
// each element in it goes on a new line at `indent` and one `unit` more.
void render_written(const WrittenElement &e, const WrittenStyle &style, const std::string &indent,
										std::string &out);
// What stands between an element's open and close tags in the writer's own layout.
void render_written_content(const WrittenElement &e, const WrittenStyle &style, const std::string &indent,
                            std::string &out);
// The open tag of an element in the writer's own layout: `<TAG attributes...>`.
std::string written_open_tag(const WrittenElement &e);

} // namespace opennova::mnu
