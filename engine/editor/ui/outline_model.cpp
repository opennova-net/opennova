#include <editor/ui/outline_model.h>

#include <algorithm>
#include <utility>

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

OutlineModel::OutlineModel(OutlineMode mode, OutlineFileValuesHook file_values)
    : mode_(mode), file_values_(file_values) {}

void OutlineModel::set_filter(const std::string &filter) {
	if (filter == filter_) return;
	filter_ = filter;
	needle_ = filter.empty() ? std::string() : normalized_logical_name(filter);
}

void OutlineModel::set_sort(bool by_name) { sort_ = by_name; }

void OutlineModel::set_every(bool every) { every_ = every; }

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
	key.master = mode_ == OutlineMode::MasterDetail ? master : 0;
	if (key == key_) return lines_;
	key_ = key;
	++lines_made_;
	lines_.clear();
	masters_.clear();
	columns_.clear();
	detail_kind_ = 0;
	detail_label_ = "";
	switch (mode_) {
	case OutlineMode::Tree: make_tree(document); break;
	case OutlineMode::List: make_list(document); break;
	case OutlineMode::MasterDetail: make_master_detail(document, master); break;
	}
	return lines_;
}

size_t OutlineModel::line_of(const NodeAddress &address) const {
	for (size_t i = 0; i < lines_.size(); ++i)
		if (!lines_[i].collection && lines_[i].address == address) return i;
	return SIZE_MAX;
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

// Every row, and under each open record its collections, under each open collection its records;
// under a filter, the records it keeps and what holds them, open.
void OutlineModel::make_tree(const Document &document) {
	const auto &rows = document.rows();
	for (size_t i = 0; i < rows.size(); ++i) {
		if (!rows[i]) continue;
		const NodeAddress row{rows[i]->id, rows[i]->kind, 0};
		if (needle_.empty()) add_record(document, row, 0, i);
		else add_filtered_record(document, row, 0, i);
	}
}

void OutlineModel::add_record(const Document &document, const NodeAddress &record, int depth, size_t index) {
	const std::vector<Document::Collection> held = document.collections_of(record);
	OutlineLine line = record_line(document, record, depth, index, !held.empty());
	line.open = line.branch && is_open(line);
	const bool open = line.open;
	lines_.push_back(std::move(line));
	if (!open) return;
	for (const Document::Collection &collection : held) add_collection(document, record, collection, depth + 1);
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
		const Document::Collection &collection, int depth) {
	OutlineLine line = collection_line(owner, collection, depth);
	line.open = line.branch && is_open(line);
	const bool open = line.open;
	lines_.push_back(std::move(line));
	if (!open) return;
	for (size_t i = 0; i < collection.ids.size(); ++i)
		add_record(document, {owner.row, collection.kind_at(i), collection.ids[i]}, depth + 1, i);
}

bool OutlineModel::add_filtered_record(const Document &document, const NodeAddress &record, int depth,
		size_t index) {
	const std::vector<Document::Collection> held = document.collections_of(record);
	const size_t at = lines_.size();
	lines_.push_back(record_line(document, record, depth, index, !held.empty()));
	const bool matched = matches(lines_[at].text) || (!lines_[at].name.empty() && matches(lines_[at].name));
	bool any = false;
	for (const Document::Collection &collection : held)
		any = add_filtered_collection(document, record, collection, depth + 1) || any;
	if (any) {
		lines_[at].open = lines_[at].forced = true;
		return true;
	}
	if (!matched) {
		lines_.resize(at);
		return false;
	}
	// Kept for itself with nothing under it kept: it opens as it does unfiltered, onto all it holds.
	lines_[at].open = lines_[at].branch && is_open(lines_[at]);
	if (lines_[at].open)
		for (const Document::Collection &collection : held) add_collection(document, record, collection, depth + 1);
	return true;
}

bool OutlineModel::add_filtered_collection(const Document &document, const NodeAddress &owner,
		const Document::Collection &collection, int depth) {
	const size_t at = lines_.size();
	lines_.push_back(collection_line(owner, collection, depth));
	bool any = false;
	for (size_t i = 0; i < collection.ids.size(); ++i)
		any = add_filtered_record(document, {owner.row, collection.kind_at(i), collection.ids[i]}, depth + 1, i) ||
		      any;
	if (!any) {
		lines_.resize(at);
		return false;
	}
	lines_[at].open = lines_[at].forced = true;
	return true;
}

// The rows whose name the filter keeps, in the file's order or by name; a row of another kind
// than the file's own after its kind's label.
void OutlineModel::make_list(const Document &document) {
	const RecordKindRow *own = own_kind(document);
	const auto &rows = document.rows();
	std::vector<std::pair<std::string, OutlineLine>> kept; // the name as names compare, the line
	for (size_t i = 0; i < rows.size(); ++i) {
		if (!rows[i]) continue;
		const NodeAddress row{rows[i]->id, rows[i]->kind, 0};
		OutlineLine line = record_line(document, row, 0, i, false);
		const std::string name = document.record_name(row);
		if (!matches(name)) continue;
		if (own && row.kind != own->kind) line.text = std::string(document.kind_label(row.kind)) + ": " + line.text;
		kept.emplace_back(sort_ ? normalized_logical_name(name) : std::string(), std::move(line));
	}
	if (sort_)
		std::stable_sort(kept.begin(), kept.end(),
		                 [](const auto &a, const auto &b) { return a.first < b.first; });
	lines_.reserve(kept.size());
	for (auto &line : kept) lines_.push_back(std::move(line.second));
}

// The rows as the master column lists them; the detail records of the master row (every row's,
// filtered, while every is set), each with its row's name and the lines its tallest cell shows.
void OutlineModel::make_master_detail(const Document &document, NodeId master) {
	const auto &rows = document.rows();
	for (size_t i = 0; i < rows.size(); ++i) {
		if (!rows[i]) continue;
		const NodeAddress row{rows[i]->id, rows[i]->kind, 0};
		masters_.push_back(record_line(document, row, 0, i, false));
	}
	// The detail records' kind: the first collection a row holds (a section's strings).
	bool found = false;
	for (size_t i = 0; i < rows.size() && !found; ++i) {
		if (!rows[i]) continue;
		for (const Document::Collection &collection : document.collections_of({rows[i]->id, rows[i]->kind, 0})) {
			detail_kind_ = collection.spec.kind;
			detail_label_ = collection.spec.label;
			found = true;
			break;
		}
	}
	if (!found) return;
	for (const FieldSchema &field : document.fields(detail_kind_))
		if (field.type == FieldType::Text && !field.read_only) columns_.push_back(&field);
	const bool every = every_row();
	for (size_t r = 0; r < rows.size(); ++r) {
		if (!rows[r] || (!every && rows[r]->id != master)) continue;
		const NodeAddress row{rows[r]->id, rows[r]->kind, 0};
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
				lines_.push_back(std::move(line));
			}
		}
	}
}

} // namespace opennova::editor
