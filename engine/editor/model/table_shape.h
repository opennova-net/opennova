#pragma once

// One table shape (ADR 0046 S13 D10): what a record type tells the core of its records, in one form
// for every format. A file's records are the engine's own native structs (a menu's windows, a def
// catalog's items, a model's materials, a mission's entities); the type describes them to the core
// through rows of tables, one table per type, one kind of record per row of it:
//
//   the record handle  a native record, the kind it is and the row's record it lies in
//                      (RecordHandle): what a field reads and writes and a list holds;
//   the value          how one field reads and writes a record, in the units its file writes it, and
//                      whether the file writes it (FieldValue);
//   the choice         the values a field takes by name (ChoiceRow, as constexpr data; FieldChoice);
//   the labelled field what the editor shows of a field (FieldSchema: its label, unit, choices, the
//                      reference it makes) with its value and what a record makes of it where the
//                      record decides (LabelledField);
//   the list ops       an ordered list of records a record holds: its size, the record at an index,
//                      how a record goes in and comes out (ListOps), and what the core calls the
//                      list (TableList).
//
// A kind (TableKind) is its RecordKindRow, its labelled fields in the order the Inspector shows them
// and the lists it holds in the order the file writes them; a table (RecordTable) holds one kind per
// NodeKind. The core's record document over a table (model/table_document.h) answers every record
// hook from it (the kinds, the collections and the walk, the fields, a field's read, write and
// presence, the list edits) and keeps each record's identities beside it in the same shape
// (RecordIds), so a type of records is its table, its parse and its writer, and what the game makes
// of a record that no row can say (a hook). The four forms the editor read the formats through before
// (def's member offsets over a void pointer, the menu's and the model's property tables over a
// handle, the menu's list operations, the mission's setters by name) are this one.
//
// A table is built once for the process (its FieldSchemas are the storage a record's FieldUse points
// into) and never changes; its functions keep no state, so any thread may read it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/model/document.h>
#include <editor/model/record_ids.h>
#include <editor/model/value.h>

namespace opennova::editor {

// --- the record handle -------------------------------------------------------------------------
// A native record of a table and the kind its table's row describes. The struct behind each kind is
// the type's (a menu window's mnu::Window, a def item's DefItemDef); only the kind's own functions
// cast it.
struct RecordHandle {
	NodeKind kind = -1;
	void *data = nullptr;
	// The native record of the row the record lies in (the top of its trail), which a list whose records
	// lie in the row's own tables reads: a mission event's triggers are a range of the file's trigger
	// table. A type whose lists need it gives it to its row's handle and carries it into the handles its
	// ops make (vector_list always does); null where none does.
	void *top = nullptr;
	explicit operator bool() const { return data != nullptr; }
	template <class T> T &as() const { return *static_cast<T *>(data); }
};

// A record held apart from any list (a record between its removal and its insertion elsewhere, a
// duplicate, a clipboard window): its kind and a copy of its native struct, which a list op takes.
struct DetachedRecord {
	NodeKind kind = -1;
	std::shared_ptr<void> data;
	bool shown = true; // a list's record the file leaves out while it is held (a menu window's part)
	RecordHandle handle() const { return {kind, data.get()}; }
};

// --- the value ---------------------------------------------------------------------------------
// How one field reads and writes a record of its kind, in the units the file writes it (a def
// number its line writes, a model's mission axes, a menu's text as the reader keeps it).
struct FieldValue {
	// The value the record holds (false where the record alone cannot say).
	std::function<bool(const RecordHandle &record, Value &out)> get;
	// The record given `value`, its other members as the format derives them; false, with `error`,
	// where the value does not fit (past the field's width or range, a token the reader would read as
	// another). Null for a field shown only (derived, or what the game does with it not witnessed).
	// A Set of the value the field reads changes nothing: the core skips it (Document::apply).
	std::function<bool(const RecordHandle &record, const Value &value, std::string &error)> set;
	// Whether the file writes the field (an optional one: its bit, its line's flag; a block's
	// member: the block too); null: always.
	std::function<bool(const RecordHandle &record)> present;
	// An optional field left out of the file or written again (Clear, Write), its latent value kept;
	// null: always written.
	std::function<bool(const RecordHandle &record, bool present, std::string &error)> set_present;
};

// --- the choice --------------------------------------------------------------------------------
// One value a field takes by name, as constexpr data a table writes (FieldChoice, its runtime form,
// holds its own strings): the token the file writes (or the number's name), the number, and what the
// editor shows ("" = the token).
struct ChoiceRow {
	const char *name = "";
	int64_t value = 0;
	const char *label = "";
};

inline std::vector<FieldChoice> choices_of(const ChoiceRow *rows, size_t count) {
	std::vector<FieldChoice> out;
	out.reserve(count);
	for (size_t i = 0; i < count; ++i) out.push_back({rows[i].name, rows[i].value, rows[i].label});
	return out;
}
template <size_t N> std::vector<FieldChoice> choices_of(const ChoiceRow (&rows)[N]) { return choices_of(rows, N); }

// --- the labelled field ------------------------------------------------------------------------
// A field of a kind: what the editor shows of it and what it takes (its FieldSchema: label, section,
// unit, note, width, range, choices, the reference it makes and the name it defines), its value, and
// what a record makes of it where the record decides: whether the game reads it there (a window type,
// a light that is not a spot) and what it names there (a menu ITEM's text: a string id, a texture or a
// colour by the ITEM's TYPE; a generator's parameter: a register only above style 0x70). Null: the
// schema's (FieldSchema::applies, FieldSchema::reference). A type adds what no row can say (a scope by
// the record's file, a record's own choices) in its refine_field.
struct LabelledField {
	FieldSchema schema;
	FieldValue value;
	std::function<Applicability(const RecordHandle &record)> applies;
	std::function<ReferenceKind(const RecordHandle &record)> reference;
};

// --- the list ops ------------------------------------------------------------------------------
// An ordered list of records a record holds (a window's actions, an item's attachments, a material's
// texture rows, an event's triggers): its native list and nothing else (the identities beside it are
// the core's, RecordIds).
struct ListOps {
	std::function<size_t(const RecordHandle &owner)> size;
	// The record at `index` (empty past the end), of the list's kind.
	std::function<RecordHandle(const RecordHandle &owner, size_t index)> at;
	// A record put in at `index` (the end past it): a copy of `record`, or with null the list's new
	// record (its defaults, which the writer writes and the reader reads back as written); authors
	// what gates the list (an ITEMS block). False, with `error`, where the list takes no more (a
	// part's one window, a material's 24 texture rows).
	std::function<bool(const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                   std::string &error)>
	        insert;
	std::function<bool(const RecordHandle &owner, size_t index)> erase;
	// A copy of the record at `index`, held apart (empty past the end).
	std::function<DetachedRecord(const RecordHandle &owner, size_t index)> copy;
	// Whether the file writes the list's records (an ITEMS block, a part left out); null: always.
	std::function<bool(const RecordHandle &owner)> present;
};

// A list a kind holds: what the core calls it (its CollectionSpec: its records' kind, its label, the
// field that names a record, whether it is fixed, whether the game reads it, the most it holds) and
// its ops.
struct TableList {
	Document::CollectionSpec spec;
	ListOps ops;
};

// --- the kinds and the table -------------------------------------------------------------------
// One kind of record: its row (token, label, add label, whether it is a row of the file), its labelled
// fields in the Inspector's order and the lists it holds in the order the file writes them. `fields`
// is what Document::fields(kind) answers (a record's FieldUse points into it); `values`, `applies`
// and `references` stand beside it by place.
class TableKind {
public:
	TableKind() = default;
	explicit TableKind(const RecordKindRow &row) : row_(row) {}

