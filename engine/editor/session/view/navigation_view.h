#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// Where the person is in the editor, as the navigation history keeps it (CONTEXT.md "Navigation
// history"): the pane that shows it, the file, and in a document the record selected there. Back and
// Forward take the person to one again (session/navigation_controller.h); the windows draw them on
// Back's and Forward's tooltips and lists, the editor MCP reads them in the navigation section.
struct NavigationPlace {
	// The pane that shows it: a document's tab in the Document window (its record selected), the page of
	// a file the editor has no editor for (its About tab there, where a Go to whose target the editor does
	// not edit lands, DI-17), or Files with the file selected (Show in Files, a file's card).
	enum class Pane : uint8_t { Document, Page, Files };
	Pane pane = Pane::Document;
	// The file, project-relative; "" nowhere (no document open), a place the history never keeps.
	std::string path;
	// In a record document, the primary record selected there: by its address in the document instance
	// `document` (DocumentBase::identity) while that instance stands, and by its locator, which finds it
	// again in the document read again (closed and opened, renamed, changed outside the editor); none
	// selected, neither. In a text document, `locator` alone: the place a Go to showed ("line:column"; ""
	// the text as its view left it). On a file's page, `locator` and `field`: the record and its field a Go
	// to marked there (DI-17; both "" the page alone).
	NodeAddress record;
	uint64_t document = 0;
	std::string locator;
	std::string field;
	// In words, for Back's and Forward's tooltips and lists: "weapons.def: M16", "intro.wac, line 12",
	// "About hit.wav", "About isle.trn: polytrn_colormap", "hit.wav in Files".
	std::string label;

	bool nowhere() const { return path.empty(); }
};

// Whether two places are one: the same pane and file, and in a document the same record (the same
// address in the same instance, or the same locator) or neither naming one; on a page the same record and
// field marked, or none.
bool same_place(const NavigationPlace &a, const NavigationPlace &b);

// A pane's token on the wire: document, page, files.
const char *navigation_pane_token(NavigationPlace::Pane pane);

// The navigation history as the windows and the wire show it (the Navigation concern, kept by the
// session's NavigationController): the places Back goes to and those Forward goes to, nearest first.
struct NavigationView {
	std::vector<NavigationPlace> back;
	std::vector<NavigationPlace> forward;
	bool can_back() const { return !back.empty(); }
	bool can_forward() const { return !forward.empty(); }
};

} // namespace opennova::editor
