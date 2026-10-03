#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <editor/documents/name_source.h>
#include <editor/model/document.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

// How an outline lists a document's records (ADR 0046 S13 V3): as a tree (every row, each
// collection a record holds and the records in it, a level at a time: a model, a clip, an
// animation table); as a list of the rows alone (a catalog, whose nested records are the
// Inspector's); or as master and detail (the rows in a column, the records the selected row holds
// beside it as a table of their text fields, edited in place: a string table's sections and
// strings).
enum class OutlineMode { Tree, List, MasterDetail };

// One line of an outline: a record, or in a tree a collection a record holds (its heading, over
// its records), or a heading the type groups its rows under (OutlineSpec::groups: a mission's
// "Organics", "Team 1", "Group 3", over the rows standing under it).
struct OutlineLine {
	NodeAddress address;     // the record; a collection's owner
	bool collection = false; // a collection's heading, of records of `kind`
	bool heading = false;    // a group's heading (`key` its key, `count` the rows under it)
	std::string key;         // a heading's key: its outer headings' keys and its own, '/'-joined
	NodeKind kind = 0;
	int depth = 0;       // how deep in the tree it sits: 0 for a row
	bool branch = false; // it opens: a record that holds collections, a collection holding records
	bool open = false;   // what it holds is listed under it
	// A tree under a filter: open because the filter keeps a record under it, whatever is open
	// otherwise, so its arrow changes nothing (set_open leaves it).
	bool forced = false;
	size_t index = 0;    // a record's place among its owner's records of its kind, a row's among the rows
	size_t count = 0;    // a collection's records
	bool addable = false; // a collection that takes another record at its end (its + tool)
	size_t full = 0;      // a collection holding the most it holds: that most (its + off, saying so)
	int lines = 1;        // master and detail: the lines of text its tallest cell shows (1 to 8)
	// What the line shows: a record's title (record_display, graph/display_names.h: the type's words
	// with the project's names; in a list, a row of another kind than the file's own after its kind's
	// label), a collection's label and count, a heading's words and count; a record's name where the
	// title words it otherwise (for its tooltip), and in master and detail the name of the row holding
	// it; and a record's words for a column too narrow for its title (record_brief: a mission's event by
	// its first trigger's subject and verb), "" where the title cut says it.
	std::string text;
	std::string name;
	std::string row_name;
	std::string brief;
};

// A document's file-wide values as its outline lists them after its records (an item table's
// vehicle spawn slots): whole numbers, each set by an Edit SetFileValue at its place, and one more
// at the end while fewer than `max` are there. What they are called is the hook's.
struct OutlineFileValues {
	std::string title;     // the heading ("Vehicle spawn IDs")
	std::string label;     // each value's, its place after it ("Slot" 1)
	std::string tip;       // what a value is
	std::string add_label; // the tool adding one ("Add spawn slot")
	std::string add_tip;
	std::string full_tip;  // why none is added while `max` are there
	std::vector<int64_t> values;
	size_t max = 0;
};
// The hook of a type whose files hold such values: false for a document that has none.
using OutlineFileValuesHook = bool (*)(const Document &document, OutlineFileValues &out);

// Whether a list or a tree lists `row` while its "all rows" switch is off: the hook of a type whose
// files hold rows most of which stand empty (a mission's 128 paths: one with no stop is not listed).
using OutlineRowListedHook = bool (*)(const Document &document, const Node &row);

class Workspace;
// A collection whose records are added by type (a mission's triggers and actions, S15): whether the
// collection of `kind` is one, and the body of the popup its "+" opens, which offers the types and
// raises the add into `owner`.
using OutlineAddsByMenuHook = bool (*)(NodeKind kind);
using OutlineAddMenuHook = void (*)(Workspace &workspace, const Document &document, const NodeAddress &owner, NodeKind kind);

