#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/document.h>
#include <editor/model/record_ids.h>
#include <editor/model/table_shape.h>

namespace opennova::editor {

// A row of a table document (ADR 0046 S13 D10): a record of one of its table's kinds that is a row of
// the file (RecordKindRow::top), its native struct the type's (record()), and the identities of
// everything it holds in the shape of its kind's lists (RecordIds; the row's own identity is the
// Node's, so `ids.id` stays 0). Immutable once committed, as any row: a batch changes its clone.
struct TableRow : Node {
	RecordIds ids;
	// The native record the row is, of the row's kind: what its table's kind reads and holds.
	virtual RecordHandle record() const = 0;
	// The identities of everything the row holds, list by list (RecordIds' order).
	void for_each_identity(const std::function<void(NodeId &)> &fn) override;

protected:
	// What the row's identities add to its footprint.
	size_t ids_footprint() const { return footprint_of(ids); }

private:
	// A table row keeps its identities in `ids`, never in the flat store a Node of another type keeps.
	using Node::collections;
};

// The record document over a table (ADR 0046 S13 D10): every record hook a type of records declares
// from its rows (model/table_shape.h), so a type is its table, its parse and its writer, its new rows,
// and what the game makes of a record that no row can say. From the table: the kinds, a kind's fields,
// the collections an owner holds and the walk over them (each list's records with the identities kept
// beside them), a field's read, write and presence, and the list edits (Add, Duplicate, Remove, Move
// through the list's ops, the identities edited the same way at the same index). A record is found by
// its path in its row (Document::path_in), each step a list of its owner's kind and an index there.
// What the type keeps for itself are the hooks below: whether the game reads a list where its owner
// sits, a rule of its own about a value or a list edit, what a new or copied record is called, and
// whatever else a Document hook says (refine_field, record_choices, refine_symbol, copy and paste,
// renumber_references, accept_step).
class TableDocument : public Document {
public:
	// The type's table, in storage that outlives the document (built once for the process).
	virtual const RecordTable &table() const = 0;

	// The table's kinds; a type whose files hold some of them only (a def catalog's family) answers its
	// file's.
	const std::vector<RecordKindRow> &kinds() const override { return table().kinds(); }
	// An owner's lists, in its kind's order, with the identities beside each.
	std::vector<Collection> collections(const Node &row, const NodeAddress &owner) const override;
	// One pass over the row's records in pre-order, the identities read beside them.
	void walk_records(const Node &row, const RecordVisitor &visit) const override;
	const std::vector<FieldSchema> &fields(NodeKind kind) const override { return table().fields(kind); }

	// One step of a record's trail down from its row: the owner, its identities, and the list and the
	// index it holds the next record at.
	struct TrailStep {
		RecordHandle owner;
		RecordIds *owner_ids = nullptr;
		size_t list = 0, index = 0;
	};
	// A record of a row found by its path, and where it sits: the native record, its identities, the
	// trail from the row down to it (empty for the row itself), and whether the file writes it (every
	// list it lies in, at every depth, is written).
	struct Located {
		RecordHandle record;
		RecordIds *ids = nullptr;
		std::vector<TrailStep> trail;
		bool present = true;
		bool is_row() const { return trail.empty(); }
		const TrailStep &step() const { return trail.back(); }
	};
	// The record `record` names in `row` (0: the row's own record), found by its path in the row as
	// the document holds it or as a batch has left it (Document::path_in); false when the row does
	// not hold it.
	bool locate(const Node &row, NodeId record, Located &out) const;
	// The record a Located's last step holds it in, as a Located of its own (its owner).
	Located owner_of(const Located &record) const;
	// The native record an address names in `row` (the row's own for address.child 0), when it is of
	// the kind the address says; empty otherwise.
	RecordHandle record_in(const Node &row, const NodeAddress &address) const;
	// The first list of `kind` the record holds (its place among its kind's lists).
	bool list_of(const Located &owner, NodeKind kind, size_t &list) const;

	// What the table answered since the counters were last cleared (S13 D10's count-based rule: a
	// keystroke in a field touches that field's row and no other): the labelled fields resolved (by
	// their id, or by the schema a FieldUse points at), the distinct ones among them (each once, by
	// the schema, the first four kept and `several` past them, so counting costs a read nothing that
	// grows), and the kinds resolved. A lookup is one probe of the kind's index, never a walk of its
	// rows.
	struct TableStats {
		static constexpr size_t kDistinctKept = 4;
		size_t fields = 0, kinds = 0;
		std::vector<const FieldSchema *> distinct;
		bool several = false;
	};
	const TableStats &table_stats() const { return stats_; }
	void clear_table_stats() const { stats_ = TableStats(); }

protected:
	TableDocument() = default;
	TableDocument(const TableDocument &other) : Document(other) {}

