#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <editor/model/document.h>

namespace opennova::editor {

// How an outline lists a document's records (ADR 0046 S13 V3): as a tree (every row, each
// collection a record holds and the records in it, a level at a time: a model, a clip, an
// animation table); as a list of the rows alone (a catalog, whose nested records are the
// Inspector's); or as master and detail (the rows in a column, the records the selected row holds
// beside it as a table of their text fields, edited in place: a string table's sections and
// strings).
enum class OutlineMode { Tree, List, MasterDetail };

// One line of an outline: a record, or in a tree a collection a record holds (its heading, over
// its records).
struct OutlineLine {
	NodeAddress address;     // the record; a collection's owner
	bool collection = false; // a collection's heading, of records of `kind`
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
	int lines = 1;        // master and detail: the lines of text its tallest cell shows (1 to 8)
	// What the line shows: a record's title (Document::record_title; in a list, a row of another
	// kind than the file's own after its kind's label), a collection's label and count; a record's
	// name where the title words it otherwise (for its tooltip), and in master and detail the name
	// of the row holding it.
	std::string text;
	std::string name;
	std::string row_name;
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

// What an outline view of a type is (its DocumentViewRow's outline, ui/document_views): its mode,
// the heading of the master column (master and detail: the rows' words, "Sections"), and the hook
// of a type whose files hold file-wide values a list shows after its rows (null: none).
struct OutlineSpec {
	OutlineMode mode = OutlineMode::Tree;
	const char *rows = "";
	OutlineFileValuesHook file_values = nullptr;
};

// The portable half of a document's outline (S13 V3; ImGui-free, as ui/problems_list and
// ui/find_cursor are): the lines it shows, what is open (the file-wide values' heading too), the
// filter, the order, and in master and detail the columns; the view's filter box, sort and Every
// read and write the model's. The lines are made again only when what they read moves (lines_made
// counts it): the document (its identity, load and revision), what is open, the filter, the
// order, the "every row" switch, and in master and detail the master row; drawing reads them as
// they stand.
class OutlineModel {
public:
	explicit OutlineModel(OutlineMode mode = OutlineMode::Tree,
			OutlineFileValuesHook file_values = nullptr);

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

	// A tree's line opened or closed: a record's (its collections under it), a collection's (its
	// records under it); one the filter holds open (OutlineLine::forced) is left as it is.
	void set_open(const OutlineLine &line, bool open);
	bool is_open(const OutlineLine &line) const;
	// The file-wide values' heading (a list's, after its rows) opened or closed.
	void set_values_open(bool open) { values_open_ = open; }
	bool values_open() const { return values_open_; }
	// The selection a Go to, a find or a Problems row moves to, shown (`path`: the records holding
	// it, the row first, then the record: RecordReveal's): in a tree every record and collection
	// holding it opened; a filter that would hide its line cleared, the reveal winning over it. Its
	// line among lines(document, master) as they are then (a list's: the row holding it; a tree's and
	// master and detail's: the record), SIZE_MAX for none (master and detail's rows are its master
	// column's, which lists every one).
	size_t reveal(const Document &document, const std::vector<NodeAddress> &path, NodeId master = 0);

	// The lines of `document` as they stand (a tree's, a list's, master and detail's detail
	// records), and in master and detail the rows (every one, in the file's order): made anew
	// only when what they read moved. `master`: master and detail's master row (0: none).
	const std::vector<OutlineLine> &lines(const Document &document, NodeId master = 0);
	const std::vector<OutlineLine> &masters() const { return masters_; }
	// Master and detail: the detail records' kind, their collection's label, and the columns, the
	// kind's text fields the file writes, in its schema's order (made with the lines).
	NodeKind detail_kind() const { return detail_kind_; }
	const char *detail_label() const { return detail_label_; }
	const std::vector<const FieldSchema *> &columns() const { return columns_; }
	// Which line among lines() is the record `address` (SIZE_MAX: none).
	size_t line_of(const NodeAddress &address) const;
	// The document's file-wide values (the hook's); false for none.
	bool file_values(const Document &document, OutlineFileValues &out) const;

	// How many times the lines were made.
	size_t lines_made() const { return lines_made_; }

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
		bool sort = false, every = false;
		NodeId master = 0;
		bool operator==(const Key &other) const {
			return made == other.made && document == other.document && load == other.load &&
			       revision == other.revision && open == other.open && filter == other.filter &&
			       sort == other.sort && every == other.every && master == other.master;
		}
	};
	void make_tree(const Document &document);
	void make_list(const Document &document);
	void make_master_detail(const Document &document, NodeId master);
	// A tree's record and collection lines, and under a filter whether they were kept.
	void add_record(const Document &document, const NodeAddress &record, int depth, size_t index);
	void add_collection(const Document &document, const NodeAddress &owner,
			const Document::Collection &collection, int depth);
	bool add_filtered_record(const Document &document, const NodeAddress &record, int depth, size_t index);
	bool add_filtered_collection(const Document &document, const NodeAddress &owner,
			const Document::Collection &collection, int depth);
	OutlineLine record_line(const Document &document, const NodeAddress &record, int depth, size_t index,
			bool branch) const;
	bool matches(const std::string &text) const;

	OutlineMode mode_ = OutlineMode::Tree;
	OutlineFileValuesHook file_values_ = nullptr;
	std::string filter_;
	std::string needle_; // the filter as names compare (normalized_logical_name)
	bool sort_ = false;
	bool every_ = false;
	std::set<OpenKey> open_;
	uint64_t open_version_ = 0;
	bool values_open_ = false;
	Key key_;
	std::vector<OutlineLine> lines_;
	std::vector<OutlineLine> masters_;
	NodeKind detail_kind_ = 0;
	const char *detail_label_ = "";
	std::vector<const FieldSchema *> columns_;
	size_t lines_made_ = 0;
};

} // namespace opennova::editor
