#pragma once

#include <editor/ui/editor_host.h>
#include <editor/ui/record_reveal.h>

namespace opennova::editor {

struct Node;
class StringsDocument;

// A string table in its Document tab (ADR 0046 S6b, S11d, S11e, S12): Reload / Undo / Redo,
// a filter over the keys and texts (over the selected section's strings, or every section's
// with Every section ticked), its sections (Add, Duplicate, Remove, Up, Down) in a column
// that resizes, and the strings (Add, Duplicate, Remove, Up, Down) as a key / text table
// edited in place, a text over as many lines as it holds; each section and string marked
// when it was added or changed since the last save; the section and the string a Go to (a find,
// a Problems row) selects scrolled into view. Positions and the rest are the generic
// inspector's business.
class StringsView {
public:
	void draw(EditorHost &host, const StringsDocument &document);

private:
	// `section` the selected one (its tools; null: none selected, listed only across every
	// section).
	void draw_strings(EditorHost &host, const StringsDocument &document, const Node *section);

	char filter_[128]{};
	bool every_section_ = false;
	RecordReveal reveal_;
};

} // namespace opennova::editor
