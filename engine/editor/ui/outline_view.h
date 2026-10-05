#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/session/view/findings_index.h>
#include <editor/ui/document_views.h>
#include <editor/ui/outline_model.h>
#include <editor/ui/record_reveal.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

// A document's records as an outline in its Document tab (ADR 0046 S5, S11d, S11e, S12; S13 V3
// made the catalog's, the string table's and the outline's views one): Reload / Undo / Redo, a
// filter, then the model's lines (ui/outline_model, which holds the filter, the order and what is
// open), clipped so a long list draws only what shows, each record marked when it was added or
// changed since the last save and selected on a click; the selection a Go to, a find or a Problems
// row moves there revealed (what holds it opened, a filter hiding it cleared, its line scrolled into
// view once). Its mode (its row's OutlineSpec):
// A list or a tree may filter its rows by kind (OutlineSpec::by_kind: a chip per kind of row the file
// holds) and leave out the rows its type says hold nothing until a switch lists them
// (OutlineSpec::row_listed); in both a click selects a record, Ctrl joining or leaving the selection
// and Shift selecting the lines from the primary's to it.
// - List (a catalog): the rows with a display-only sort by name, each kind's Add (its
//   RecordKindRow::add_label) and the selected row's Duplicate / Remove / Up / Down, then the file-
//   wide values (an item table's vehicle spawn registry, marked while it differs from the saved
//   file). A row's nested records are the Inspector's.
// - Tree (a model, a clip, an animation table, a mission): every row, each collection a record holds
//   (+ adds one at its end where it is not fixed) and each record in it, a level at a time and
//   scrolling sideways when deep, under the headings its type groups the rows under where it has them
//   (OutlineSpec::groups, ADR 0046 S15: a mission's pools, teams and groups, each with its count,
//   opened and closed as a record is); the rows the file adds (one tool per kind with an add label)
//   and the selected record's Duplicate / Remove / Up / Down where its list allows them.
// A record's line reads as the display names word it (ADR 0046 S15: record_display with the graph's
// names, a mission's entity by its item's name and its SSN), and one a finding is on is marked with
// the worst one's severity, every finding on it in the mark's tooltip (the view's findings index).
// - MasterDetail (a string table): the rows with their tools in a column that resizes, the
//   selected row's records (or, with Every ticked and a filter set, every row's that match) as a
//   table of their text fields edited in place (ui/text_edit), a text over as many lines as it
//   holds, with their tools; only the table's rows in sight draw their cells, and the row past
//   each edge of what shows, the one being revealed and the one whose cell has the keyboard.
// Positions and every other field are the generic Inspector's.
class OutlineView final : public DocumentView {
public:
	explicit OutlineView(const OutlineSpec &spec);

	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;
	OutlineModel *outline() override { return &model_; }

private:
	// The filter's box over the model's filter (`width` 0: the rest of the line), which it shows
	// as it stands (a reveal clears it) and sets as it is typed.
	void filter_box(const char *hint, float width, const char *tip);
	void draw_kinds(const Document &document);
	void draw_list(Workspace &workspace, const Document &document);
	void draw_tree(Workspace &workspace, const Document &document);
	void draw_tree_line(Workspace &workspace, const Document &document, const OutlineLine &line, size_t index);
	void draw_tree_tools(Workspace &workspace, const Document &document);
	void draw_master_detail(Workspace &workspace, const Document &document);
	void draw_masters(Workspace &workspace, const Document &document);
	void draw_details(Workspace &workspace, const Document &document, const Node *master, size_t revealed);
	void draw_file_values(Workspace &workspace, const Document &document);

	// What the outline shows of its document is the workspace's (the MCP gaps lane: workspace.document's
	// filter, kinds, all_rows, sort and every): taken into the model where the session's moved, before the
	// view draws; what the view changed of the model as it drew (a person's control, a reveal clearing a
	// filter that hid its record) sent to the session after.
	struct Shown {
		std::string filter;
		uint64_t kinds = ~uint64_t(0);
		bool all_rows = false, sort = false, every = false;
		bool operator==(const Shown &other) const {
			return filter == other.filter && kinds == other.kinds && all_rows == other.all_rows && sort == other.sort &&
			       every == other.every;
		}
	};
	Shown shown() const;
	void follow_workspace(const SessionView &view, const Document &document);
	void send_workspace(Workspace &workspace, const Document &document, const Shown &before);

	// The mark after a record's line where a finding is on it (none drawn for none).
	void finding_mark(const SessionView &view, const Document &document, const NodeAddress &address);
	// Master and detail: the widest value of the column whose field defines a record's name (a string's
	// key) over `lines`, its box's padding on top, measured again only when the document's revision or
	// the lines move; and the widest row name of the master column, the same way.
	float defining_width(const Document &document, const std::vector<OutlineLine> &lines, const FieldSchema &field);
	float masters_width(const Document &document);

	OutlineSpec spec_;
	OutlineModel model_;
	RecordReveal reveal_;
	FindingsIndex findings_;
	char filter_[kWorkspaceText]{};  // the filter box's text: the model's filter
	ui_kit::Held<Shown> held_; // the workspace's, as last taken
	NodeAddress editing_; // master and detail: the record whose cell had the keyboard last frame
	struct Measured {
		float width = 0.0f;
		uint64_t document = 0, revision = ~uint64_t(0);
		size_t lines = SIZE_MAX;
		const void *first = nullptr;
	};
	Measured defining_, masters_; // defining_width's and masters_width's
};

} // namespace opennova::editor
