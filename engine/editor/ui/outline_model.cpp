#include <editor/ui/outline_model.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <variant>

#include <editor/assets/asset_registry.h>

namespace opennova::editor {

namespace {

// The most lines of text a master and detail cell shows before its box scrolls.
constexpr int kMostLines = 8;

// The first kind of row the file holds (RecordKindRow::top): its own records, whose rows a list
// names alone (another kind's after its label: a weapon table's carry limits).
const RecordKindRow *own_kind(const Document &document) {
	for (const RecordKindRow &row : document.kinds())
		if (row.top) return &row;
	return nullptr;
}

} // namespace

OutlineModel::OutlineModel(OutlineMode mode, OutlineFileValuesHook file_values, OutlineRowListedHook row_listed)
    : mode_(mode), file_values_(file_values), row_listed_(row_listed) {}

void OutlineModel::set_filter(const std::string &filter) {
	if (filter == filter_) return;
	filter_ = filter;
	needle_ = filter.empty() ? std::string() : normalized_logical_name(filter);
}

void OutlineModel::set_sort(bool by_name) { sort_ = by_name; }

void OutlineModel::set_every(bool every) { every_ = every; }

void OutlineModel::set_kinds(uint64_t mask) { kinds_ = mask; }

void OutlineModel::set_all_rows(bool all) { all_rows_ = all; }

uint64_t OutlineModel::kind_bit(const Document &document, NodeKind kind) {
	const std::vector<RecordKindRow> &kinds = document.kinds();
	for (size_t i = 0; i < kinds.size() && i < 64; ++i)
		if (kinds[i].kind == kind) return uint64_t(1) << i;
	return 0;
}

bool OutlineModel::row_listed(const Document &document, const Node &row) const {
	if (mode_ == OutlineMode::MasterDetail) return true;
	// A kind the table does not name has no chip: its rows are listed.
	const uint64_t bit = kind_bit(document, row.kind);
	if (bit && !(kinds_ & bit)) return false;
	return all_rows_ || !row_listed_ || row_listed_(document, row);
}

void OutlineModel::set_open(const OutlineLine &line, bool open) {
	if (line.forced) return;
	const bool changed = open ? open_.insert(key_of(line)).second : open_.erase(key_of(line)) > 0;
	if (changed) ++open_version_;
}

bool OutlineModel::is_open(const OutlineLine &line) const { return open_.count(key_of(line)) > 0; }

size_t OutlineModel::reveal(const Document &document, const std::vector<NodeAddress> &path, NodeId master) {
	if (path.empty()) return SIZE_MAX;
	const NodeAddress &shown = mode_ == OutlineMode::List ? path.front() : path.back();
	if (mode_ == OutlineMode::MasterDetail && !shown.child) return SIZE_MAX;
	if (mode_ == OutlineMode::Tree) {
		bool changed = false;
		for (size_t i = 0; i + 1 < path.size(); ++i) {
			changed = open_.insert({path[i], false, 0}).second || changed;
			changed = open_.insert({path[i], true, path[i + 1].kind}).second || changed;
		}
		if (changed) ++open_version_;
	}
	lines(document, master);
	size_t at = line_of(shown);
	if (at == SIZE_MAX && filtered()) {
		set_filter(std::string());
		lines(document, master);
		at = line_of(shown);
	}
	// Its row's kind not listed, or its row one the listed hook leaves out: listed.
	const Node *row = at == SIZE_MAX ? document.row(path.front().row) : nullptr;
	if (row && !row_listed(document, *row)) {
		kinds_ |= kind_bit(document, row->kind);
		if (!row_listed(document, *row)) all_rows_ = true;
		lines(document, master);
		at = line_of(shown);
	}
	return at;
}

bool OutlineModel::matches(const std::string &text) const {
	return needle_.empty() || normalized_logical_name(text).find(needle_) != std::string::npos;
}

const std::vector<OutlineLine> &OutlineModel::lines(const Document &document, NodeId master) {
	Key key;
	key.made = true;
	key.document = document.identity();
	key.load = document.load_generation();
	key.revision = document.revision();
	key.open = open_version_;
	key.filter = filter_;
	key.sort = sort_;
	key.every = every_;
	key.all_rows = all_rows_;
	key.kinds = kinds_;
	key.master = mode_ == OutlineMode::MasterDetail ? master : 0;
	if (key == key_) return lines_;
	// The revision alone moved (an edit, an undo, a redo): the rows the document's change set names.
	if (key.same_but_revision(key_)) {
		ChangeSet changes;
		const RowChanges *rows =
				document.changes_since(key_.load, key_.revision, changes) ? std::get_if<RowChanges>(&changes) : nullptr;
		if (rows && follow_changes(document, *rows, key.master)) {
			key_ = key;
			return lines_;
		}
	}
	key_ = key;
	++lines_made_;
	make_detail_columns(document);
	const auto &rows = document.rows();
	rows_.assign(rows.size(), RowLines());
	for (size_t i = 0; i < rows.size(); ++i) make_row(document, i, key.master, rows_[i]);
	join_rows();
	return lines_;
}

void OutlineModel::make_detail_columns(const Document &document) {
	columns_.clear();
	has_detail_ = false;
	detail_kind_ = 0;
	detail_label_ = "";
	if (mode_ != OutlineMode::MasterDetail) return;
	// The detail records' kind: the first collection a row holds (a section's strings).
	for (const auto &row : document.rows()) {
		if (!row) continue;
		const std::vector<Document::Collection> held = document.collections_of({row->id, row->kind, 0});
		if (held.empty()) continue;
		has_detail_ = true;
		detail_kind_ = held.front().spec.kind;
		detail_label_ = held.front().spec.label;
		break;
	}
	if (!has_detail_) return;
	for (const FieldSchema &field : document.fields(detail_kind_))
		if (field.type == FieldType::Text && !field.read_only) columns_.push_back(&field);
}

bool OutlineModel::follow_changes(const Document &document, const RowChanges &changes, NodeId master) {
	const auto &rows = document.rows();
	if (mode_ == OutlineMode::MasterDetail) {
		// The detail columns come from the first row holding a collection: made anew should that move.
		const bool had = has_detail_;
		const NodeKind kind = detail_kind_;
		const std::vector<const FieldSchema *> columns = columns_;
		make_detail_columns(document);
		if (has_detail_ != had || detail_kind_ != kind || columns_ != columns) return false;
	}
	if (!changes.reshapes()) {
		// The rows stand as they were: each one changed made again in its place.
		if (rows_.size() != rows.size()) return false;
		bool made = false;
		for (size_t i = 0; i < rows.size(); ++i)
			if (rows[i] && changes.was_changed(rows[i]->id)) {
				make_row(document, i, master, rows_[i]);
				made = true;
			}
		if (!made) return true;
	} else {
		// The rows in the document's order: one kept as it was moved to its place, one changed or
		// added made, one removed gone with the slots of the order before.
		std::unordered_map<NodeId, size_t> was;
		for (size_t i = 0; i < rows_.size(); ++i)
			if (rows_[i].row) was.emplace(rows_[i].row, i);
		std::vector<RowLines> placed(rows.size());
		for (size_t i = 0; i < rows.size(); ++i) {
			if (!rows[i]) continue;
			const auto kept = was.find(rows[i]->id);
			if (kept == was.end() || changes.was_changed(rows[i]->id)) {
				make_row(document, i, master, placed[i]);
				continue;
			}
			placed[i] = std::move(rows_[kept->second]);
			place_row(placed[i], i);
		}
		rows_ = std::move(placed);
	}
	join_rows();
	return true;
}

void OutlineModel::make_row(const Document &document, size_t index, NodeId master, RowLines &out) {
	out = RowLines();
	const auto &node = document.rows()[index];
	if (!node) return;
	++rows_made_;
	out.row = node->id;
	if (!row_listed(document, *node)) return;
	const NodeAddress row{node->id, node->kind, 0};
	switch (mode_) {
	case OutlineMode::Tree:
		// The row, and under it what is open; under a filter, the records it keeps and what holds them.
		if (needle_.empty()) add_record(document, row, 0, index, out.lines);
		else add_filtered_record(document, row, 0, index, out.lines);
		break;
	case OutlineMode::List: {
		// Its line where the filter keeps its name; a row of another kind than the file's own after
		// its kind's label.
		const std::string name = document.record_name(row);
		if (!matches(name)) break;
		OutlineLine line = record_line(document, row, 0, index, false);
		const RecordKindRow *own = own_kind(document);
		if (own && row.kind != own->kind) line.text = std::string(document.kind_label(row.kind)) + ": " + line.text;
		if (sort_) out.order = normalized_logical_name(name);
		out.lines.push_back(std::move(line));
		break;
	}
	case OutlineMode::MasterDetail: {
		// The row as a master; its detail records where they are listed (the master row's, or every
		// row's that the filter keeps with Every), each with its row's name and the lines its
		// tallest cell shows.
		out.master = record_line(document, row, 0, index, false);
		if (has_detail_) {
			if (!every_row() && node->id != master) break;
			const std::string row_name = document.record_name(row);
			for (const Document::Collection &collection : document.collections_of(row)) {
				if (collection.spec.kind != detail_kind_) continue;
				for (size_t i = 0; i < collection.ids.size(); ++i) {
					const NodeAddress record{row.row, detail_kind_, collection.ids[i]};
					bool kept = needle_.empty();
					int tallest = 1;
					for (const FieldSchema *field : columns_) {
						Value value;
						if (!document.get(record, field->id, value)) continue;
						const auto *text = std::get_if<std::string>(&value);
						if (!text) continue;
						kept = kept || matches(*text);
						if (field->multiline)
							tallest = std::max(tallest,
							                   std::min(kMostLines, 1 + int(std::count(text->begin(), text->end(), '\n'))));
					}
					if (!kept) continue;
					OutlineLine line;
					line.address = record;
					line.index = i;
					line.lines = tallest;
					line.row_name = row_name;
					out.lines.push_back(std::move(line));
				}
			}
		}
		break;
	}
	}
}

void OutlineModel::place_row(RowLines &row, size_t index) {
	if (mode_ == OutlineMode::MasterDetail) {
		row.master.index = index;
		return;
	}
	// A tree's and a list's row line is the first of its lines.
	if (!row.lines.empty() && row.lines.front().depth == 0 && !row.lines.front().collection) row.lines.front().index = index;
}

void OutlineModel::join_rows() {
	lines_.clear();
	masters_.clear();
	if (mode_ == OutlineMode::MasterDetail)
		for (const RowLines &row : rows_)
			if (row.row) masters_.push_back(row.master);
	if (mode_ == OutlineMode::List && sort_) {
		// By name, a display order that keeps each row's place in the file.
		std::vector<const RowLines *> kept;
		for (const RowLines &row : rows_)
			if (!row.lines.empty()) kept.push_back(&row);
		std::stable_sort(kept.begin(), kept.end(), [](const RowLines *a, const RowLines *b) { return a->order < b->order; });
		lines_.reserve(kept.size());
		for (const RowLines *row : kept) lines_.push_back(row->lines.front());
		return;
	}
	for (const RowLines &row : rows_) lines_.insert(lines_.end(), row.lines.begin(), row.lines.end());
}

size_t OutlineModel::line_of(const NodeAddress &address) const {
	for (size_t i = 0; i < lines_.size(); ++i)
		if (!lines_[i].collection && lines_[i].address == address) return i;
	return SIZE_MAX;
}

OutlineClick OutlineModel::click(size_t clicked, const NodeAddress &primary, bool ctrl, bool shift) const {
	OutlineClick out;
	if (clicked >= lines_.size() || lines_[clicked].collection) return out;
	out.record = lines_[clicked].address;
	if (ctrl) {
		out.mode = SelectMode::Toggle;
		return out;
	}
	size_t from = shift ? line_of(primary) : SIZE_MAX;
	// A list's lines are its rows: the primary's row's where the primary is a record it holds.
	if (shift && from == SIZE_MAX && mode_ == OutlineMode::List)
		for (size_t i = 0; i < lines_.size() && from == SIZE_MAX; ++i)
			if (lines_[i].address.row == primary.row) from = i;
	if (from == SIZE_MAX || from == clicked) return out;
	for (size_t i = std::min(from, clicked); i <= std::max(from, clicked); ++i)
		if (i != clicked && !lines_[i].collection) out.records.push_back(lines_[i].address);
	return out;
}

bool OutlineModel::file_values(const Document &document, OutlineFileValues &out) const {
	return file_values_ && file_values_(document, out);
}

OutlineLine OutlineModel::record_line(const Document &document, const NodeAddress &record, int depth,
		size_t index, bool branch) const {
	OutlineLine line;
	line.address = record;
	line.depth = depth;
	line.index = index;
	line.branch = branch;
	line.text = document.record_title(record);
	std::string name = document.record_name(record);
	if (name != line.text) line.name = std::move(name);
	return line;
}

// A record and, open, its collections under it, an open collection's records under it.
void OutlineModel::add_record(const Document &document, const NodeAddress &record, int depth, size_t index,
		std::vector<OutlineLine> &out) const {
	const std::vector<Document::Collection> held = document.collections_of(record);
	OutlineLine line = record_line(document, record, depth, index, !held.empty());
	line.open = line.branch && is_open(line);
	const bool open = line.open;
	out.push_back(std::move(line));
	if (!open) return;
	for (const Document::Collection &collection : held) add_collection(document, record, collection, depth + 1, out);
}

namespace {

OutlineLine collection_line(const NodeAddress &owner, const Document::Collection &collection, int depth) {
	const Document::CollectionSpec &spec = collection.spec;
	OutlineLine line;
	line.address = owner;
	line.collection = true;
	line.kind = spec.kind;
	line.depth = depth;
	line.count = collection.ids.size();
	line.branch = !collection.ids.empty();
	line.addable = !spec.fixed && (spec.max == 0 || collection.ids.size() < spec.max);
	line.text = std::string(spec.label) + " (" + std::to_string(collection.ids.size()) + ")";
	return line;
}

} // namespace

void OutlineModel::add_collection(const Document &document, const NodeAddress &owner,
		const Document::Collection &collection, int depth, std::vector<OutlineLine> &out) const {
	OutlineLine line = collection_line(owner, collection, depth);
	line.open = line.branch && is_open(line);
	const bool open = line.open;
	out.push_back(std::move(line));
	if (!open) return;
	for (size_t i = 0; i < collection.ids.size(); ++i)
		add_record(document, {owner.row, collection.kind_at(i), collection.ids[i]}, depth + 1, i, out);
}

bool OutlineModel::add_filtered_record(const Document &document, const NodeAddress &record, int depth,
		size_t index, std::vector<OutlineLine> &out) const {
	const std::vector<Document::Collection> held = document.collections_of(record);
	const size_t at = out.size();
	out.push_back(record_line(document, record, depth, index, !held.empty()));
	const bool matched = matches(out[at].text) || (!out[at].name.empty() && matches(out[at].name));
	bool any = false;
	for (const Document::Collection &collection : held)
		any = add_filtered_collection(document, record, collection, depth + 1, out) || any;
	if (any) {
		out[at].open = out[at].forced = true;
		return true;
	}
	if (!matched) {
		out.resize(at);
		return false;
	}
	// Kept for itself with nothing under it kept: it opens as it does unfiltered, onto all it holds.
	out[at].open = out[at].branch && is_open(out[at]);
	if (out[at].open)
		for (const Document::Collection &collection : held) add_collection(document, record, collection, depth + 1, out);
	return true;
}

bool OutlineModel::add_filtered_collection(const Document &document, const NodeAddress &owner,
		const Document::Collection &collection, int depth, std::vector<OutlineLine> &out) const {
	const size_t at = out.size();
	out.push_back(collection_line(owner, collection, depth));
	bool any = false;
	for (size_t i = 0; i < collection.ids.size(); ++i)
		any = add_filtered_record(document, {owner.row, collection.kind_at(i), collection.ids[i]}, depth + 1, i, out) ||
		      any;
	if (!any) {
		out.resize(at);
		return false;
	}
	out[at].open = out[at].forced = true;
	return true;
}

} // namespace opennova::editor
