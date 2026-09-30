#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/session/problem_fixes.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// What a Files row carries while it is dragged: its project-relative path, NUL-ended.
inline constexpr const char *kFileDragPayload = "opennova.file";

// The reference picker (ADR 0046 S12): the one widget that picks the name a reference field
// holds, in the Inspector's rows, its tables' cells and the stylesheet view. Its button (Pick,
// or "..." in a table cell) opens a popup listing what the field may name (the graph's choices
// in the field's scope, Document::reference_choices): each name with where it is defined and,
// where the field would not reach it, what it would be (Missing); the names defined only where
// no lookup of the game finds them are behind "Show unreachable", each with why. Typing narrows
// the list, the arrows move through it, Enter picks, Escape closes. While the field's value is
// missing, a footer offers what Problems offers for it (fixes_for on the reference.missing
// finding the graph would make: Create, Import, a placeholder; the variable's, for a %NAME% the
// stylesheets do not define). Each popup keeps its own filter and its own "Show unreachable", by
// its id, its document and its record (every field of every record its own); a picker instance
// holds them for the window drawing it, and forgets a document's once it is closed.
class ReferencePicker {
public:
	// The button and its popup for `field` (a reference, as it applies to `record`), whose value
	// is `value`: true when a name was picked this frame, `picked` holding it (the caller sets
	// it). A fix the footer offers is raised through the workspace. `others`: the names of what the
	// value may name instead are offered too (the kind's also_offers: a menu's stylesheet
	// variables); a stylesheet's own value names its file alone.
	bool draw(Workspace &workspace, const Document &document, const NodeAddress &record, const FieldUse &field,
	          const Value &value, bool compact, std::string &picked, bool others = true);
	// A Files row dropped on the item just drawn (a reference field's value): true when the file is
	// one a reference of the field's kind loads (file_serves_reference, by the field's loader_arg),
	// `picked` its logical name. While a row is dragged over an item that does not fit, nothing
	// is highlighted and nothing happens on release.
	static bool accept_file(const SessionView &view, const FieldUse &field, std::string &picked);
	// How many times a popup's list was made (the graph's choices, the missing value's finding and
	// its fixes): once per opening while what it reads stands (ListKey), never for a change of
	// anything else (a line of Output, a build's step).
	size_t lists_made() const { return lists_made_; }

private:
	// What a popup's list reads: of the view, the graph (the choices, the finding), the files, the
	// project and the editor's settings (the fixes); the document as it is now (the field's value
	// and scope: its instance and revision).
	struct ListKey {
		RevisionKey view;
		uint64_t document = 0;
		uint64_t revision = 0;
		bool operator==(const ListKey &other) const {
			return view == other.view && document == other.document && revision == other.revision;
		}
		bool operator!=(const ListKey &other) const { return !(*this == other); }
	};
	static ListKey cache_key(const SessionView &view, const Document &document);
	// Whose a popup's state is: the document (its instance), the record, the popup's id.
	struct Key {
		uint64_t document = 0;
		NodeId row = 0;
		NodeKind kind = 0;
		NodeId child = 0;
		uint32_t popup = 0;
		bool operator<(const Key &other) const {
			return std::tie(document, row, kind, child, popup) <
			       std::tie(other.document, other.row, other.kind, other.child, other.popup);
		}
	};
	// One popup's own state.
	struct Popup {
		char filter[64] = {};
		bool unreachable = false;  // the inert names shown
		size_t cursor = 0;         // the highlighted row among those shown
		bool moved = false;        // the cursor moved this frame: its row scrolled to
		// What the popup lists, kept while what it reads stands (cache_key).
		const SessionView *view = nullptr;
		ListKey key;
		std::vector<ReferenceChoice> choices;
		bool missing = false;
		std::vector<ProblemFix> fixes;
	};
	void refresh(Popup &popup, const SessionView &view, const Document &document, const NodeAddress &record,
	             const FieldUse &field, const Value &value, bool others);
	bool draw_popup(Workspace &workspace, Popup &popup, std::string &picked);
	// The popups of documents no longer open forgotten, once per change of which are open.
	void prune(const SessionView &view);

	std::map<Key, Popup> popups_;
	const SessionView *pruned_view_ = nullptr;
	RevisionKey pruned_key_;
	size_t lists_made_ = 0;
};

} // namespace opennova::editor
