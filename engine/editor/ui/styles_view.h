#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/graph/style_value_use.h>
#include <editor/session/view/findings_index.h>
#include <editor/ui/document_views.h>
#include <editor/ui/record_reveal.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

class AssetGraph;
class MnsDocument;
struct GraphEdge;

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
// move, duplicate or go. S13 V3: one view per open stylesheet (a DocumentView), the table
// clipped (only the lines in sight draw), the lines it lists kept while the document and the
// filter stand, and what each variable's value is used as (graph/style_value_use) and its uses
// kept while the document and the graph stand. S13 V8: an edit, an undo or a redo (the document's
// revision alone) is followed through its change set: a changed or added line matched against the
// filter again and the places made again from the rows' order, the others' matches kept; a changed
// variable's use made again with those of the lines of its name (which of them the game reads may
// have moved), every other kept, rows added, removed or moved starting the uses again.
class StylesView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;

	// How many times the lines' value uses were started again: once per change of the graph, or of
	// the document that its change set cannot follow (another load, rows added, removed or moved),
	// never for a frame.
	size_t uses_made() const { return uses_made_; }
	// How many lines' value uses were made (only the lines drawn ask), and how many lines were
	// matched against the filter (each a value read).
	size_t lines_used() const { return lines_used_; }
	size_t lines_matched() const { return lines_matched_; }

private:
	// What each variable the table draws has its value used as, and the menus' uses of it.
	struct LineUse {
		StyleValueUse use;
		std::vector<const GraphEdge *> users;
	};
	void refresh_lines(const MnsDocument &document);
	const LineUse &use_of(const MnsDocument &document, const NodeAddress &line, const AssetGraph *graph);
	// The uses after the document's revision alone moved, from its change set: false when they start
	// again (a change set naming rows added, removed or moved, the file-wide state, or a line that
	// is no variable; none at all).
	bool follow_uses(const MnsDocument &document);

	ui_kit::HeldText<kWorkspaceText> filter_; // the workspace's filter (workspace.document's)
	// The lines the table lists (the variables and the lines that decide what the game reads, by
	// their place among the rows) and those the filter shows, made when the document (its
	// identity, load and revision) or the filter moved; whether the filter keeps each line, by its
	// identity (what a change set leaves alone is not matched again).
	struct Lines {
		bool made = false;
		uint64_t document = 0, load = 0, revision = 0;
		std::string filter;
		std::vector<size_t> listed, shown;
		std::unordered_map<NodeId, bool> matched;
	};
	Lines lines_;
	// Each drawn variable's use, started again when the document (its identity, load and
	// revision) or the graph (the object and its generation) moved, and the name of each variable
	// line the uses were made over (upper case, the name the game's lookup compares).
	struct Uses {
		uint64_t document = 0, load = 0, revision = 0;
		const AssetGraph *graph = nullptr;
		uint64_t generation = 0;
		std::unordered_map<NodeId, LineUse> rows;
		std::unordered_map<NodeId, std::string> names;
	};
	Uses uses_;
	size_t uses_made_ = 0;
	size_t lines_used_ = 0;
	size_t lines_matched_ = 0;
	FindingsIndex findings_; // the stylesheet's Problems rows, for what the game makes of it
	ReferencePicker picker_;
	RecordReveal reveal_;
};

} // namespace opennova::editor
