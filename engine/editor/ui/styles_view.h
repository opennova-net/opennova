#pragma once

#include <cstdint>

#include <editor/session/view/findings_index.h>
#include <editor/ui/workspace.h>
#include <editor/ui/record_reveal.h>
#include <editor/ui/reference_picker.h>

namespace opennova::editor {

class MnsDocument;

// A menu stylesheet in its Document tab (ADR 0046 S9i, S11d, S11e, S11h2): Reload / Undo /
// Redo, what the game makes of the file (menu_style.mns and brand.mns are read, in that
// order; any other .mns never is), then its variables and the lines that decide what the
// game reads as a table whose columns resize and scroll sideways when narrow: a variable's
// name, value, colour swatch or font / texture picker (the reference picker; a Files row
// dropped on the value sets it too), comment and uses; the #if lines and
// the lines they switch off, locked where they are; each line's number marked when it was
// added or changed since the last save; the line a Go to (a find, a Problems row) selects
// scrolled into view. Comment and blank lines stay in the file, unlisted.
// Adding goes after the selected line when the table lists it, else at the end of the file;
// Up and Down to the place of the listed line before or after it; a locked line does not
// move, duplicate or go. What a value is used as (a colour, a font, a texture) is the
// document's (MnsDocument::style_value_use).
class StylesView {
public:
	void draw(Workspace &workspace, const MnsDocument &document);
	// A RevealRecord event for its document: the next draw shows the selection again.
	void reveal_again() { reveal_.ask(); }

private:
	// What the view keeps the document's answers by (style_value_use's graph key): the graph's
	// counter (view_revisions.h: the menus' uses of a definition); the document keys them by
	// its own revision too.
	static uint64_t cache_key(const SessionView &view) {
		return view.revisions.of(ViewConcern::Graph);
	}

	char filter_[128]{};
	FindingsIndex findings_; // the stylesheet's Problems rows, for what the game makes of it
	ReferencePicker picker_;
	RecordReveal reveal_;
};

} // namespace opennova::editor
