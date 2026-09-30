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

// The portable half of a document's outline (S13 V3; ImGui-free, as ui/problems_list and
// ui/find_cursor are): the lines it shows, what is open, the filter, the order, and in master and
// detail the columns. The lines are made again only when what they read moves (lines_made counts
// it): the document (its identity, load and revision), what is open, the filter, the order, the
// "every row" switch, and in master and detail the master row; drawing reads them as they stand.
class OutlineModel {
public:
	explicit OutlineModel(OutlineMode mode = OutlineMode::Tree,
			OutlineFileValuesHook file_values = nullptr);

	OutlineMode mode() const { return mode_; }

	// What the lines keep to. The filter, any case, as the game compares names: a list keeps the
	// rows whose name holds it; a tree the records whose title or name holds it and the records
	// and collections holding them, listed open whatever is open otherwise; master and detail the
	// detail records one of whose columns holds it. The order, a list's alone: its rows by name,
	// a display order (Up and Down keep the file's). Every row, master and detail's alone: while a
	// filter is set, every row's detail records it keeps, not the master row's alone.
	void set_filter(const std::string &filter);
	const std::string &filter() const { return filter_; }
	void set_sort(bool by_name);
	bool sort() const { return sort_; }
	void set_every(bool every);
	bool every() const { return every_; }

	// A tree's line opened or closed: a record's (its collections under it), a collection's (its
	// records under it).
	void set_open(const OutlineLine &line, bool open);
	bool is_open(const OutlineLine &line) const;
	// A tree: every record and collection that holds the last record of `path` opened (`path`:
	// the records holding it, the row first, then the record: RecordReveal's), so its line is
	// listed; false when every one was open already.
	bool reveal(const std::vector<NodeAddress> &path);

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
	Key key_;
	std::vector<OutlineLine> lines_;
	std::vector<OutlineLine> masters_;
	NodeKind detail_kind_ = 0;
	const char *detail_label_ = "";
	std::vector<const FieldSchema *> columns_;
	size_t lines_made_ = 0;
};

} // namespace opennova::editor
