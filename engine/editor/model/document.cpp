#include "document.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>

#include <base/io/strutil.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/staged_rows.h>

namespace opennova::editor {

uint64_t next_edit_gesture() {
	static std::atomic<uint64_t> next{0};
	return ++next;
}

Document::Document(const Document &other)
		: DocumentBase(other), rows_(other.rows_), file_state_(other.file_state_),
		  next_id_(other.next_id_), last_added_(other.last_added_), added_(other.added_),
		  made_(other.made_), found_(other.found_),
		  history_(other.history_, rows_, file_state_), saved_rows_(other.saved_rows_),
		  saved_state_(other.saved_state_), saved_positions_(other.saved_positions_) {}

namespace {

bool fail(Diagnostic &error, const std::string &path, CoreFinding code, const std::string &message,
          const std::string &field = {}) {
	error = make_finding(code, DiagnosticSeverity::Error, message, path, field);
	return false;
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

// A locator's index: decimal digits alone (strutil's parse, never a sign, a blank or a tail), within
// what a size holds.
bool whole_number(const std::string &text, size_t &out) {
	if (!strutil::all_digits(text)) return false;
	const std::optional<unsigned long long> value = strutil::parse_ullong(text);
	if (!value || *value > static_cast<unsigned long long>(SIZE_MAX)) return false;
	out = static_cast<size_t>(*value);
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
		const std::vector<Collection> held = collections(row, owner);
		for (size_t c = 0; c < held.size(); ++c) {
			const Collection &collection = held[c];
			for (size_t i = 0; i < collection.ids.size(); ++i) {
				const NodeAddress record{row.id, collection.kind_at(i), collection.ids[i]};
				CollectionSpec spec = collection.spec;
				spec.kind = record.kind; // a list of several kinds places each record as the kind it is
				if (!visit(record, Placement{owner, spec, i, c})) return false;
				if (!descend(record)) return false;
			}
		}
		return true;
	};
	descend({row.id, row.kind, 0});
}

Document::RecordPath Document::RowIndex::path(NodeId record) const {
	const auto found = records.find(record);
	if (found == records.end()) return {};
	return {steps.data() + found->second.path, found->second.depth};
}

Document::RowIndex Document::make_index(const Node &row) const {
	RowIndex index;
	const std::vector<TargetedCollection> &targets = targeted_collections();
	index.targeted.resize(targets.size());
	// A walk meets an owner before what it holds: a record's path is its owner's, then its own step.
	walk_records(row, [&](const NodeAddress &record, const Placement &at) {
		for (size_t t = 0; t < targets.size(); ++t)
			if (targets[t].kind == record.kind) index.targeted[t].push_back(record.child);
		RowIndex::Entry entry;
		entry.at = at;
		entry.path = uint32_t(index.steps.size());
		if (at.owner.child) {
			const auto owner = index.records.find(at.owner.child);
			if (owner != index.records.end()) {
				const RowIndex::Entry held = owner->second;
				for (uint32_t s = 0; s < held.depth; ++s) {
					const PathStep step = index.steps[held.path + s];
					index.steps.push_back(step);
				}
				entry.depth = held.depth;
			}
		}
		index.steps.push_back({uint32_t(at.collection), uint32_t(at.index)});
		++entry.depth;
		index.records.emplace(record.child, entry);
		return true;
	});
	return index;
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
	RowIndex index = make_index(*row);
	index.row = row;
	return indexes_.emplace(row.get(), std::move(index)).first->second;
}

const Document::RowIndex *Document::index_for(const Node &row) const {
	// A row indexed before: an entry keeps its row alive, so its address names that row.
	const auto indexed = indexes_.find(&row);
	if (indexed != indexes_.end()) return &indexed->second;
	// The batch's own version of a row: one an edit of the batch changed the shape of (or made) has
	// an index of its own, made by walking it as it is; any other has its committed row's shape.
	if (staged_ && staged_->find(row.id) == &row) {
		if (staged_->reshaped(row.id)) {
			auto own = staged_indexes_.find(row.id);
			if (own == staged_indexes_.end()) own = staged_indexes_.emplace(row.id, make_index(row)).first;
			return &own->second;
		}
		const std::shared_ptr<const Node> committed = current_row(row.id);
		return committed ? &row_index_of(committed) : nullptr;
	}
	if (const std::shared_ptr<const Node> now = current_row(row.id); now.get() == &row)
		return &row_index_of(now);
	if (const std::shared_ptr<const Node> saved = saved_row(row.id); saved.get() == &row)
		return &row_index_of(saved);
	return nullptr;
}

Document::RecordPath Document::path_in(const Node &row, NodeId record) const {
	if (!record) return {};
	if (const RowIndex *index = index_for(row)) return index->path(record);
	walked_ = make_index(row);
	return walked_.path(record);
}

bool Document::placement_in(const Node &row, NodeId child, Placement &out) const {
	const RowIndex *index = index_for(row);
	if (!index) {
		walked_ = make_index(row);
		index = &walked_;
	}
	const auto found = index->records.find(child);
	if (found == index->records.end()) return false;
	out = found->second.at;
	return true;
}

void Document::index_records() const {
	if (records_known_ && records_revision_ == revision()) return;
	// A version's records out of the index, those still indexed under its row (one another row
	// has taken since keeps that row).
	const auto take_out = [&](NodeId row, const std::shared_ptr<const Node> &version) {
		for (const auto &record : row_index_of(version).records) {
			const auto entry = record_rows_.find(record.first);
			if (entry != record_rows_.end() && entry->second == row) record_rows_.erase(entry);
		}
	};
	for (const auto &row : rows_) {
		std::shared_ptr<const Node> &indexed = indexed_rows_[row->id];
		if (indexed == row) continue;
		if (indexed) take_out(row->id, indexed);
		record_rows_[row->id] = row->id;
		for (const auto &record : row_index_of(row).records)
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
	const RowIndex &indexed = row_index_of(row);
	const auto placed = indexed.records.find(id);
	if (placed == indexed.records.end()) return {};
	return {row->id, placed->second.at.spec.kind, id};
}

FieldUse Document::field_on(const NodeAddress &address, const FieldSchema &field) const {
	FieldUse use = field_use(field);
	refine_field(address, use);
	use.schema = &field;
	// A type never makes a read-only field writable.
	use.read_only = use.read_only || field.read_only;
	// A record of this file by its index, counted across the file, resolves in this file (S13 D8):
	// its scope is the file.
	const ReferenceKindRow &reference = reference_row(use.reference);
	if (reference.resolution == ReferenceResolution::Record && reference.index_space == RecordIndexSpace::File)
		use.scope = path();
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
	const RowIndex &indexed = row_index_of(rows_[index]);
	const auto found = indexed.records.find(address.child);
	if (found == indexed.records.end()) return false;
	out = found->second.at;
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
	if (!address.child) {
		// A row of a kind with no name of its own (a mission's event): its kind and its place among
		// the rows of its kind.
		const std::string name = top->name();
		if (!name.empty()) return name;
		size_t place = 1;
		for (const auto &other : rows_) {
			if (other.get() == top) break;
			place += other->kind == top->kind;
		}
		return std::string(kind_label(top->kind)) + " " + std::to_string(place);
	}
	Placement at;
	if (!placement(address, at)) return std::string();
	Value name;
	if (*at.spec.name_field && get(address, at.spec.name_field, name)) {
		if (const auto *text = std::get_if<std::string>(&name); text && !text->empty()) return *text;
	}
	return std::string(kind_label(at.spec.kind)) + " " + std::to_string(at.index + at.spec.first_number);
}

std::string Document::record_path(const NodeAddress &address) const {
	if (!row(address.row)) return std::string();
	std::string path;
	for (const NodeAddress &owner : ancestors(address)) path += record_name(owner) + "/";
	return path + record_name(address);
}

namespace {

// FNV-1a over a word, then a separator: two words never run together.
void digest_word(uint64_t &digest, const std::string &word) {
	for (const unsigned char c : word) {
		digest ^= c;
		digest *= 1099511628211ull;
	}
	digest ^= 0x1f;
	digest *= 1099511628211ull;
}

std::string value_word(const Value &value) {
	if (const auto *number = std::get_if<int64_t>(&value)) return std::to_string(*number);
	if (const auto *real = std::get_if<double>(&value)) {
		uint64_t bits = 0;
		std::memcpy(&bits, real, sizeof(bits));
		return "r" + std::to_string(bits);
	}
	return "s" + std::get<std::string>(value);
}

} // namespace

std::string Document::record_identity(const NodeAddress &address) const {
	const Node *top = row(address.row);
	if (!top) return std::string();
	NodeKind kind = top->kind;
	if (address.child) {
		Placement at;
		if (!placement(address, at)) return std::string();
		kind = at.spec.kind;
	}
	const char *token = kind_token(kind);
	const std::string word = *token ? std::string(token) : std::to_string(kind);
	const std::string own = own_name(address);
	if (!own.empty()) return word + ":" + own;
	uint64_t digest = 14695981039346656037ull;
	for (const FieldSchema &field : fields(kind)) {
		// A field its place derives (a stylesheet line's number) or one naming a record by its index (an
		// event's trigger naming another event: renumbered when one is inserted above) is no part of it.
		if (field.read_only || (field.reference != ReferenceKind::None &&
		                        reference_row(field.reference).resolution == ReferenceResolution::Record))
			continue;
		Value value;
		if (!present(address, field.id) || !get(address, field.id, value)) continue;
		digest_word(digest, field.id);
		digest_word(digest, value_word(value));
	}
	for (const Collection &collection : collections_of(address))
		for (size_t i = 0; i < collection.ids.size(); ++i)
			digest_word(digest, record_identity({address.row, collection.kind_at(i), collection.ids[i]}));
	return word + "#" + std::to_string(digest);
}

void key_findings(const DocumentBase &document, std::vector<Diagnostic> &findings, size_t from) {
	const Document *records = records_of(document);
	if (!records) return;
	for (size_t i = from; i < findings.size(); ++i) {
		Diagnostic &d = findings[i];
		if (!d.row_id || !d.record_key.empty() || d.asset != document.path()) continue;
		d.record_key = records->record_identity({d.row_id, d.record_kind, d.child_id});
	}
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
			if (position >= collection.ids.size()) continue;
			// A step names its record's own kind (a list of several kinds: the one at the position).
			const NodeKind kind = collection.kind_at(position);
			if (locator_token(*this, kind) != token) continue;
			current = {top.id, kind, collection.ids[position]};
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

std::shared_ptr<Node> Document::make_node(NodeKind, NodeId,
                                          const std::vector<std::shared_ptr<const Node>> &,
                                          std::string &error) {
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

bool Document::paste_rows(const Edit &, const std::vector<std::shared_ptr<const Node>> &,
                          std::vector<std::shared_ptr<Node>> &, std::string &error) {
	error = "Paste inside a record: select where the records go.";
	return false;
}

bool Document::removal_edits(const std::vector<NodeAddress> &records, std::vector<Edit> &out, std::string &) const {
	for (const NodeAddress &record : records) {
		Edit edit;
		edit.operation = EditOperation::Remove;
		edit.address = record;
		out.push_back(edit);
	}
	return true;
}

bool Document::move_out_edits(const Edit &move, std::vector<Edit> &, std::string &error) const {
	const Node *own = row(move.address.row);
	error = "A record moves within its own " + std::string(own ? kind_label(own->kind) : "row") + ".";
	return false;
}

bool Document::set_file_value(std::shared_ptr<const FileState> &, const Edit &, Diagnostic &error) {
	return fail(error, path(), CoreFinding::DocumentValue, "This document has no file-wide values.");
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
	next_id_ = 1; last_added_ = 0; added_.clear(); made_.clear(); found_.clear();
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

bool Document::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	if (edits.empty()) return true;
	bool builds = false;
	const std::string key = step_key(edits, builds);
	// A coalesced batch (typing) applies to the rows as its group found them, so the group's one
	// step is those rows plus the latest values: an empty value typed on the way to a new one (an
	// image or a hotkey cleared and retyped, a name) cannot drop what the new value keeps, and a
	// group that ends empty is the clear. A gesture's batches build on each other instead.
	const bool reopened = !builds && history_.reopen(key);
	const auto refused = [&]() {
		if (reopened) history_.resume(); // the group's step as it was
		return false;
	};
	StagedRows staged(rows_, file_state_, [this](NodeId id) { return row_index(id); });
	// While the batch stages, path_in answers for the rows as it leaves them (its own index of each
	// row an edit changes the shape of); its indexes go with it.
	struct Staging {
		Document &document;
		~Staging() {
			document.staged_ = nullptr;
			document.staged_indexes_.clear();
		}
	} staging{*this};
	staged_ = &staged;
	std::vector<NodeId> made, added;
	found_.clear();
	if (!stage_edits(edits, staged, made, added, error)) {
		found_.clear();
		return refused();
	}
	staged.for_each_changed([this](Node &row) { after_edit(row); });
	EditStep step = staged.step();
	if (step.empty()) {
		// Nothing changes. A reopened group typed back to the values it found is no step at all:
		// the document stays as the group found it (clean again when that was saved).
		if (reopened) history_.drop();
		return true;
	}
	// The step is the type's to refuse before it commits.
	StepRefusal refusal;
	if (!accept_step(step, staged, refusal)) {
		const std::string message = refusal.message.empty() ? "This document refuses that change." : refusal.message;
		if (!refusal.code) {
			fail(error, path(), CoreFinding::DocumentStructure, message);
			return refused();
		}
		error = make_finding(*refusal.code, DiagnosticSeverity::Error, message, path(), refusal.field);
		error.record = refusal.record_name;
		error.row_id = refusal.record.row;
		error.child_id = refusal.record.child;
		error.record_kind = refusal.record.kind;
		return refused();
	}
	history_.commit(std::move(step), key);
	if (!added.empty()) {
		// What the batch made and kept: a row or a record a later edit of it removed is gone (so is
		// what a removed record held), and the edit that made it names nothing.
		const bool removes = std::any_of(edits.begin(), edits.end(), [](const Edit &edit) {
			return edit.operation == EditOperation::Remove;
		});
		if (removes) {
			const auto gone = [this](NodeId id) { return id && !address_of(id).row; };
			for (NodeId &id : made)
				if (gone(id)) id = 0;
			added.erase(std::remove_if(added.begin(), added.end(), gone), added.end());
		}
		added_ = std::move(added);
		made_ = std::move(made);
		last_added_ = added_.empty() ? 0 : added_.front();
	}
	return true;
}

std::string Document::step_key(const std::vector<Edit> &edits, bool &builds) {
	const uint64_t gesture = edits.front().gesture;
	bool same_gesture = gesture != 0, all_coalesce = true;
	for (const Edit &edit : edits) {
		same_gesture = same_gesture && edit.gesture == gesture;
		all_coalesce = all_coalesce && edit.coalesce && edit.operation == EditOperation::Set;
	}
	builds = same_gesture;
	// A gesture's batches fold over every row they change (S13 D7: a drag of several records is one
	// step), so its key names no row.
	if (same_gesture) return "g" + std::to_string(gesture);
	std::string key;
	if (all_coalesce)
		for (const Edit &edit : edits)
			key += (key.empty() ? "" : "|") + std::to_string(edit.address.row) + "/" + std::to_string(edit.address.child) + "/" + edit.field;
	return key;
}

bool Document::stage_edits(const std::vector<Edit> &edits, StagedRows &staged,
                           std::vector<NodeId> &made_by_edit, std::vector<NodeId> &added,
                           Diagnostic &error) {
	const IdAllocator allocate = [this] { return allocate_id(); };
	using C = CoreFinding; // the refusals' rows
	// A refusal: `code` in `words`, on `field` where it names one.
	const auto refuse = [&](C code, const std::string &words,
	                        const std::string &field = std::string()) {
		return fail(error, path(), code, words, field);
	};
	// A record of one kind where the address says another.
	const auto wrong_kind = [&](NodeKind is, NodeKind says) {
		return refuse(C::DocumentSelection, std::string("Wrong kind: the record is ") +
		                                    kind_label(is) + ", the address says " +
		                                    kind_words(*this, says) + ".");
	};
	// What each edit made, which a later edit names by batch_made(its index): a row (row == id),
	// or a record in the row `row`.
	struct Made {
		NodeId id = 0, row = 0;
	};
	std::vector<Made> made(edits.size());
	// The collections other records name by their index (S13 D8), whose order an edit that may move
	// their records is compared by (renumber): none for a document of a kind no Record reference
	// names, which pays nothing for them.
	const std::vector<TargetedCollection> targets = targeted_collections();
	// One edit staged against the rows as the edits before it left them; false, with `error`, when
	// it is refused.
	const auto stage_one = [&](size_t i) -> bool {
		Edit edit = edits[i];
		std::string message;
		// The hook's own words where it gave some, else `words`.
		const auto said = [&](const char *words) {
			return message.empty() ? std::string(words) : message;
		};
		const auto earlier = [&](NodeId id, Made &out) {
			const NodeId index = id - kBatchMadeBase;
			if (index >= NodeId(i) || !made[size_t(index)].id) return false;
			out = made[size_t(index)];
			return true;
		};
		const auto names_nothing_made = [&]() {
			return refuse(C::DocumentBatch,
			              "An edit names a record that no earlier edit of its batch made.");
		};
		const auto made_elsewhere = [&]() {
			return refuse(C::DocumentBatch, "An edit names a record its batch made in another row "
			                                "than the edit names.");
		};
		// A row or a record an earlier edit made, named by batch_made: its identity, in its row (a
		// made row named as a record, or as an owner, is the row itself).
		Made earlier_made;
		if (is_batch_made(edit.address.row)) {
			if (!earlier(edit.address.row, earlier_made)) return names_nothing_made();
			edit.address.row = earlier_made.row;
		}
		if (is_batch_made(edit.address.child)) {
			if (!earlier(edit.address.child, earlier_made)) return names_nothing_made();
			if (edit.address.row && edit.address.row != earlier_made.row) return made_elsewhere();
			edit.address.row = earlier_made.row;
			edit.address.child = earlier_made.id == earlier_made.row ? 0 : earlier_made.id;
		}
		if (is_batch_made(edit.parent)) {
			if (!earlier(edit.parent, earlier_made)) return names_nothing_made();
			if (edit.address.row && edit.address.row != earlier_made.row) return made_elsewhere();
			edit.address.row = earlier_made.row;
			edit.parent = earlier_made.id == earlier_made.row ? 0 : earlier_made.id;
		}

		// The file-wide state alone: a value, or a change the type made naming no row.
		if (edit.operation == EditOperation::SetFileValue) {
			if (!set_file_value(staged.state(), edit, error)) return false;
			return true;
		}
		if (edit.operation == EditOperation::Apply && !edit.address.row && !edit.address.child) {
			if (!edit.payload)
				return refuse(C::DocumentPayload, "This change carries nothing to apply.");
			// The type's change to the state as the batch left it, kept only when it changes
			// something: a copy handed back unchanged is no step.
			std::shared_ptr<const FileState> state = staged.state();
			bool changes = true;
			if (!apply_file_payload(state, *edit.payload, changes, message))
				return refuse(C::DocumentPayload, said("This document does not take that change."));
			if (changes) staged.state() = std::move(state);
			return true;
		}

		// The row the edit is about: the one it names, else the one its record or its owner is in.
		const bool into =
		        edit.operation == EditOperation::Add || edit.operation == EditOperation::Paste;
		if (into && edit.parent && !is_batch_made(edits[i].parent)) {
			const NodeAddress owner = address_of(edit.parent);
			if (!owner.row)
				return refuse(C::DocumentSelection, "The record to add into no longer exists.");
			if (edit.address.row && edit.address.row != owner.row)
				return refuse(C::DocumentSelection,
				              "The record to add into is in another row than the edit names.");
			edit.address.row = owner.row;
		} else if (!edit.address.row && edit.address.child) {
			edit.address.row = address_of(edit.address.child).row;
		}

		// A row of the file: added, pasted at the top level, duplicated, removed or moved.
		if (into && !edit.address.row) {
			if (edit.operation == EditOperation::Add) {
				// A row the game finds by the value asked already: the Add names it, makes none.
				if (!edit.field.empty())
					if (const NodeId found = existing_row_for(edit, staged.rows())) {
						made[i] = {found, found};
						found_.push_back(found);
						return true;
					}
				const NodeId id = allocate_id();
				std::shared_ptr<Node> row =
				        make_node(edit.address.kind, id, staged.rows(), message);
				if (!row)
					return refuse(C::DocumentKind, said("This document cannot add that record."));
				row->id = id;
				assign_ids(*row);
				// The new row's field, set in the same step (Edit::field on an Add).
				if (!edit.field.empty() &&
				    !set_field(*row, {id, edit.address.kind, 0}, edit.field, edit.value, message))
					return refuse(C::DocumentValue, said("Unknown field."), edit.field);
				// Where the type's order puts it (row_position: a mission's band).
				const size_t position = row_position(*row, staged.rows(), edit.position);
				staged.insert(std::move(row), position);
				made[i] = {id, id};
				added.push_back(id);
				return true;
			}
			std::vector<std::shared_ptr<Node>> pasted;
			if (!paste_rows(edit, staged.rows(), pasted, message))
				return refuse(C::DocumentPaste, said("These records cannot be pasted here."));
			// Each pasted row where the type's order puts it: one of the kind the row before it was
			// right after that row, the first of a kind at the place asked (a mission's rows of several
			// bands each land in their band nearest that place, the rows of a band in their order),
			// which moves past every row put in before it.
			size_t asked = std::min(edit.position, staged.size());
			size_t after = asked;
			NodeKind last = -1;
			for (auto &row : pasted) {
				const NodeId id = allocate_id();
				row->id = id;
				assign_ids(*row);
				if (!made[i].id) made[i] = {id, id};
				added.push_back(id);
				const NodeKind kind = row->kind;
				const size_t position = row_position(*row, staged.rows(), kind == last ? after : asked);
				staged.insert(std::move(row), position);
				if (position <= asked) ++asked;
				after = position + 1;
				last = kind;
			}
			return true;
		}
		const NodeId row_id = edit.address.row;
		if (staged.removed(row_id))
			return refuse(C::DocumentBatch,
			              "An edit names a row an earlier edit of its batch removed.");
		const Node *current = staged.find(row_id);
		if (!current) return refuse(C::DocumentSelection, "The selected record no longer exists.");
		const std::string row_label = kind_label(current->kind);
		const bool on_row = !into && !edit.address.child;
		if (on_row && current->kind != edit.address.kind)
			return wrong_kind(current->kind, edit.address.kind);
		if (on_row && (edit.operation == EditOperation::Duplicate ||
		               edit.operation == EditOperation::Remove ||
		               edit.operation == EditOperation::Move)) {
			if (edit.parent)
				return refuse(C::DocumentCollection, "A row moves among the rows only.");
			if (edit.operation == EditOperation::Remove) {
				staged.state() = state_after_remove(staged.state(), staged.size() - 1);
				staged_indexes_.erase(row_id);
				staged.remove(row_id);
			} else if (edit.operation == EditOperation::Duplicate) {
				std::shared_ptr<Node> copy = current->clone();
				copy->id = allocate_id();
				assign_ids(*copy);
				prepare_duplicate(*copy, *current, staged.rows());
				const NodeId id = copy->id;
				// Right after the row as the batch has left it, where the edit names no place; where
				// the type's order puts it (row_position).
				const size_t asked =
				        edit.position == SIZE_MAX ? staged.index_of(row_id) + 1 : edit.position;
				const size_t position = row_position(*copy, staged.rows(), asked);
				staged.insert(std::move(copy), position);
				made[i] = {id, id};
				added.push_back(id);
			} else {
				// A Move that leaves the row where it is changes nothing; the type's order keeps a row
				// among its own (row_position, the row still among the rows it reads).
				staged.move(row_id, row_position(*current, staged.rows(), edit.position));
			}
			return true;
		}

		// A record inside the row: the row's clone changed (the row cloned on its first touch).
		Node *updated = staged.touch(row_id);
		// A nested record's placement: the committed row's index until an edit of this batch
		// changes the row's shape (or the batch made the row), then the batch's own index of the
		// clone, made by one walk (placement_in, as path_in reads it).
		auto place = [&](NodeId child, Placement &at) { return placement_in(*updated, child, at); };
		// A structural hook changed what the row holds: from here its index is the batch's own, made
		// again when next asked (the new record's field an Add sets reads it, so does a later edit).
		const auto reshaped = [&] {
			staged.mark_reshaped(row_id);
			staged_indexes_.erase(row_id);
		};
		// The owner a record goes into: a record of this row, or the row itself (0).
		auto owner_of = [&](NodeId parent, NodeAddress &owner) {
			if (!parent || parent == row_id) {
				owner = {row_id, updated->kind, 0};
				return true;
			}
			Placement at;
			if (!place(parent, at)) return false;
			owner = {row_id, at.spec.kind, parent};
			return true;
		};
		auto find_collection = [&](const NodeAddress &owner, NodeKind kind, Collection &out) {
			for (const Collection &collection : collections(*updated, owner))
				if (collection.spec.holds(kind)) {
					out = collection;
					return true;
				}
			return false;
		};
		// An owner's collection of `kind` that is not there, or fixed.
		const auto holds_none = [&](NodeKind owner, NodeKind kind) {
			return refuse(C::DocumentCollection, std::string(kind_label(owner)) +
			                                     " records hold no " +
			                                     kind_words(*this, kind) + " records.");
		};
		const auto fixed = [&](const char *label, NodeKind owner, const char *what) {
			return refuse(C::DocumentCollection, std::string("The ") + label + " of this " +
			                                     kind_label(owner) + " is fixed: " + what);
		};
		const NodeAddress &address = edit.address;
		Edit hook = edit;
		Placement at;
		// Resolve the address: every edit but an Add or a Paste names an existing record of the
		// kind it says.
		if (!into) {
			if (!address.child) {
				if (updated->kind != address.kind) return wrong_kind(updated->kind, address.kind);
			} else if (!place(address.child, at)) {
				return refuse(C::DocumentSelection, "The selected record no longer exists.");
			} else if (at.spec.kind != address.kind) {
				return wrong_kind(at.spec.kind, address.kind);
			}
		}
		switch (edit.operation) {
		case EditOperation::Set: {
			// A Set of the value the field holds changes nothing (as the record reads it, a def's
			// number in its written units included, and whether an optional field is written).
			Value before, after;
			const bool read_before = read(*updated, address, edit.field, before);
			const bool written_before = read_present(*updated, address, edit.field);
			if (!set_field(*updated, address, edit.field, edit.value, message))
				return refuse(C::DocumentValue, said("Unknown field."), edit.field);
			if (read_before && read(*updated, address, edit.field, after) && after == before &&
			    read_present(*updated, address, edit.field) == written_before)
				return true;
			break;
		}
		case EditOperation::Apply: {
			// A change the type made in C++, to the record and the file-wide state as the batch has
			// left them; one that changes nothing is nothing, as a Set of the value held.
			if (!edit.payload)
				return refuse(C::DocumentPayload, "This change carries nothing to apply.");
			std::shared_ptr<const FileState> state = staged.state();
			bool changes = true;
			if (!apply_payload(*updated, address, *edit.payload, state, allocate, changes, message))
				return refuse(C::DocumentPayload, said("This document does not take that change."));
			reshaped();
			if (!changes) return true;
			staged.state() = std::move(state);
			break;
		}
		case EditOperation::Clear:
		case EditOperation::Write: {
			const FieldSchema *schema = field_schema(address.kind, edit.field);
			if (!schema) return refuse(C::DocumentValue, "Unknown field.", edit.field);
			if (!schema->optional || schema->read_only)
				return refuse(C::DocumentValue, "This field is always written.", edit.field);
			const bool written = edit.operation == EditOperation::Write;
			// A field already left out (Clear) or already written (Write) changes nothing.
			if (read_present(*updated, address, edit.field) == written) return true;
			if (!set_present(*updated, address, edit.field, written, message))
				return refuse(C::DocumentValue, said("This field is always written."), edit.field);
			break;
		}
		case EditOperation::Add:
		case EditOperation::Paste: {
			NodeAddress owner;
			if (!owner_of(edit.parent, owner))
				return refuse(C::DocumentSelection, "The record to add into no longer exists.");
			hook.parent = owner.child;
			if (edit.operation == EditOperation::Paste) {
				std::vector<NodeId> pasted;
				if (!paste_records(*updated, hook, allocate, pasted, message))
					return refuse(C::DocumentPaste, said("These records cannot be pasted here."));
				reshaped();
				added.insert(added.end(), pasted.begin(), pasted.end());
				if (!pasted.empty()) made[i] = {pasted.front(), row_id};
				break;
			}
			Collection collection;
			if (!find_collection(owner, address.kind, collection))
				return holds_none(owner.kind, address.kind);
			if (collection.spec.fixed)
				return fixed(collection.spec.label, owner.kind, "nothing is added to it.");
			NodeId one = 0;
			if (!edit_collection(*updated, hook, allocate, one, message))
				return refuse(C::DocumentCollection,
				              said("This collection cannot accept that edit."));
			reshaped();
			if (one) {
				added.push_back(one);
				made[i] = {one, row_id};
			}
			// The new record's field, set in the same step (Edit::field on an Add).
			if (!edit.field.empty() &&
			    (!one || !set_field(*updated, {row_id, address.kind, one}, edit.field, edit.value,
			                        message)))
				return refuse(C::DocumentValue, said("Unknown field."), edit.field);
			break;
		}
		case EditOperation::Duplicate:
		case EditOperation::Remove:
		case EditOperation::Move: {
			if (at.spec.fixed)
				return fixed(at.spec.label, at.owner.kind, "it stays where it is.");
			hook.parent = at.owner.child;
			// A copy goes right after its record as the batch has left it, where the edit names no
			// place.
			if (edit.operation == EditOperation::Duplicate && hook.position == SIZE_MAX)
				hook.position = at.index + 1;
			if (edit.operation == EditOperation::Move) {
				NodeAddress destination = at.owner;
				if (edit.parent && !owner_of(edit.parent, destination)) {
					const NodeAddress elsewhere = address_of(edit.parent);
					return refuse(C::DocumentCollection,
					              elsewhere.row && elsewhere.row != row_id
					                      ? "A record moves within its own " + row_label + "."
					                      : std::string("The destination no longer exists."));
				}
				Collection collection;
				if (!find_collection(destination, address.kind, collection))
					return holds_none(destination.kind, address.kind);
				if (collection.spec.fixed)
					return fixed(collection.spec.label, destination.kind, "nothing moves into it.");
				// Never into the record itself or anything it holds.
				for (NodeAddress up = destination; up.child;) {
					if (up.child == address.child)
						return refuse(C::DocumentCollection, "A record cannot move inside itself.");
					Placement above;
					if (!place(up.child, above)) break;
					up = above.owner;
				}
				// A Move that leaves the record where it is changes nothing (B5).
				if (destination == at.owner && !collection.ids.empty() &&
				    std::min(edit.position, collection.ids.size() - 1) == at.index)
					return true;
				hook.parent = destination.child;
			}
			NodeId one = 0;
			if (!edit_collection(*updated, hook, allocate, one, message))
				return refuse(C::DocumentCollection,
				              said("This collection cannot accept that edit."));
			reshaped();
			if (one) {
				added.push_back(one);
				made[i] = {one, row_id};
			}
			break;
		}
		default:
			return refuse(C::DocumentBatch, "This edit names no record.");
		}
		staged.mark_changed(row_id);
		return true;
	};
	for (size_t i = 0; i < edits.size(); ++i) {
		const bool reorders = !targets.empty() && structural(edits[i].operation);
		const std::vector<std::vector<NodeId>> before =
		        reorders ? collection_orders(targets, staged) : std::vector<std::vector<NodeId>>();
		if (!stage_one(i)) return false;
		if (reorders && !renumber(targets, before, staged, error)) return false;
	}
	made_by_edit.assign(edits.size(), 0);
	for (size_t i = 0; i < edits.size(); ++i) made_by_edit[i] = made[i].id;
	return true;
}

const std::vector<Document::TargetedCollection> &Document::targeted_collections() const {
	if (targets_known_) return targets_;
	// The Record kinds the type's schema names on a field of any of its kinds, each whose collection
	// is one of its record kinds, in the reference kinds' order.
	std::vector<bool> named(kReferenceKindCount, false);
	for (const RecordKindRow &row : kinds())
		for (const FieldSchema &field : fields(row.kind)) {
			const size_t k = static_cast<size_t>(field.reference);
			if (k < named.size() && reference_row(field.reference).resolution == ReferenceResolution::Record)
				named[k] = true;
		}
	targets_.clear();
	for (size_t k = 0; k < named.size(); ++k) {
		if (!named[k]) continue;
		const ReferenceKindRow &row = reference_row(static_cast<ReferenceKind>(k));
		const NodeKind held = kind_from_name(row.collection);
		if (held >= 0) targets_.push_back({row.kind, held});
	}
	targets_known_ = true;
	return targets_;
}

std::vector<std::vector<NodeAddress>> Document::record_sets() const {
	const std::vector<TargetedCollection> &targets = targeted_collections();
	std::vector<std::vector<NodeAddress>> sets(targets.size());
	if (targets.empty()) return sets;
	for (const auto &row : rows_) {
		for (size_t t = 0; t < targets.size(); ++t)
			if (targets[t].kind == row->kind) sets[t].push_back({row->id, row->kind, 0});
		const RowIndex &index = row_index_of(row);
		for (size_t t = 0; t < targets.size(); ++t)
			for (const NodeId id : index.targeted[t]) sets[t].push_back({row->id, targets[t].kind, id});
	}
	return sets;
}

std::string Document::own_name(const NodeAddress &address) const {
	const Node *top = row(address.row);
	if (!top) return std::string();
	if (!address.child) return top->name();
	Placement at;
	if (!placement(address, at) || !*at.spec.name_field) return std::string();
	Value name;
	if (!get(address, at.spec.name_field, name)) return std::string();
	const auto *text = std::get_if<std::string>(&name);
	return text ? *text : std::string();
}

bool Document::renumber_references(const StagedRows &, const RecordShift &shift,
                                   std::vector<Edit> &, std::string &error) const {
	error = std::string("Other records name each ") + reference_row(shift.reference).label +
	        " by its index, which this document does not renumber.";
	return false;
}

std::vector<std::vector<NodeId>> Document::collection_orders(
		const std::vector<TargetedCollection> &targets, const StagedRows &staged) const {
	std::vector<std::vector<NodeId>> orders(targets.size());
	for (const auto &row : staged.rows()) {
		for (size_t t = 0; t < targets.size(); ++t)
			if (targets[t].kind == row->kind) orders[t].push_back(row->id);
		// The row's own index (index_for: the committed row's, or the batch's own once an edit
		// reshaped it), each list copied before another row's index is asked for.
		const RowIndex *index = index_for(*row);
		RowIndex walked;
		if (!index) {
			walked = make_index(*row);
			index = &walked;
		}
		for (size_t t = 0; t < targets.size() && t < index->targeted.size(); ++t)
			orders[t].insert(orders[t].end(), index->targeted[t].begin(), index->targeted[t].end());
	}
	return orders;
}

bool Document::renumber(const std::vector<TargetedCollection> &targets,
                        const std::vector<std::vector<NodeId>> &before, StagedRows &staged,
                        Diagnostic &error) {
	const std::vector<std::vector<NodeId>> after = collection_orders(targets, staged);
	for (size_t t = 0; t < targets.size(); ++t) {
		if (after[t] == before[t]) continue;
		// Where each record the edit found stands now, and how many it left: one added past the
		// others moves none of them, but an index past them moves with the count (RecordShift::now).
		RecordShift shift;
		shift.reference = targets[t].reference;
		shift.kind = targets[t].kind;
		shift.after = after[t].size();
		std::unordered_map<NodeId, size_t> now;
		for (size_t k = 0; k < after[t].size(); ++k) now.emplace(after[t][k], k);
		shift.to.reserve(before[t].size());
		for (size_t k = 0; k < before[t].size(); ++k) {
			const auto found = now.find(before[t][k]);
			shift.to.push_back(found == now.end() ? RecordShift::kRemoved : found->second);
		}
		if (!shift.moves()) continue;
		std::vector<Edit> sites;
		std::string message;
		if (!renumber_references(staged, shift, sites, message))
			return fail(error, path(), CoreFinding::DocumentCollection,
			            message.empty() ? "This collection cannot accept that edit." : message);
		// The type's Sets, in the same step, against the rows as the edit left them.
		std::vector<NodeId> made, added;
		if (!sites.empty() && !stage_edits(sites, staged, made, added, error)) return false;
	}
	return true;
}

bool Document::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	if (load_generation != this->load_generation()) return false;
	editor::RowChanges rows;
	if (!history_.changes_since(revision, rows)) return false;
	out = std::move(rows);
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
	const auto found = index.records.find(address.child);
	return found != index.records.end() && found->second.at.spec.kind == address.kind;
}

const FieldSchema *Document::field_schema(NodeKind kind, const std::string &id) const { return find_field(kind, id); }

const FieldSchema *Document::find_field(NodeKind kind, const std::string &id) const {
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

Document::RowAnswers &Document::row_changes(NodeId row, const std::shared_ptr<const Node> &now,
                                            const std::shared_ptr<const Node> &saved) const {
	RowAnswers &changes = row_changes_[row];
	if (changes.now != now || changes.saved != saved) changes = RowAnswers{now, saved, {}, {}};
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
		if (after[i].spec.kind != before[i].spec.kind || after[i].ids != before[i].ids || after[i].kinds != before[i].kinds)
			return RecordChange::Changed;
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