// A heading a tree groups rows under (OutlineSpec::groups): its key among its siblings, which orders
// them (a mission's pools by their band, its teams and groups by number: "03", "t001"), and its words
// ("Organics", "Team 1", "Group 3"): documents/name_source.h's RowHeading, which a type's hook makes.
using OutlineGroup = RowHeading;
// The headings each row of the document stands under, outermost first, one list per row in the
// document's order (an empty list: a row under no heading, listed before every heading): the hook of
// a type whose rows read better grouped (a mission's entities by pool, a marker's type, team and
// group), the headings worded with the project's names where `names` gives them (a marker's type by
// its item's name). The rows of a heading are listed together under it, the headings in their keys'
// order and each heading's rows in the file's order.
using OutlineGroupsHook = void (*)(const Document &document, const NameSource *names,
                                   std::vector<std::vector<OutlineGroup>> &out);

// What an outline view of a type is (its DocumentViewRow's outline, ui/document_views): its mode,
// the heading of the master column (master and detail: the rows' words, "Sections"), the hook
// of a type whose files hold file-wide values a list shows after its rows (null: none), whether a
// list or a tree filters its rows by kind (a chip per kind of row the file holds, each listing or
// leaving out its kind's rows: a mission's pools, paths, areas and events), and the hook of a type
// some of whose rows are left out until the view's switch lists them (null: every row is listed),
// with the switch's words ("Empty paths"); and the hooks of a type some of whose collections add
// their records by type (null: a "+" adds a blank one).
struct OutlineSpec {
	OutlineMode mode = OutlineMode::Tree;
	const char *rows = "";
	OutlineFileValuesHook file_values = nullptr;
	bool by_kind = false;
	OutlineRowListedHook row_listed = nullptr;
	const char *unlisted = "";
	OutlineAddsByMenuHook adds_by_menu = nullptr;
	OutlineAddMenuHook add_menu = nullptr;
	// A tree's headings over its rows (null: none; ADR 0046 S15).
	OutlineGroupsHook groups = nullptr;
};

// What a click on a record's line selects (OutlineModel::click): the record, how it joins the
// selection, and the records named with it (a Shift click's range).
struct OutlineClick {
	NodeAddress record;
	SelectMode mode = SelectMode::Replace;
	std::vector<NodeAddress> records;
};

// The portable half of a document's outline (S13 V3; ImGui-free, as ui/problems_list and
// ui/find_cursor are): the lines it shows, what is open (the file-wide values' heading too), the
// filter, the order, and in master and detail the columns; the view's filter box, sort and Every
// read and write the model's. The lines are made again only when what they read moves: the
// document (its identity, load and revision), what is open, the filter, the order, the "every row"
// switch, the kinds listed and the "all rows" switch, and in master and detail the master row; drawing reads them as they stand. Each row's
// lines are kept apart (S13 V8), so when the document's revision alone moved (an edit, an undo, a
// redo) the lines follow its change set: a changed or added row's lines made again, a removed row's
// dropped, the rows put in the document's order again where they moved (a list in name order sorted
// again), every other row's kept; everything is made anew when the document cannot say
// (lines_made counts those, rows_made every row's lines made).
class OutlineModel {
public:
	explicit OutlineModel(OutlineMode mode = OutlineMode::Tree,
			OutlineFileValuesHook file_values = nullptr, OutlineRowListedHook row_listed = nullptr,
			OutlineGroupsHook groups = nullptr);

	OutlineMode mode() const { return mode_; }

	// What the lines keep to. The filter, any case, as the game compares names (filtered(): one
	// of blanks alone keeps everything): a list keeps the rows whose name holds it; a tree the
	// records whose title or name holds it and the records and collections holding them, listed
	// open whatever is open otherwise (forced), a record kept for itself opening as it does
	// unfiltered, onto all it holds; master and detail the detail records one of whose columns
	// holds it. The order, a list's alone: its rows by name, a display order (Up and Down keep the
	// file's). Every row, master and detail's alone: while the filter keeps lines (every_row), every
	// row's detail records it keeps, not the master row's alone.
	void set_filter(const std::string &filter);
	const std::string &filter() const { return filter_; }
	bool filtered() const { return !needle_.empty(); }
	void set_sort(bool by_name);
	bool sort() const { return sort_; }
	void set_every(bool every);
	bool every() const { return every_; }
	bool every_row() const { return every_ && filtered(); }
	// The kinds of row a list or a tree lists: a bit per kind in the document's kinds() order
	// (kind_bit), every one set until a view clears one; a row whose kind's bit is clear has no line.
	// And the rows the type's listed hook leaves out (OutlineSpec::row_listed), listed too while the
	// "all rows" switch is on.
	void set_kinds(uint64_t mask);
	uint64_t kinds() const { return kinds_; }
	static uint64_t kind_bit(const Document &document, NodeKind kind);
	void set_all_rows(bool all);
	bool all_rows() const { return all_rows_; }

