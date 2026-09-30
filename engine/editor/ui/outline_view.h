#pragma once

#include <editor/ui/document_views.h>
#include <editor/ui/outline_model.h>
#include <editor/ui/record_reveal.h>

namespace opennova::editor {

class Document;
struct OutlineLine;

// What an outline view of a type is (its DocumentViewRow's make): its mode, the heading of the
// master column (master and detail: the rows' words, "Sections"), and the hook of a type whose
// files hold file-wide values a list shows after its rows (null: none).
struct OutlineSpec {
	OutlineMode mode = OutlineMode::Tree;
	const char *rows = "";
	OutlineFileValuesHook file_values = nullptr;
};

// A document's records as an outline in its Document tab (ADR 0046 S5, S11d, S11e, S12; S13 V3
// made the catalog's, the string table's and the outline's views one): Reload / Undo / Redo, a
// filter, then the model's lines (ui/outline_model), clipped so a long list draws only what shows,
// each record marked when it was added or changed since the last save and selected on a click; the
// selection a Go to, a find or a Problems row moves there revealed (what holds it opened, its line
// scrolled into view once). Its mode:
// - List (a catalog): the rows with a display-only sort by name, each kind's Add (its
//   RecordKindRow::add_label) and the selected row's Duplicate / Remove / Up / Down, then the file-
//   wide values (an item table's vehicle spawn registry, marked while it differs from the saved
//   file). A row's nested records are the Inspector's.
// - Tree (a model, a clip, an animation table): every row, each collection a record holds (+ adds
//   one at its end where it is not fixed) and each record in it, a level at a time and scrolling
//   sideways when deep; the rows the file adds (one tool per kind with an add label) and the
//   selected record's Duplicate / Remove / Up / Down where its list allows them.
// - MasterDetail (a string table): the rows with their tools in a column that resizes, the
//   selected row's records (or, with Every ticked and a filter set, every row's that match) as a
//   table of their text fields edited in place (ui/text_edit), a text over as many lines as it
//   holds, with their tools; only the table's rows in sight draw their cells.
// Positions and every other field are the generic Inspector's.
class OutlineView final : public DocumentView {
public:
	explicit OutlineView(const OutlineSpec &spec);

	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;

	const OutlineModel &model() const { return model_; }

private:
	void draw_list(Workspace &workspace, const Document &document);
	void draw_tree(Workspace &workspace, const Document &document);
	void draw_tree_line(Workspace &workspace, const Document &document, const OutlineLine &line);
	void draw_tree_tools(Workspace &workspace, const Document &document);
	void draw_master_detail(Workspace &workspace, const Document &document);
	void draw_masters(Workspace &workspace, const Document &document);
	void draw_details(Workspace &workspace, const Document &document, const Node *master);
	void draw_file_values(Workspace &workspace, const Document &document);

	OutlineSpec spec_;
	OutlineModel model_;
	RecordReveal reveal_;
	char filter_[128]{};
	bool sort_ = false;
	bool every_ = false;
};

} // namespace opennova::editor
