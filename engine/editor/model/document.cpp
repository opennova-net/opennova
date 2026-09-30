#include "document.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>

namespace opennova::editor {

uint64_t next_edit_gesture() {
	static std::atomic<uint64_t> next{0};
	return ++next;
}

Document::Document(const Document &other)
		: DocumentBase(other), rows_(other.rows_), file_state_(other.file_state_),
		  next_id_(other.next_id_), last_added_(other.last_added_), added_(other.added_),
		  history_(other.history_, rows_, file_state_), saved_rows_(other.saved_rows_),
		  saved_state_(other.saved_state_), saved_positions_(other.saved_positions_) {}

namespace {

bool fail(Diagnostic &error, const std::string &path, const char *code, const std::string &message,
          const std::string &field = {}) {
	error = make_diagnostic(DiagnosticSeverity::Error, code, message, path, field);
	return false;
}

// True for an edit that adds, removes or moves a whole row (or pastes rows at the top
// level): it changes the row list, not one row; and for an Apply naming no row, which changes
// the file-wide state alone.
bool row_level(const Edit &edit) {
	switch (edit.operation) {
	case EditOperation::Add:
	case EditOperation::Paste: return edit.address.row == 0 && edit.parent == 0;
	case EditOperation::Duplicate:
	case EditOperation::Remove:
	case EditOperation::Move: return edit.address.child == 0;
	case EditOperation::Apply: return edit.address.row == 0;
	default: return false;
	}
}

// True for an edit that may change what a row holds (an Apply's payload may too): a later edit
// of its batch finds its record by walking the clone.
bool structural(EditOperation operation) {
	return operation == EditOperation::Add || operation == EditOperation::Duplicate ||
	       operation == EditOperation::Remove || operation == EditOperation::Move ||
	       operation == EditOperation::Paste || operation == EditOperation::Apply;
}

// A kind as a message names it: its label, else its number (an address may name a kind the
// document does not hold).
std::string kind_words(const Document &document, NodeKind kind) {
	const char *label = document.kind_label(kind);
	return *label ? std::string(label) : "kind " + std::to_string(kind);
}

// The token a locator names a collection's kind by: its row's, else its number.
std::string locator_token(const Document &document, NodeKind kind) {
	const char *token = document.kind_token(kind);
	return *token ? std::string(token) : std::to_string(kind);
}

bool whole_number(const std::string &text, size_t &out) {
	if (text.empty() || text.size() > 18) return false;
	for (const char c : text)
		if (!std::isdigit(static_cast<unsigned char>(c))) return false;
	out = size_t(std::strtoull(text.c_str(), nullptr, 10));
	return true;
}

} // namespace

const RecordKindRow *Document::kind_row(NodeKind kind) const {
	for (const RecordKindRow &row : kinds())
		if (row.kind == kind) return &row;
	return nullptr;
}

const char *Document::kind_label(NodeKind kind) const {
	const RecordKindRow *row = kind_row(kind);
	return row ? row->label : "";
}

const char *Document::kind_token(NodeKind kind) const {
	const RecordKindRow *row = kind_row(kind);
	return row ? row->token : "";
}

NodeKind Document::kind_from_name(const std::string &token) const {
	for (const RecordKindRow &row : kinds())
		if (token == row.token) return row.kind;
	return -1;
}

size_t Document::row_index(NodeId id) const {
	const auto found = row_positions_.find(id);
	if (found != row_positions_.end() && found->second < rows_.size() && rows_[found->second]->id == id) return found->second;
	// Stale or unknown: every row indexed again.
	row_positions_.clear();
	for (size_t i = 0; i < rows_.size(); ++i) row_positions_[rows_[i]->id] = i;
	const auto again = row_positions_.find(id);
	return again == row_positions_.end() ? rows_.size() : again->second;
}

const Node *Document::row(NodeId id) const {
	const size_t index = row_index(id);
	return index < rows_.size() ? rows_[index].get() : nullptr;
}

void Document::walk_records(const Node &row, const RecordVisitor &visit) const {
	std::function<bool(const NodeAddress &)> descend = [&](const NodeAddress &owner) {
		for (const Collection &collection : collections(row, owner)) {
			for (size_t i = 0; i < collection.ids.size(); ++i) {
				const NodeAddress record{row.id, collection.spec.kind, collection.ids[i]};
				if (!visit(record, Placement{owner, collection.spec, i})) return false;
				if (!descend(record)) return false;
			}
		}
		return true;
	};
	descend({row.id, row.kind, 0});
}

const Document::RowIndex &Document::row_index_of(const std::shared_ptr<const Node> &row) const {
	const auto found = indexes_.find(row.get());
	if (found != indexes_.end()) return found->second;
	// Forget the indexes of rows an edit, an undo or a reload replaced, once they outnumber
	// the rows and the baseline's (an entry keeps its row alive, so an address is never
	// reused under it).
	if (indexes_.size() > rows_.size() + saved_rows_.size() + 8) {
		std::unordered_map<const Node *, RowIndex> kept;
		for (const auto *list : {&rows_, &saved_rows_})
			for (const auto &current : *list) {
				const auto index = indexes_.find(current.get());
				if (index != indexes_.end() && !kept.count(current.get())) kept.emplace(current.get(), std::move(index->second));
			}
		indexes_.swap(kept);
	}
	RowIndex index;
	index.row = row;
	walk_records(*row, [&](const NodeAddress &record, const Placement &at) {
		index.placements.emplace(record.child, at);
		return true;
	});
	return indexes_.emplace(row.get(), std::move(index)).first->second;
}

bool Document::placement_in(const Node &row, NodeId child, Placement &out) const {
	bool found = false;
	walk_records(row, [&](const NodeAddress &record, const Placement &at) {
		if (record.child != child) return true;
		out = at;
		found = true;
		return false;
	});
	return found;
}

void Document::index_records() const {
	if (records_known_ && records_revision_ == revision()) return;
	// A version's records out of the index, those still indexed under its row (one another row
	// has taken since keeps that row).
	const auto take_out = [&](NodeId row, const std::shared_ptr<const Node> &version) {
		for (const auto &record : row_index_of(version).placements) {
			const auto entry = record_rows_.find(record.first);
			if (entry != record_rows_.end() && entry->second == row) record_rows_.erase(entry);
		}
	};
	for (const auto &row : rows_) {
		std::shared_ptr<const Node> &indexed = indexed_rows_[row->id];
		if (indexed == row) continue;
		if (indexed) take_out(row->id, indexed);
		record_rows_[row->id] = row->id;
		for (const auto &record : row_index_of(row).placements)
			record_rows_[record.first] = row->id;
		indexed = row;
	}
	// A row no longer among the rows (one indexed that the loop above did not meet): its records
	// out, itself too.
	if (indexed_rows_.size() > rows_.size()) {
		std::unordered_set<NodeId> current;
		for (const auto &row : rows_) current.insert(row->id);
		for (auto it = indexed_rows_.begin(); it != indexed_rows_.end();) {
			if (current.count(it->first)) {
				++it;
				continue;
			}
			take_out(it->first, it->second);
			const auto self = record_rows_.find(it->first);
			if (self != record_rows_.end() && self->second == it->first) record_rows_.erase(self);
			it = indexed_rows_.erase(it);
		}
	}
	records_known_ = true;
	records_revision_ = revision();
}

NodeAddress Document::address_of(NodeId id) const {
	if (!id) return {};
	index_records();
	const auto owner = record_rows_.find(id);
	if (owner == record_rows_.end()) return {};
	const size_t index = row_index(owner->second);
	if (index >= rows_.size()) return {};
	const std::shared_ptr<const Node> &row = rows_[index];
	if (row->id == id) return {row->id, row->kind, 0};
	const RowIndex &records = row_index_of(row);
	const auto placed = records.placements.find(id);
	if (placed == records.placements.end()) return {};
	return {row->id, placed->second.spec.kind, id};
}

FieldUse Document::field_on(const NodeAddress &address, const FieldSchema &field) const {
	FieldUse use = field_use(field);
	refine_field(address, use);
	use.schema = &field;
	// A type never makes a read-only field writable.
	use.read_only = use.read_only || field.read_only;
	return use;
}

bool Document::record_choices(const NodeAddress &, const FieldUse &,
		std::vector<FieldChoice> &) const {
	return false;
}

const std::vector<FieldChoice> &Document::choices_on(const NodeAddress &address,
		const FieldUse &use, std::vector<FieldChoice> &own) const {
	own.clear();
	if (use.own_choices && record_choices(address, use, own)) return own;
	return use.schema->choices;
}

bool Document::get(const NodeAddress &address, const std::string &field, Value &out) const {
	const Node *node = row(address.row);
	return node && read(*node, address, field, out);
}

bool Document::present(const NodeAddress &address, const std::string &field) const {
	const Node *node = row(address.row);
	return node && read_present(*node, address, field);
}

std::vector<Document::Collection> Document::collections_of(const NodeAddress &owner) const {
	const Node *top = row(owner.row);
	if (!top) return {};
	return collections(*top, owner.child ? owner : NodeAddress{top->id, top->kind, 0});
}

bool Document::placement(const NodeAddress &address, Placement &out) const {
	if (!address.child) return false;
	const size_t index = row_index(address.row);
	if (index == rows_.size()) return false;
	const RowIndex &records = row_index_of(rows_[index]);
	const auto found = records.placements.find(address.child);
	if (found == records.placements.end()) return false;
	out = found->second;
	return true;
}

std::vector<NodeAddress> Document::ancestors(const NodeAddress &address) const {
	std::vector<NodeAddress> chain;
	Placement at;
	NodeAddress current = address;
	while (current.child && placement(current, at)) {
		chain.push_back(at.owner);
		current = at.owner;
	}
	if (current.child) return {}; // an unknown record
	std::reverse(chain.begin(), chain.end());
	return chain;
}

std::vector<NodeAddress> Document::outermost(const std::vector<NodeAddress> &records) const {
	std::vector<NodeAddress> out;
	for (const NodeAddress &record : records) {
		bool held = false;
		for (const NodeAddress &owner : ancestors(record))
			held = held || std::find(records.begin(), records.end(), owner) != records.end();
		if (!held && std::find(out.begin(), out.end(), record) == out.end()) out.push_back(record);
	}
	return out;
}

std::string Document::record_name(const NodeAddress &address) const {
	const Node *top = row(address.row);
	if (!top) return std::string();
	if (!address.child) return top->name();
	Placement at;
	if (!placement(address, at)) return std::string();
	Value name;
	if (*at.spec.name_field && get(address, at.spec.name_field, name)) {
		if (const auto *text = std::get_if<std::string>(&name); text && !text->empty()) return *text;
	}
	return std::string(kind_label(at.spec.kind)) + " " + std::to_string(at.index + 1);
}

std::string Document::record_path(const NodeAddress &address) const {
	if (!row(address.row)) return std::string();
	std::string path;
	for (const NodeAddress &owner : ancestors(address)) path += record_name(owner) + "/";
	return path + record_name(address);
}

std::string Document::locator(const NodeAddress &address) const {
	const size_t index = row_index(address.row);
	if (index == rows_.size()) return std::string();
	std::string out = std::to_string(index);
	if (!address.child) return out;
	std::vector<NodeAddress> chain = ancestors(address);
	if (chain.empty()) return std::string();
	chain.push_back(address);
	Placement at;
	for (size_t i = 1; i < chain.size(); ++i) {
		if (!placement(chain[i], at)) return std::string();
		out += "/" + locator_token(*this, at.spec.kind) + ":" + std::to_string(at.index);
	}
	return out;
}

NodeAddress Document::address_at(const std::string &text) const { return address_in(rows_, text); }

NodeAddress Document::source_address(const std::string &text) const {
	const NodeAddress saved = address_in(saved_rows_, text);
	return saved.row && holds(current_row(saved.row), saved) ? saved : NodeAddress();
}

NodeAddress Document::address_in(const std::vector<std::shared_ptr<const Node>> &rows, const std::string &text) const {
	std::vector<std::string> parts;
	size_t start = 0;
	for (;;) {
		const size_t slash = text.find('/', start);
		parts.push_back(text.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	size_t index = 0;
	if (!whole_number(parts[0], index) || index >= rows.size()) return {};
	const Node &top = *rows[index];
	NodeAddress current{top.id, top.kind, 0};
	for (size_t i = 1; i < parts.size(); ++i) {
		const size_t colon = parts[i].rfind(':');
		size_t position = 0;
		if (colon == std::string::npos || !whole_number(parts[i].substr(colon + 1), position)) return {};
		const std::string token = parts[i].substr(0, colon);
		bool found = false;
		for (const Collection &collection : collections(top, current)) {
			if (locator_token(*this, collection.spec.kind) != token) continue;
			if (position >= collection.ids.size()) continue;
			current = {top.id, collection.spec.kind, collection.ids[position]};
			found = true;
			break;
		}
		if (!found) return {};
	}
	return current;
}

void Document::assign_ids(Node &row) {
	row.for_each_identity([this](NodeId &id) { id = allocate_id(); });
}

std::shared_ptr<Node> Document::make_node(NodeKind, NodeId, std::string &error) {
	error = "This document cannot add that record.";
	return nullptr;
}

bool Document::set_present(Node &, const NodeAddress &, const std::string &, bool, std::string &error) {
	error = "This field is always written.";
	return false;
}

bool Document::edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &,
                               std::string &error) {
	error = "This collection cannot accept that edit.";
	return false;
}

bool Document::apply_payload(Node &, const NodeAddress &, const EditPayload &,
                             std::shared_ptr<const FileState> &, const IdAllocator &, bool &,
                             std::string &error) {
	error = "This document does not take that change.";
	return false;
}

bool Document::apply_file_payload(std::shared_ptr<const FileState> &, const EditPayload &, bool &,
                                  std::string &error) {
	error = "This document does not take that change.";
	return false;
}

bool Document::paste_records(Node &, const Edit &, const IdAllocator &, std::vector<NodeId> &, std::string &error) {
	error = "This document cannot paste records.";
	return false;
}

bool Document::set_file_value(std::shared_ptr<const FileState> &, const Edit &, Diagnostic &error) {
	return fail(error, path(), "document.value", "This document has no file-wide values.");
}

bool Document::read_source(const std::vector<uint8_t> &decoded, bool adopt,
                           std::vector<SourceIssue> &issues, Diagnostic &error) {
	std::vector<std::shared_ptr<Node>> rows;
	std::shared_ptr<const FileState> state;
	// The C parsers permit an empty file, but require a non-null input pointer.
	const std::vector<uint8_t> one_byte(decoded.empty() ? 1 : 0, 0);
	if (!parse(decoded.empty() ? one_byte : decoded, rows, state, issues, error)) return false;
	if (!adopt) return true;
	rows_.clear(); file_state_.reset();
	indexes_.clear();
	history_.reset();
	next_id_ = 1; last_added_ = 0; added_.clear();
	for (auto &row : rows) {
		row->id = allocate_id();
		assign_ids(*row);
		rows_.push_back(row);
	}
	file_state_ = state;
	set_baseline();
	return true;
}

void Document::on_saved() {
	history_.mark_saved();
	history_.end_edit_group();
	set_baseline();
}

// A whole row added, duplicated, removed or moved (or a file-wide value set): one edit,
// one step, never folded. An Apply naming no row, the file-wide state alone: one step, a
// gesture's folding into one.
bool Document::apply_row_edit(const Edit &edit, Diagnostic &error) {
	Change change;
	change.before_state = change.after_state = file_state_;
	if (edit.operation == EditOperation::SetFileValue) {
		std::shared_ptr<const FileState> updated = file_state_;
		if (!set_file_value(updated, edit, error)) return false;
		change.after_state = updated;
		return commit(std::move(change), {}, error);
	}
	std::string message;
	if (edit.operation == EditOperation::Apply) {
		if (!edit.payload)
			return fail(error, path(), "document.payload", "This change carries nothing to apply.");
		std::shared_ptr<const FileState> updated = file_state_;
		bool changes = true;
		if (!apply_file_payload(updated, *edit.payload, changes, message))
			return fail(error, path(), "document.payload",
			            message.empty() ? "This document does not take that change." : message);
		if (!changes) return true; // the file-wide state as it was: no step
		change.after_state = updated;
		// A gesture's changes fold into one step, keyed by the file rather than a row.
		const std::string key =
		        edit.gesture ? "g" + std::to_string(edit.gesture) + "/file" : std::string();
		return commit(std::move(change), key, error);
	}
	std::shared_ptr<Node> updated;
	if (edit.operation == EditOperation::Paste)
		return fail(error, path(), "document.paste", "Paste inside a record: select where the records go.");
	if (edit.operation == EditOperation::Add) {
		const NodeId id = allocate_id();
		updated = make_node(edit.address.kind, id, message);
		if (!updated) return fail(error, path(), "document.kind", message.empty() ? "This document cannot add that record." : message);
		updated->id = id;
		assign_ids(*updated);
		// The new row's field, set in the same step (Edit::field on an Add).
		if (!edit.field.empty()) {
			if (!set_field(*updated, {id, edit.address.kind, 0}, edit.field, edit.value, message))
				return fail(error, path(), "document.value", message.empty() ? "Unknown field." : message, edit.field);
			after_edit(*updated);
		}
		change.after = updated;
		change.after_position = std::min(edit.position, rows_.size());
		if (!commit(std::move(change), {}, error)) return false;
		last_added_ = id;
		added_ = {id};
		return true;
	}
	const size_t index = row_index(edit.address.row);
	if (index == rows_.size()) return fail(error, path(), "document.selection", "The selected record no longer exists.");
	if (rows_[index]->kind != edit.address.kind)
		return fail(error, path(), "document.selection",
		            std::string("Wrong kind: the record is ") + kind_label(rows_[index]->kind) + ", the address says " + kind_words(*this, edit.address.kind) + ".");
	if (edit.parent) return fail(error, path(), "document.collection", "A row moves among the rows only.");
	change.before = rows_[index];
	change.before_position = index;
	if (edit.operation == EditOperation::Remove) {
		change.after_state = state_after_remove(file_state_, rows_.size() - 1);
		change.after_position = index;
	} else if (edit.operation == EditOperation::Duplicate) {
		updated = rows_[index]->clone();
		updated->id = allocate_id();
		assign_ids(*updated);
		prepare_duplicate(*updated);
		change.before.reset();
		change.after_position = std::min(edit.position, rows_.size());
	} else {
		change.after_position = std::min(edit.position, rows_.size() - 1);
		// A Move that leaves the row where it is changes nothing: no history step, so the
		// document stays clean.
		if (change.after_position == index) return true;
		updated = rows_[index]->clone();
	}
	if (updated) {
		after_edit(*updated);
		change.after = updated;
	}
	const NodeId made = edit.operation == EditOperation::Duplicate ? updated->id : 0;
	if (!commit(std::move(change), {}, error)) return false;
	if (made) {
		last_added_ = made;
		added_ = {made};
	}
	return true;
}

bool Document::commit(Change change, const std::string &key, Diagnostic &error) {
	std::string message;
	if (!accept_change(change, message))
		return fail(error, path(), "document.structure", message.empty() ? "This document refuses that change." : message);
	history_.commit(std::move(change), key);
	return true;
}

bool Document::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	if (edits.empty()) return true;
	for (const Edit &edit : edits) {
		if (edit.operation != EditOperation::SetFileValue && !row_level(edit)) continue;
		if (edits.size() == 1) return apply_row_edit(edit, error);
		return fail(error, path(), "document.batch", "Rows and file-wide values change one edit at a time, not in a batch.");
	}
	NodeId row = 0;
	if (!batch_row(edits, row, error)) return false;
	bool builds = false;
	const std::string key = step_key(edits, row, builds);
	// A coalesced batch (typing) applies to the row as its group found it, so the group's one
	// step is that row plus the latest value: an empty value typed on the way to a new one (an
	// image or a hotkey cleared and retyped, a name) cannot drop what the new value keeps, and a
	// group that ends empty is the clear. A gesture's edits build on each other instead.
	const bool reopened = !builds && history_.reopen(key);
	const auto refused = [&]() {
		if (reopened) history_.resume(); // the group's step as it was
		return false;
	};
	RowBatch batch;
	if (!prepare_row_batch(edits, batch, error)) return refused();
	if (!batch.changed) {
		// Nothing changes. A reopened group typed back to the values it found is no step at all:
		// the document stays as the group found it (clean again when that was saved).
		if (reopened) history_.drop();
		return true;
	}
	// The change is the type's to refuse before it commits.
	std::string message;
	if (!accept_change(batch.change, message)) {
		fail(error, path(), "document.structure",
		     message.empty() ? "This document refuses that change." : message);
		return refused();
	}
	history_.commit(std::move(batch.change), key);
	if (!batch.added.empty()) {
		added_ = batch.added;
		last_added_ = batch.added.front();
	}
	return true;
}

bool Document::batch_row(const std::vector<Edit> &edits, NodeId &row, Diagnostic &error) const {
	// Every edit changes one row: the row an Add or a Paste goes into is its owner's.
	row = 0;
	for (const Edit &edit : edits) {
		NodeId id = edit.address.row;
		const bool into = edit.operation == EditOperation::Add || edit.operation == EditOperation::Paste;
		// A record the batch makes is in the batch's row: its edit names that row, or none.
		if (is_batch_made(edit.address.child) || (into && is_batch_made(edit.parent))) {
			if (!id) continue;
		} else if (into && edit.parent) {
			const NodeAddress owner = address_of(edit.parent);
			if (!owner.row) return fail(error, path(), "document.selection", "The record to add into no longer exists.");
			if (edit.address.row && edit.address.row != owner.row)
				return fail(error, path(), "document.selection", "The record to add into is in another row than the edit names.");
			id = owner.row;
		}
		if (row && id != row)
			return fail(error, path(), "document.batch", "A batch edits one row: edit other rows in another change.");
		row = id;
	}
	return true;
}

std::string Document::step_key(const std::vector<Edit> &edits, NodeId row, bool &builds) {
	const uint64_t gesture = edits.front().gesture;
	bool same_gesture = gesture != 0, all_coalesce = true;
	for (const Edit &edit : edits) {
		same_gesture = same_gesture && edit.gesture == gesture;
		all_coalesce = all_coalesce && edit.coalesce && edit.operation == EditOperation::Set;
	}
	builds = same_gesture;
	if (same_gesture) return "g" + std::to_string(gesture) + "/r" + std::to_string(row);
	std::string key;
	if (all_coalesce)
		for (const Edit &edit : edits)
			key += (key.empty() ? "" : "|") + std::to_string(edit.address.row) + "/" + std::to_string(edit.address.child) + "/" + edit.field;
	return key;
}

bool Document::prepare_row_batch(const std::vector<Edit> &edits, RowBatch &out, Diagnostic &error) {
	NodeId row_id = 0;
	if (!batch_row(edits, row_id, error)) return false;
	const size_t index = row_index(row_id);
	if (index == rows_.size()) return fail(error, path(), "document.selection", "The selected record no longer exists.");
	const std::shared_ptr<const Node> current = rows_[index];
	std::shared_ptr<Node> updated = current->clone();

	const IdAllocator allocate = [this] { return allocate_id(); };
	std::vector<NodeId> added;
	bool changed = false, reshaped = false;
	// The file-wide state as the batch leaves it (an Apply's payload may change it too).
	std::shared_ptr<const FileState> state = file_state_;
	const std::string row_label = kind_label(current->kind);
	// A nested record's placement: the committed row's index until an edit of this batch
	// changes the row's shape, then a walk of the clone.
	auto place = [&](NodeId child, Placement &at) {
		return reshaped ? placement_in(*updated, child, at) : placement({row_id, 0, child}, at);
	};
	// The owner a record goes into: a record of this row, or the row itself (0).
	auto owner_of = [&](NodeId parent, NodeAddress &owner) {
		if (!parent || parent == row_id) { owner = {row_id, updated->kind, 0}; return true; }
		Placement at;
		if (!place(parent, at)) return false;
		owner = {row_id, at.spec.kind, parent};
		return true;
	};
	auto find_collection = [&](const NodeAddress &owner, NodeKind kind, Collection &out) {
		for (const Collection &collection : collections(*updated, owner))
			if (collection.spec.kind == kind) { out = collection; return true; }
		return false;
	};

	// The record each edit made, which a later edit names by batch_made(its index).
	std::vector<NodeId> made(edits.size(), 0);
	for (size_t i = 0; i < edits.size(); ++i) {
		Edit edit = edits[i];
		const auto resolve_made = [&](NodeId &id) {
			if (!is_batch_made(id)) return true;
			const NodeId earlier = id - kBatchMadeBase;
			if (earlier >= NodeId(i) || !made[size_t(earlier)]) return false;
			id = made[size_t(earlier)];
			return true;
		};
		if (!resolve_made(edit.address.child) || !resolve_made(edit.parent))
			return fail(error, path(), "document.batch", "An edit names a record that no earlier edit of its batch made.");
		if (!edit.address.row) edit.address.row = row_id;
		std::string message;
		const NodeAddress &address = edit.address;
		Edit hook = edit;
		hook.address.row = row_id;
		Placement at;
		// Resolve the address: every edit but an Add or a Paste names an existing record of
		// the kind it says.
		if (edit.operation != EditOperation::Add && edit.operation != EditOperation::Paste) {
			if (!address.child) {
				if (updated->kind != address.kind)
					return fail(error, path(), "document.selection",
					            std::string("Wrong kind: the record is ") + row_label + ", the address says " + kind_words(*this, address.kind) + ".");
			} else if (!place(address.child, at)) {
				return fail(error, path(), "document.selection", "The selected record no longer exists.");
			} else if (at.spec.kind != address.kind) {
				return fail(error, path(), "document.selection",
				            std::string("Wrong kind: the record is ") + kind_label(at.spec.kind) + ", the address says " + kind_words(*this, address.kind) + ".");
			}
		}
		switch (edit.operation) {
		case EditOperation::Set: {
			// A Set of the value the field holds changes nothing (as the record reads it, a def's
			// number in its written units included, and whether an optional field is written): no
			// history step, as for a Clear, a Write or a Move that leaves the record as it is.
			Value before, after;
			const bool read_before = read(*updated, address, edit.field, before);
			const bool written_before = read_present(*updated, address, edit.field);
			if (!set_field(*updated, address, edit.field, edit.value, message))
				return fail(error, path(), "document.value", message.empty() ? "Unknown field." : message, edit.field);
			if (read_before && read(*updated, address, edit.field, after) && after == before &&
			    read_present(*updated, address, edit.field) == written_before)
				continue;
			break;
		}
		case EditOperation::Apply: {
			// A change the type made in C++, to the record and the file-wide state as the batch
			// has left them; one that changes nothing is no step, as a Set of the value held.
			if (!edit.payload)
				return fail(error, path(), "document.payload",
				            "This change carries nothing to apply.");
			bool changes = true;
			if (!apply_payload(*updated, address, *edit.payload, state, allocate, changes, message))
				return fail(error, path(), "document.payload",
				            message.empty() ? "This document does not take that change." : message);
			if (!changes) continue;
			break;
		}
		case EditOperation::Clear:
		case EditOperation::Write: {
			const FieldSchema *schema = field_schema(address.kind, edit.field);
			if (!schema) return fail(error, path(), "document.value", "Unknown field.", edit.field);
			if (!schema->optional || schema->read_only)
				return fail(error, path(), "document.value", "This field is always written.", edit.field);
			const bool written = edit.operation == EditOperation::Write;
			// A field already left out (Clear) or already written (Write) changes nothing. The
			// committed row answers while no earlier edit of the batch has changed the clone.
			if (!changed && present(address, edit.field) == written) continue;
			if (!set_present(*updated, address, edit.field, written, message))
				return fail(error, path(), "document.value", message.empty() ? "This field is always written." : message, edit.field);
			break;
		}
		case EditOperation::Add:
		case EditOperation::Paste: {
			NodeAddress owner;
			if (!owner_of(edit.parent, owner))
				return fail(error, path(), "document.selection", "The record to add into no longer exists.");
			hook.parent = owner.child;
			if (edit.operation == EditOperation::Paste) {
				std::vector<NodeId> pasted;
				if (!paste_records(*updated, hook, allocate, pasted, message))
					return fail(error, path(), "document.paste", message.empty() ? "These records cannot be pasted here." : message);
				added.insert(added.end(), pasted.begin(), pasted.end());
				if (!pasted.empty()) made[i] = pasted.front();
				break;
			}
			Collection collection;
			if (!find_collection(owner, address.kind, collection))
				return fail(error, path(), "document.collection",
				            std::string(kind_label(owner.kind)) + " records hold no " + kind_words(*this, address.kind) + " records.");
			if (collection.spec.fixed)
				return fail(error, path(), "document.collection", std::string("The ") + collection.spec.label + " of this " +
				            kind_label(owner.kind) + " is fixed: nothing is added to it.");
			NodeId one = 0;
			if (!edit_collection(*updated, hook, allocate, one, message))
				return fail(error, path(), "document.collection", message.empty() ? "This collection cannot accept that edit." : message);
			if (one) added.push_back(one);
			made[i] = one;
			// The new record's field, set in the same step (Edit::field on an Add).
			if (!edit.field.empty() && (!one || !set_field(*updated, {row_id, address.kind, one}, edit.field, edit.value, message)))
				return fail(error, path(), "document.value", message.empty() ? "Unknown field." : message, edit.field);
			break;
		}
		case EditOperation::Duplicate:
		case EditOperation::Remove:
		case EditOperation::Move: {
			if (!address.child) return fail(error, path(), "document.collection", "A row is duplicated, removed or moved on its own.");
			if (at.spec.fixed)
				return fail(error, path(), "document.collection", std::string("The ") + at.spec.label + " of this " +
				            kind_label(at.owner.kind) + " is fixed: it stays where it is.");
			hook.parent = at.owner.child;
			if (edit.operation == EditOperation::Move) {
				NodeAddress destination = at.owner;
				if (edit.parent && !owner_of(edit.parent, destination)) {
					const NodeAddress elsewhere = address_of(edit.parent);
					return fail(error, path(), "document.collection",
					            elsewhere.row && elsewhere.row != row_id ? "A record moves within its own " + row_label + "."
					                                                     : std::string("The destination no longer exists."));
				}
				Collection collection;
				if (!find_collection(destination, address.kind, collection))
					return fail(error, path(), "document.collection",
					            std::string(kind_label(destination.kind)) + " records hold no " + kind_words(*this, address.kind) + " records.");
				if (collection.spec.fixed)
					return fail(error, path(), "document.collection", std::string("The ") + collection.spec.label + " of this " +
					            kind_label(destination.kind) + " is fixed: nothing moves into it.");
				// Never into the record itself or anything it holds.
				for (NodeAddress up = destination; up.child;) {
					if (up.child == address.child)
						return fail(error, path(), "document.collection", "A record cannot move inside itself.");
					Placement above;
					if (!place(up.child, above)) break;
					up = above.owner;
				}
				// A Move that leaves the record where it is changes nothing (B5).
				if (destination == at.owner && !collection.ids.empty() &&
				    std::min(edit.position, collection.ids.size() - 1) == at.index)
					continue;
				hook.parent = destination.child;
			}
			NodeId one = 0;
			if (!edit_collection(*updated, hook, allocate, one, message))
				return fail(error, path(), "document.collection", message.empty() ? "This collection cannot accept that edit." : message);
			if (one) added.push_back(one);
			made[i] = one;
			break;
		}
		default:
			return fail(error, path(), "document.batch", "Rows and file-wide values change one edit at a time, not in a batch.");
		}
		changed = true;
		reshaped = reshaped || structural(edit.operation);
	}
	out.changed = changed;
	if (!changed) return true;
	after_edit(*updated);
	out.change.before_state = file_state_;
	out.change.after_state = state;
	out.change.before = current;
	out.change.before_position = out.change.after_position = index;
	out.change.after = updated;
	out.added = std::move(added);
	return true;
}

// --- the saved baseline ------------------------------------------------------------------

void Document::set_baseline() {
	saved_rows_ = rows_;
	saved_state_ = file_state_;
	saved_positions_.clear();
	for (size_t i = 0; i < saved_rows_.size(); ++i) saved_positions_[saved_rows_[i]->id] = i;
	// Every memo made against the old baseline, or a load's rows, is forgotten (a load starts the
	// revisions again, so one may name another state).
	row_changes_.clear();
	moved_known_ = false;
	records_known_ = false;
	record_rows_.clear();
	indexed_rows_.clear();
}

std::shared_ptr<const Node> Document::current_row(NodeId id) const {
	const size_t index = row_index(id);
	return index < rows_.size() ? rows_[index] : nullptr;
}

std::shared_ptr<const Node> Document::saved_row(NodeId id) const {
	const auto found = saved_positions_.find(id);
	return found == saved_positions_.end() ? nullptr : saved_rows_[found->second];
}

bool Document::holds(const std::shared_ptr<const Node> &row, const NodeAddress &address) const {
	if (!row) return false;
	if (!address.child) return row->kind == address.kind;
	const RowIndex &index = row_index_of(row);
	const auto found = index.placements.find(address.child);
	return found != index.placements.end() && found->second.spec.kind == address.kind;
}

const FieldSchema *Document::field_schema(NodeKind kind, const std::string &id) const {
	for (const FieldSchema &field : fields(kind))
		if (field.id == id) return &field;
	return nullptr;
}

bool Document::field_differs(const NodeAddress &address, const FieldSchema &field, const std::shared_ptr<const Node> &now,
                             const std::shared_ptr<const Node> &saved) const {
	Value current, before;
	const bool reads_now = holds(now, address) && read(*now, address, field.id, current);
	const bool reads_saved = holds(saved, address) && read(*saved, address, field.id, before);
	if (reads_now != reads_saved) return true;
	if (!reads_now) return false;
	if (!same_value(current, before)) return true;
	return field.optional && read_present(*now, address, field.id) != read_present(*saved, address, field.id);
}

Document::RowChanges &Document::row_changes(NodeId row, const std::shared_ptr<const Node> &now,
                                            const std::shared_ptr<const Node> &saved) const {
	RowChanges &changes = row_changes_[row];
	if (changes.now != now || changes.saved != saved) changes = RowChanges{now, saved, {}, {}};
	return changes;
}

bool Document::field_changed(const NodeAddress &address, const std::string &field) const {
	const std::shared_ptr<const Node> now = current_row(address.row);
	const std::shared_ptr<const Node> saved = saved_row(address.row);
	// The baseline's own committed row: nothing in it changed.
	if (now == saved) return false;
	const NodeId record = address.child ? address.child : address.row;
	std::unordered_map<std::string, bool> &fields = row_changes(address.row, now, saved).fields[record];
	const auto cached = fields.find(field);
	if (cached != fields.end()) return cached->second;
	const FieldSchema *schema = field_schema(address.kind, field);
	const bool changed = schema && field_differs(address, *schema, now, saved);
	fields.emplace(field, changed);
	return changed;
}

Document::RecordChange Document::record_change(const NodeAddress &address) const {
	const std::shared_ptr<const Node> now = current_row(address.row);
	const std::shared_ptr<const Node> saved = saved_row(address.row);
	if (!now) return RecordChange::Unchanged; // gone: nothing left to mark
	// What the record itself holds against the baseline: nothing changed in the baseline's own
	// committed row; else the two rows compared, the answer kept while they stand.
	RecordChange change = RecordChange::Unchanged;
	if (now != saved) {
		std::unordered_map<NodeId, RecordChange> &records =
				row_changes(address.row, now, saved).records;
		const NodeId key = address.child ? address.child : address.row;
		auto cached = records.find(key);
		if (cached == records.end())
			cached = records.emplace(key, compare_record(address, now, saved)).first;
		change = cached->second;
	}
	// A row's place among the rows depends on every row, not on its own two: asked on each call
	// (row_moved answers once a revision), never kept with the row's own answer.
	if (change == RecordChange::Unchanged && !address.child && now->kind == address.kind &&
			row_moved(address.row))
		change = RecordChange::Changed;
	return change;
}

Document::RecordChange Document::compare_record(const NodeAddress &address,
		const std::shared_ptr<const Node> &now, const std::shared_ptr<const Node> &saved) const {
	if (!holds(now, address)) return RecordChange::Unchanged; // gone: nothing left to mark
	if (!holds(saved, address)) return RecordChange::Added;
	for (const FieldSchema &field : fields(address.kind))
		if (field_differs(address, field, now, saved)) return RecordChange::Changed;
	// What it holds: the same records in the same order, collection by collection.
	const NodeAddress owner = address.child ? address : NodeAddress{now->id, now->kind, 0};
	const std::vector<Collection> after = collections(*now, owner), before = collections(*saved, owner);
	if (after.size() != before.size()) return RecordChange::Changed;
	for (size_t i = 0; i < after.size(); ++i)
		if (after[i].spec.kind != before[i].spec.kind || after[i].ids != before[i].ids) return RecordChange::Changed;
	return RecordChange::Unchanged;
}

bool Document::row_moved(NodeId id) const {
	if (!moved_known_ || moved_revision_ != revision()) {
		moved_known_ = true;
		moved_revision_ = revision();
		moved_rows_.clear();
		// The rows both sides have, in each side's order: a row added or removed moves no other,
		// a reorder moves each row whose index there differs.
		std::unordered_set<NodeId> now_ids;
		for (const auto &row : rows_) now_ids.insert(row->id);
		std::vector<NodeId> before, after;
		for (const auto &row : saved_rows_)
			if (now_ids.count(row->id)) before.push_back(row->id);
		for (const auto &row : rows_)
			if (saved_positions_.count(row->id)) after.push_back(row->id);
		for (size_t i = 0; i < after.size() && i < before.size(); ++i)
			if (after[i] != before[i]) moved_rows_.insert(after[i]);
	}
	return moved_rows_.count(id) != 0;
}

bool Document::file_state_changed() const { return !same_file_state(file_state_.get(), saved_state_.get()); }

bool Document::saved_value(const NodeAddress &address, const std::string &field, Value &out, bool *present) const {
	const std::shared_ptr<const Node> saved = saved_row(address.row);
	if (!holds(saved, address) || !read(*saved, address, field, out)) return false;
	if (present) {
		const FieldSchema *schema = field_schema(address.kind, field);
		*present = !schema || !schema->optional || read_present(*saved, address, field);
	}
	return true;
}

std::vector<Edit> Document::revert_edits(const NodeAddress &address, const std::string &field) const {
	const FieldSchema *schema = field_schema(address.kind, field);
	const std::shared_ptr<const Node> now = current_row(address.row), saved = saved_row(address.row);
	Value current, before;
	if (!schema || schema->read_only || !holds(now, address) || !holds(saved, address) ||
	    !read(*now, address, field, current) || !read(*saved, address, field, before) ||
	    !field_differs(address, *schema, now, saved))
		return {};
	const auto edit = [&](EditOperation operation) {
		Edit out;
		out.operation = operation;
		out.address = address;
		out.field = field;
		if (operation == EditOperation::Set) out.value = before;
		return out;
	};
	std::vector<Edit> edits;
	const bool value_differs = !same_value(current, before);
	if (value_differs) edits.push_back(edit(EditOperation::Set));
	if (schema->optional) {
		// A Set of another value writes an optional field (edit.h): after it the field is
		// written whatever it was.
		const bool was = read_present(*saved, address, field);
		const bool will_be = value_differs || read_present(*now, address, field);
		if (was != will_be) edits.push_back(edit(was ? EditOperation::Write : EditOperation::Clear));
	}
	return edits;
}

} // namespace opennova::editor