	// A tree's line opened or closed: a record's (its collections under it), a collection's (its
	// records under it), a heading's (the rows under it; every heading open until closed); one the
	// filter holds open (OutlineLine::forced) is left as it is.
	void set_open(const OutlineLine &line, bool open);
	bool is_open(const OutlineLine &line) const;
	// The file-wide values' heading (a list's, after its rows) opened or closed.
	void set_values_open(bool open) { values_open_ = open; }
	bool values_open() const { return values_open_; }
	// The selection a Go to, a find or a Problems row moves to, shown (`path`: the records holding
	// it, the row first, then the record: RecordReveal's): in a tree every record and collection
	// holding it opened; a filter that would hide its line cleared, its kind listed and the rows left
	// out listed where either hid it, the reveal winning over them, and the headings over its row
	// opened. Its line among lines(document, master, names) as they are then (a list's: the row holding
	// it; a tree's and master and detail's: the record), SIZE_MAX for none (master and detail's rows are
	// its master column's, which lists every one).
	size_t reveal(const Document &document, const std::vector<NodeAddress> &path, NodeId master = 0,
	              const NameSource *names = nullptr);

	// The lines of `document` as they stand (a tree's, a list's, master and detail's detail
	// records), and in master and detail the rows (every one, in the file's order): made again only
	// when what they read moved, after an edit the rows its change set names alone. `master`: master
	// and detail's master row (0: none). `names`: what the records' titles read of the project (ADR 0046
	// S15, graph/display_names.h's record_display: a mission's entity by its item's name), the lines made
	// anew when its generation moves; null: the document's own titles.
	const std::vector<OutlineLine> &lines(const Document &document, NodeId master = 0, const NameSource *names = nullptr);
	const std::vector<OutlineLine> &masters() const { return masters_; }
	// Master and detail: the detail records' kind, their collection's label, and the columns, the
	// kind's text fields the file writes, in its schema's order (made with the lines).
	NodeKind detail_kind() const { return detail_kind_; }
	const char *detail_label() const { return detail_label_; }
	const std::vector<const FieldSchema *> &columns() const { return columns_; }
	// Which line among lines() is the record `address` (SIZE_MAX: none).
	size_t line_of(const NodeAddress &address) const;
	// What a click on the record line `clicked` of lines() selects, `primary` the selection's primary
	// record: its record alone; with Ctrl its record joining the selection or leaving it; with Shift
	// every record line from the primary's to it, both ends in (the collections' headings between them
	// left out), the clicked one the primary, one selection. A Shift click with no primary on the lines
	// (none selected, or its line not listed) selects the record alone. An empty record for a line
	// that is no record's.
	OutlineClick click(size_t clicked, const NodeAddress &primary, bool ctrl, bool shift) const;
	// The document's file-wide values (the hook's); false for none.
	bool file_values(const Document &document, OutlineFileValues &out) const;

