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
// kept while the document and the graph stand.
class StylesView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;

	// How many times the lines' value uses were started again: once per change of the document
	// (its load and revision) or of the graph, never for a frame.
	size_t uses_made() const { return uses_made_; }

private:
	// What each variable the table draws has its value used as, and the menus' uses of it.
	struct LineUse {
		StyleValueUse use;
		std::vector<const GraphEdge *> users;
	};
	void refresh_lines(const MnsDocument &document);
	const LineUse &use_of(const MnsDocument &document, const NodeAddress &line, const AssetGraph *graph);

	char filter_[128]{};
	// The lines the table lists (the variables and the lines that decide what the game reads, by
	// their place among the rows) and those the filter shows, made when the document (its
	// identity, load and revision) or the filter moved.
	struct Lines {
		bool made = false;
		uint64_t document = 0, load = 0, revision = 0;
		std::string filter;
		std::vector<size_t> listed, shown;
	};
	Lines lines_;
	// Each drawn variable's use, started again when the document (its identity, load and
	// revision) or the graph (the object and its generation) moved.
	struct Uses {
		uint64_t document = 0, load = 0, revision = 0;
		const AssetGraph *graph = nullptr;
		uint64_t generation = 0;
		std::unordered_map<NodeId, LineUse> rows;
	};
	Uses uses_;
	size_t uses_made_ = 0;
	FindingsIndex findings_; // the stylesheet's Problems rows, for what the game makes of it
	ReferencePicker picker_;
	RecordReveal reveal_;
};

} // namespace opennova::editor
