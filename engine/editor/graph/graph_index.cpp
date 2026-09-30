#include <editor/graph/graph_index.h>

#include <algorithm>

#include <editor/graph/graph_names.h>

namespace opennova::editor {

std::string GraphIndex::key_of(ReferenceKind kind, const std::string &name) {
	return std::string(reference_row(kind).token) + '\n' + name;
}

uint32_t GraphIndex::find(const std::string &path) const {
	const auto found = paths_.find(path);
	return found == paths_.end() ? kNone : found->second;
}

uint32_t GraphIndex::first_named(const std::string &key) const {
	const auto found = order_.lower_bound({key, std::string()});
	return found != order_.end() && found->first.first == key ? found->second : kNone;
}

uint32_t GraphIndex::add(const std::string &path, const std::string &logical_name,
		const std::string &key, AssetKind kind) {
	uint32_t id;
	if (free_.empty()) {
		id = static_cast<uint32_t>(slots_.size());
		slots_.emplace_back();
	} else {
		id = free_.back();
		free_.pop_back();
	}
	GraphSlot &slot = slots_[id];
	slot = GraphSlot();
	slot.path = path;
	slot.logical_name = logical_name;
	slot.key = key;
	slot.kind = kind;
	paths_[path] = id;
	order_[{key, path}] = id;
	return id;
}

void GraphIndex::remove(uint32_t id) {
	GraphSlot &slot = slots_[id];
	paths_.erase(slot.path);
	order_.erase({slot.key, slot.path});
	slot = GraphSlot();
	free_.push_back(id);
}

bool GraphIndex::before(Ref a, Ref b) const {
	if (a.slot == b.slot) return a.index < b.index;
	const GraphSlot &x = slots_[a.slot];
	const GraphSlot &y = slots_[b.slot];
	if (x.key != y.key) return x.key < y.key;
	return x.path < y.path;
}

const std::vector<GraphIndex::Ref> &GraphIndex::list(const Lists &lists, const std::string &key) {
	static const std::vector<Ref> kEmpty;
	const auto found = lists.find(key);
	return found == lists.end() ? kEmpty : found->second;
}

const std::vector<GraphIndex::Ref> &GraphIndex::symbols_of_kind(ReferenceKind kind) const {
	static const std::vector<Ref> kEmpty;
	const size_t index = static_cast<size_t>(kind);
	return index < kinds_.size() ? kinds_[index] : kEmpty;
}

std::vector<GraphIndex::Ref>::iterator GraphIndex::lower(std::vector<Ref> &list, Ref ref) const {
	return std::lower_bound(list.begin(), list.end(), ref,
	                        [this](Ref a, Ref b) { return before(a, b); });
}

void GraphIndex::insert(std::vector<Ref> &list, Ref ref) {
	// A file's places go in in the file's order, and the files are read in theirs, so a list
	// mostly grows at its end.
	if (list.empty() || before(list.back(), ref)) {
		list.push_back(ref);
		return;
	}
	list.insert(lower(list, ref), ref);
}

void GraphIndex::erase(std::vector<Ref> &list, Ref ref) {
	const auto at = lower(list, ref);
	if (at != list.end() && at->slot == ref.slot && at->index == ref.index) list.erase(at);
}

void GraphIndex::erase_from(Lists &lists, const std::string &key, Ref ref) {
	const auto found = lists.find(key);
	if (found == lists.end()) return;
	erase(found->second, ref);
	if (found->second.empty()) lists.erase(found);
}

void GraphIndex::erase_content(uint32_t id) {
	GraphSlot &slot = slots_[id];
	for (uint32_t i = 0; i < slot.symbols.size(); ++i) {
		const GraphSymbol &symbol = slot.symbols[i];
		erase_from(names_, key_of(symbol.kind, symbol.name), {id, i});
	}
	// A file's symbols of a kind lie together in the kind's list.
	for (std::vector<Ref> &list : kinds_) {
		const auto first = lower(list, Ref{id, 0});
		auto last = first;
		while (last != list.end() && last->slot == id) ++last;
		list.erase(first, last);
	}
	for (uint32_t i = 0; i < slot.edges.size(); ++i) {
		const GraphEdge &edge = slot.edges[i];
		const Ref ref{id, i};
		if (graph_names::is_style_reference(edge.value))
			erase_from(variables_, graph_names::style_variable(edge.value), ref);
		if (i >= slot.resolutions.size() || !slot.resolutions[i].resolved) continue;
		const EdgeResolution &resolution = slot.resolutions[i];
		erase_from(targets_, key_of(edge.kind, edge.target), ref);
		if (!resolution.file.empty()) erase_from(users_, resolution.file, ref);
		if (resolution.missing) erase(missing_, ref);
	}
	slot.resolutions.assign(slot.edges.size(), EdgeResolution());
	edges_ -= slot.edges.size();
	symbols_ -= slot.symbols.size();
}

void GraphIndex::insert_content(uint32_t id) {
	GraphSlot &slot = slots_[id];
	slot.resolutions.assign(slot.edges.size(), EdgeResolution());
	slot.edges_at.clear();
	slot.symbols_at.clear();
	slot.symbols_in.clear();
	std::array<std::vector<Ref>, kReferenceKindCount> of_kind;
	for (uint32_t i = 0; i < slot.symbols.size(); ++i) {
		const GraphSymbol &symbol = slot.symbols[i];
		slot.symbols_at[symbol.locator].push_back(i);
		slot.symbols_in[symbol.record].push_back(i);
		insert(names_[key_of(symbol.kind, symbol.name)], {id, i});
		const size_t kind = static_cast<size_t>(symbol.kind);
		if (kind < of_kind.size()) of_kind[kind].push_back({id, i});
	}
	for (size_t kind = 0; kind < of_kind.size(); ++kind) {
		if (of_kind[kind].empty()) continue;
		std::vector<Ref> &list = kinds_[kind];
		list.insert(lower(list, of_kind[kind].front()), of_kind[kind].begin(), of_kind[kind].end());
	}
	for (uint32_t i = 0; i < slot.edges.size(); ++i) {
		const GraphEdge &edge = slot.edges[i];
		slot.edges_at[edge.locator].push_back(i);
		if (graph_names::is_style_reference(edge.value))
			insert(variables_[graph_names::style_variable(edge.value)], {id, i});
	}
	edges_ += slot.edges.size();
	symbols_ += slot.symbols.size();
}

} // namespace opennova::editor