	TableKind &field(LabelledField field) {
		index_.emplace(field.schema.id, fields_.size());
		fields_.push_back(std::move(field.schema));
		values_.push_back(std::move(field.value));
		applies_.push_back(std::move(field.applies));
		references_.push_back(std::move(field.reference));
		return *this;
	}
	TableKind &list(TableList list) {
		lists_.push_back(std::move(list));
		return *this;
	}

	const RecordKindRow &row() const { return row_; }
	const std::vector<FieldSchema> &fields() const { return fields_; }
	const std::vector<TableList> &lists() const { return lists_; }
	// The labelled field `id` names (its place among the fields), or npos: one lookup, no walk.
	static constexpr size_t npos = SIZE_MAX;
	size_t find(const std::string &id) const {
		const auto found = index_.find(id);
		return found == index_.end() ? npos : found->second;
	}
	// The place of a field this kind's table holds (a FieldUse's schema), or npos for another's.
	size_t place(const FieldSchema *field) const {
		return field >= fields_.data() && field < fields_.data() + fields_.size() ? size_t(field - fields_.data()) : npos;
	}
	// The place of a field: the table's own schema by where it sits, a copy of it a caller kept by its
	// id (one probe of the index); npos for none of this kind's.
	size_t place_of(const FieldSchema &field) const {
		const size_t at = place(&field);
		return at != npos ? at : find(field.id);
	}
	const FieldValue &value(size_t place) const { return values_[place]; }
	const std::function<Applicability(const RecordHandle &)> &applies(size_t place) const { return applies_[place]; }
	const std::function<ReferenceKind(const RecordHandle &)> &reference(size_t place) const {
		return references_[place];
	}

private:
	RecordKindRow row_;
	std::vector<FieldSchema> fields_;
	std::vector<FieldValue> values_;
	std::vector<std::function<Applicability(const RecordHandle &)>> applies_;
	std::vector<std::function<ReferenceKind(const RecordHandle &)>> references_;
	std::vector<TableList> lists_;
	std::unordered_map<std::string, size_t> index_;
};

// A type's records: one kind per NodeKind, in its order (kind(k).row().kind == k), each of its lists
// naming a kind the table holds. Made once for the process.
class RecordTable {
public:
	RecordTable() = default;
	explicit RecordTable(std::vector<TableKind> kinds) : kinds_(std::move(kinds)) {
		for (const TableKind &kind : kinds_) rows_.push_back(kind.row());
	}
	const std::vector<RecordKindRow> &kinds() const { return rows_; }
	const TableKind *kind(NodeKind kind) const {
		return kind >= 0 && size_t(kind) < kinds_.size() ? &kinds_[size_t(kind)] : nullptr;
	}
	const std::vector<FieldSchema> &fields(NodeKind kind) const {
		static const std::vector<FieldSchema> none;
		const TableKind *found = this->kind(kind);
		return found ? found->fields() : none;
	}
	// The table's invariants: each kind at its own place and its token its own; each list naming a
	// kind of the table; each field's id its own within its kind. What a type's tests assert of it.
	bool well_formed() const {
		for (size_t k = 0; k < kinds_.size(); ++k) {
			const TableKind &kind = kinds_[k];
			if (kind.row().kind != NodeKind(k) || !kind.row().token || !*kind.row().token) return false;
			for (size_t other = 0; other < k; ++other)
				if (std::string(kinds_[other].row().token) == kind.row().token) return false;
			for (const TableList &list : kind.lists())
				if (!this->kind(list.spec.kind) || !list.ops.size || !list.ops.at) return false;
			for (size_t f = 0; f < kind.fields().size(); ++f)
				if (kind.find(kind.fields()[f].id) != f || !kind.value(f).get) return false;
		}
		return true;
	}

private:
	std::vector<TableKind> kinds_;
	std::vector<RecordKindRow> rows_;
};

// Zero identities in the shape of a native record and of everything it holds (each list's records,
// all the way down), which a load or an insert gives identities (for_each_identity).
inline RecordIds shape_ids(const RecordTable &table, const RecordHandle &record) {
	RecordIds ids;
	const TableKind *kind = table.kind(record.kind);
	if (!kind || !record) return ids;
	ids.lists.resize(kind->lists().size());
	for (size_t l = 0; l < kind->lists().size(); ++l) {
		const ListOps &ops = kind->lists()[l].ops;
		const size_t size = ops.size(record);
		ids.lists[l].reserve(size);
		for (size_t i = 0; i < size; ++i) ids.lists[l].push_back(shape_ids(table, ops.at(record, i)));
	}
	return ids;
}

// The invariant every structural edit keeps: `ids` has the shape of the native record (each list as
// long, all the way down).
inline bool ids_match(const RecordTable &table, const RecordHandle &record, const RecordIds &ids) {
	const TableKind *kind = table.kind(record.kind);
	if (!kind || !record || ids.lists.size() != kind->lists().size()) return false;
	for (size_t l = 0; l < kind->lists().size(); ++l) {
		const ListOps &ops = kind->lists()[l].ops;
		const size_t size = ops.size(record);
		if (ids.lists[l].size() != size) return false;
		for (size_t i = 0; i < size; ++i)
			if (!ids_match(table, ops.at(record, i), ids.lists[l][i])) return false;
	}
	return true;
}

// --- list ops over the usual containers ----------------------------------------------------------
// A std::vector member of the owner's native struct (`list(owner)` returns it): records copied in and
// out as values, a new one `fresh(owner, index)` (the list's defaults where it goes; a value
// initialised record with none).
template <class Owner, class Record>
ListOps vector_list(NodeKind kind, std::vector<Record> &(*list)(Owner &owner),
                    Record (*fresh)(const Owner &owner, size_t index) = nullptr) {
	ListOps ops;
	ops.size = [list](const RecordHandle &owner) { return list(owner.as<Owner>()).size(); };
	ops.at = [list, kind](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<Owner>());
		return index < records.size() ? RecordHandle{kind, &records[index], owner.top} : RecordHandle{};
	};
	ops.insert = [list, kind, fresh](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                                 std::string &error) {
		std::vector<Record> &records = list(owner.as<Owner>());
		if (record && (!record->data || record->kind != kind)) {
			error = "This list takes records of its own kind only.";
			return false;
		}
		index = std::min(index, records.size());
		if (record) records.insert(records.begin() + std::ptrdiff_t(index), *static_cast<const Record *>(record->data.get()));
		else records.insert(records.begin() + std::ptrdiff_t(index), fresh ? fresh(owner.as<Owner>(), index) : Record{});
		return true;
	};
	ops.erase = [list](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<Owner>());
		if (index >= records.size()) return false;
		records.erase(records.begin() + std::ptrdiff_t(index));
		return true;
	};
	ops.copy = [list, kind](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		std::vector<Record> &records = list(owner.as<Owner>());
		if (index >= records.size()) return out;
		out.kind = kind;
		out.data = std::make_shared<Record>(records[index]);
		return out;
	};
	return ops;
}

} // namespace opennova::editor