	// How many times the lines were made anew (every row's), and how many rows' lines were made: every
	// row's each time the lines are made anew, else the rows a change set names changed or added.
	size_t lines_made() const { return lines_made_; }
	size_t rows_made() const { return rows_made_; }

private:
	// A tree's open line by what it is: a record, or a record's collection of a kind.
	struct OpenKey {
		NodeAddress address;
		bool collection = false;
		NodeKind kind = 0;
		bool operator<(const OpenKey &other) const {
			return std::tie(address.row, address.kind, address.child, collection, kind) <
			       std::tie(other.address.row, other.address.kind, other.address.child, other.collection,
			                other.kind);
		}
	};
	static OpenKey key_of(const OutlineLine &line) { return {line.address, line.collection, line.kind}; }
	struct Key {
		bool made = false;
		uint64_t document = 0, load = 0, revision = 0, open = 0;
		std::string filter;
		bool sort = false, every = false, all_rows = false;
		uint64_t kinds = 0;
		NodeId master = 0;
		bool has_names = false;
		uint64_t names = 0;
		// Everything but the revision the same: the lines may follow the document's change set.
		bool same_but_revision(const Key &other) const {
			return made == other.made && document == other.document && load == other.load &&
			       open == other.open && filter == other.filter && sort == other.sort &&
			       every == other.every && all_rows == other.all_rows && kinds == other.kinds &&
			       master == other.master && has_names == other.has_names && names == other.names;
		}
		bool operator==(const Key &other) const { return same_but_revision(other) && revision == other.revision; }
	};
	// One row's lines, a slot per row in the document's row order: a tree's (the row and what is
	// listed under it), a list's (its line, none where the filter drops it; in name order its name as
	// names compare), master and detail's (the row as a master, and its detail records where they are
	// listed); and in a tree with headings the ones it stands under (OutlineSpec::groups).
	struct RowLines {
		NodeId row = 0;
		OutlineLine master;
		std::vector<OutlineLine> lines;
		std::string order;
		std::vector<OutlineGroup> groups;
	};
	// A tree's headings: each row's (the hook's) put on its lines' slot.
	void make_groups(const Document &document);
	// The tree's lines with its headings: the rows in their headings' order, a heading line where a
	// row's headings first differ from the row before it, a closed heading's rows (and those of the
	// headings under it) left out.
	void join_grouped();
	// Master and detail: the detail records' kind, their label and the columns, from the first
	// collection a row holds.
	void make_detail_columns(const Document &document);
	// The lines of the row at `index` among the document's rows.
	void make_row(const Document &document, size_t index, NodeId master, RowLines &out);
	// A row's lines at its place `index` among the rows (its own line's index).
	void place_row(RowLines &row, size_t index);
	// The rows' lines after a change set (lines()); false when everything is made anew.
	bool follow_changes(const Document &document, const RowChanges &changes, NodeId master);
	// The lines (and in master and detail the masters) from the rows' lines.
	void join_rows();
	// A tree's record and collection lines into `out`, and under a filter whether they were kept.
	void add_record(const Document &document, const NodeAddress &record, int depth, size_t index,
			std::vector<OutlineLine> &out) const;
	void add_collection(const Document &document, const NodeAddress &owner,
			const Document::Collection &collection, int depth, std::vector<OutlineLine> &out) const;
	bool add_filtered_record(const Document &document, const NodeAddress &record, int depth, size_t index,
			std::vector<OutlineLine> &out) const;
	bool add_filtered_collection(const Document &document, const NodeAddress &owner,
			const Document::Collection &collection, int depth, std::vector<OutlineLine> &out) const;
	OutlineLine record_line(const Document &document, const NodeAddress &record, int depth, size_t index,
			bool branch) const;
	bool matches(const std::string &text) const;
	// Whether a list or a tree lists the row: its kind's bit set, and the listed hook keeping it or the
	// "all rows" switch on.
	bool row_listed(const Document &document, const Node &row) const;

	OutlineMode mode_ = OutlineMode::Tree;
	OutlineFileValuesHook file_values_ = nullptr;
	OutlineRowListedHook row_listed_ = nullptr;
	OutlineGroupsHook groups_ = nullptr;
	// The names the titles read while the lines are made (lines()' argument; null after).
	const NameSource *names_ = nullptr;
	// The headings closed (by key): every other one is open.
	std::set<std::string> closed_;
	uint64_t kinds_ = ~uint64_t(0);
	bool all_rows_ = false;
	std::string filter_;
	std::string needle_; // the filter as names compare (normalized_logical_name)
	bool sort_ = false;
	bool every_ = false;
	std::set<OpenKey> open_;
	uint64_t open_version_ = 0;
	bool values_open_ = false;
	Key key_;
	std::vector<RowLines> rows_;
	std::vector<OutlineLine> lines_;
	std::vector<OutlineLine> masters_;
	bool has_detail_ = false; // master and detail: a row holds a collection, the detail's kind
	NodeKind detail_kind_ = 0;
	const char *detail_label_ = "";
	std::vector<const FieldSchema *> columns_;
	size_t lines_made_ = 0;
	size_t rows_made_ = 0;
};

} // namespace opennova::editor
