#pragma once

#include <cstddef>
#include <string>

// A text box over a field's value (ADR 0046 S13 V3): the one way the editor's windows edit a text
// in place (the Inspector's text fields and the box a list of choices narrows by, a string table's
// cells, a stylesheet's names, values and comments). The box's buffer is the caller's own copy of
// the value, sized to hold the whole of it: no buffer is kept per cell, and a value longer than
// the field holds is shown whole and never cut, where a box of the field's width had cut it and
// written the cut back on the first keystroke. What is typed grows the text up to the field's
// width (its terminator included) and no further: counted in the value's own bytes (a def's, a
// menu's, a stylesheet's field), or in characters for a field its file stores in the game's code
// page (Box::code_page: a string table's, one byte a character there, up to three in UTF-8). Past
// the width an insertion is refused whole, never a part of it, so a UTF-8 sequence is never split:
// counted in bytes, by Dear ImGui's own bound on the buffer (a selected span a refused paste would
// have replaced is still removed: it deletes the span first); counted in characters, by an edit
// callback that puts back the text the frame began with (the span too), the buffer room for the
// width's characters at three bytes each. A value already past the width may shrink and not grow.
// A width of 0 bounds nothing.
namespace opennova::editor {

class Document;
class Workspace;
struct FieldSchema;
struct NodeAddress;

namespace text_edit {

struct Box {
	const char *hint = nullptr; // shown while the box is empty (a list's filter, "(mixed)")
	bool multiline = false;     // Enter starts a new line; the box `height` high (0: a line)
	float height = 0.0f;
	bool enter_returns = false; // edit() answers true for Enter alone, not for each change
	bool code_page = false;     // the width counts characters (FieldSchema::code_page), not bytes
};

// The box `id` (its label, "##" for none) at the width the caller set, over `text`: true when
// the text changed this frame (with Box::enter_returns, when Enter was pressed), `text` holding
// it.
bool edit(const char *id, std::string &text, size_t width, const Box &box = Box());

// A record's text field edited in place as a table's cell (a string table's key and text, a
// stylesheet's name, value and comment): the whole cell (its column's header names the field: the
// box's id is "##<field>"), over the field's value (edit above: the whole of it, its width as the
// field counts it), `lines` lines high (at most 8; it scrolls past them) where the field runs over
// several lines (Enter starts a new one); a coalesced Set while typing, the edit group ended as the
// cell lets go. False, drawing nothing, where the record has no text in the field.
bool cell(Workspace &workspace, const Document &document, const NodeAddress &address,
		const FieldSchema &field, int lines = 1);

} // namespace text_edit
} // namespace opennova::editor
