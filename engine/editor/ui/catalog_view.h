#pragma once

#include <editor/ui/workspace.h>
#include <editor/ui/record_reveal.h>

namespace opennova::editor {

class DefCatalogDocument;

// An item, weapon or ammo catalog in its Document tab (ADR 0046 S5, S11d, S11e): Reload /
// Undo / Redo, the records with a filter and a display-only sort, each kind's Add and the
// selected record's Duplicate / Remove / Up / Down, each record marked when it was added or
// changed since the last save (the one a Go to, a find or a Problems row selects scrolled into
// view), and items.def's vehicle spawn registry, marked while it differs from the saved file.
// Record fields are the generic inspector's business.
class CatalogView {
public:
	void draw(Workspace &workspace, const DefCatalogDocument &document);

private:
	char filter_[128]{};
	bool sort_names_ = false;
	RecordReveal reveal_;
};

} // namespace opennova::editor