	// The record an address names, read through its kind's labelled field.
	bool read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const override;
	// A field: its value's presence (always written where it has none). "": whether the file writes
	// the record itself (every list above it written).
	bool read_present(const Node &row, const NodeAddress &address, const std::string &field) const override;
	bool set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override;
	bool set_present(Node &row, const NodeAddress &address, const std::string &field, bool present,
	                 std::string &error) override;
	// Add: the list's new record into the owner edit.parent (0 = the row) at the edit's position.
	// Duplicate: a copy of the record with everything it holds, fresh identities, at the position.
	// Remove: the record with everything it holds. Move: the record with everything it holds to the
	// position of the same kind of list of edit.parent (another owner's: a reparent). The type's
	// rules come first (accept_list_edit), then the list's ops change the native list and the
	// identities beside it change the same way.
	bool edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override;
	// What the labelled field's record decides (its applies and reference functions), on the use the
	// base seeds from the schema; a type that refines further calls this first.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;

	// Gives a row its identities' shape (zero identities for every record it holds), which the load
	// or the batch then numbers: a type's parse and make_node call it for each row they make.
	void shape(TableRow &row) const;

	// Whether the game reads list `list` of `owner` where the owner sits (CollectionSpec::applies);
	// the default, the list's own spec.
	virtual Applicability list_applies(const Node &row, const Located &owner, size_t list) const;
	// A Set of the labelled field at `field` (its place in the record's kind) on a located record:
	// the field's own set. A type checks a rule of its own about the value where the record sits (a
	// window's attributes), or derives beside it what the value decides (a material's shader).
	virtual bool set_value(Node &row, const Located &at, size_t field, const Value &value, std::string &error);
	// A Clear or a Write of an optional field: its value's set_present.
	virtual bool set_written(Node &row, const Located &at, size_t field, bool present, std::string &error);

	// A list edit, before anything changes: what it is, the owner the record is in (or an Add's goes
	// into) and its list, the record (null for an Add), and a Move's destination and its list.
	struct ListChange {
		EditOperation operation = EditOperation::Add;
		const Located *owner = nullptr;
		size_t list = 0;
		const Located *record = nullptr;
		const Located *destination = nullptr;
		size_t destination_list = 0;
	};
	// A rule of the type's own about a list edit (a screen keeps a root window, a material a strip
	// draws with stays): false, with `error`, refuses it. The default accepts.
	virtual bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const;
	// Where an Add or a Duplicate puts its record: the edit's position (a Duplicate's after the
	// record where it names none), clamped to the list by the core. A type's order rule moves it.
	virtual size_t list_position(const Node &row, const ListChange &change, size_t position) const;
	// A record about to go in by a Duplicate or a Move, held apart (its copy): what the type tells
	// apart in a copy (a name no other window of the screen has, a material no strip draws with).
	virtual void prepare_record(const Node &row, const ListChange &change, DetachedRecord &record) const;
	// A record an Add made in its list: what the type makes its new record (a name of its own).
	virtual void after_add(Node &row, const ListChange &change, const RecordHandle &made);

	// Puts `record` in at `position` of the owner's list `list`, its identities fresh from `allocate`
	// (`added` the record's): a type's paste. False, with `error`, where the list takes no more.
	bool insert_record(const Located &owner, size_t list, size_t position, const DetachedRecord &record,
	                   const IdAllocator &allocate, NodeId &added, std::string &error);

	// The labelled field `id` of `kind`, or npos: one probe of the kind's index, counted (TableStats).
	size_t resolve_field(const TableKind &kind, const std::string &id) const;
	const TableKind *resolve_kind(NodeKind kind) const;

private:
	// Follows a row's path from `out` (the row's own Located) down its steps.
	bool descend(const PathStep *steps, size_t size, Located &out) const;
	// The walk below `owner` (a Located kept by the walk, its trail grown and given back one step at
	// a time).
	bool walk(const Node &row, Located &owner, const NodeAddress &self, const RecordVisitor &visit) const;
	void count_field(const FieldSchema &field) const;

	mutable TableStats stats_;
};

// The table document a document is, null for one of another kind: what a caller that reads a record
// through its table asks (a test's count, a type's sibling).
inline const TableDocument *tables_of(const DocumentBase &document) {
	return dynamic_cast<const TableDocument *>(&document);
}

} // namespace opennova::editor
