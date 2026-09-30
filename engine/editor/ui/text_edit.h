#pragma once

#include <cstddef>
#include <string>

// A text box over a field's value (ADR 0046 S13 V3): the one way the editor's windows edit a text
// in place (the Inspector's text fields and the box a list of choices narrows by, a string table's
// cells, a stylesheet's names, values and comments). The box's buffer is the caller's own copy of
// the value, sized to hold the whole of it: no buffer is kept per cell, and a value longer than
// the field holds (a UTF-8 text past the schema's width, which the format may still store in
// fewer bytes) is shown whole and never cut, where a box of the field's width had cut it and
// written the cut back on the first keystroke. Typing grows the text up to the field's width
// (its terminator included) and no further: past it, ImGui refuses a typed character or a paste
// whole, never a part of one, so a UTF-8 sequence is never split; a value already longer than
// the width may shrink and not grow. A width of 0 bounds nothing.
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
};

// The box `id` (its label, "##" for none) at the width the caller set, over `text`: true when
// the text changed this frame (with Box::enter_returns, when Enter was pressed), `text` holding
// it.
bool edit(const char *id, std::string &text, size_t width, const Box &box = Box());

// A record's text field edited in place as a table's cell (a string table's key and text, a
// stylesheet's name, value and comment): the whole cell (its column's header names the field: the
// box's id is "##<field>"), over the field's value (edit above: the whole of it), `lines` lines
// high (at most 8; it scrolls past them) where the field runs over several lines (Enter starts a
// new one); a coalesced Set while typing, the edit group ended as the cell lets go. False, drawing
// nothing, where the record has no text in the field.
bool cell(Workspace &workspace, const Document &document, const NodeAddress &address,
		const FieldSchema &field, int lines = 1);

} // namespace text_edit
} // namespace opennova::editor
