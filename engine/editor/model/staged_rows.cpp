#include "staged_rows.h"

#include <algorithm>
#include <utility>

namespace opennova::editor {

namespace {

// Of `keys` (each distinct), the indexes of the longest run that ascends: the rows a reorder keeps in
// their order (patience sorting, n log n).
std::vector<size_t> longest_ascending(const std::vector<size_t> &keys) {
	std::vector<size_t> tails;                     // tails[k]: the index ending the best run of k + 1
	std::vector<size_t> before(keys.size(), SIZE_MAX); // each index's predecessor in its run
	for (size_t i = 0; i < keys.size(); ++i) {
		const auto at = std::lower_bound(tails.begin(), tails.end(), keys[i],
		                                 [&](size_t index, size_t key) { return keys[index] < key; });
		if (at != tails.begin()) before[i] = *(at - 1);
		if (at == tails.end()) tails.push_back(i);
		else *at = i;
	}
	std::vector<size_t> run;
	for (size_t i = tails.empty() ? SIZE_MAX : tails.back(); i != SIZE_MAX; i = before[i]) run.push_back(i);
	std::reverse(run.begin(), run.end());
	return run;
}

bool by_id(const RowSwap &a, const RowSwap &b) { return a.id() < b.id(); }

} // namespace

StagedRows::StagedRows(const std::vector<std::shared_ptr<const Node>> &rows, std::shared_ptr<const FileState> state,
                       RowFinder find)
		: committed_(rows), find_(std::move(find)), state_before_(state), state_(std::move(state)) {}

void StagedRows::list() const {
	if (listed_) return;
	list_ = committed_;
	for (const auto &entry : touched_) {
		const size_t index = find_(entry.first);
		if (index < list_.size()) list_[index] = entry.second.row;
	}
	listed_ = true;
}

const std::vector<std::shared_ptr<const Node>> &StagedRows::rows() const {
	list();
	return list_;
}

size_t StagedRows::size() const { return shaped_ ? list_.size() : committed_.size(); }

size_t StagedRows::index_of(NodeId id) const {
	if (!shaped_) return find_(id);
	if (!positions_known_) {
		positions_.clear();
		for (size_t i = 0; i < list_.size(); ++i) positions_[list_[i]->id] = i;
		positions_known_ = true;
	}
	const auto found = positions_.find(id);
	return found == positions_.end() ? list_.size() : found->second;
}

const Node *StagedRows::find(NodeId id) const {
	const auto touched = touched_.find(id);
	if (touched != touched_.end()) return touched->second.row.get();
	const size_t index = index_of(id);
	if (index >= size()) return nullptr;
	return shaped_ ? list_[index].get() : committed_[index].get();
}

Node *StagedRows::touch(NodeId id) {
	const auto touched = touched_.find(id);
	if (touched != touched_.end()) return touched->second.row.get();
	const size_t index = index_of(id);
	if (index >= size()) return nullptr;
	std::shared_ptr<Node> clone = (shaped_ ? list_[index] : committed_[index])->clone();
	if (listed_) list_[index] = clone;
	Node *edited = clone.get();
	touched_.emplace(id, Touched{std::move(clone)});
	return edited;
}

void StagedRows::mark_changed(NodeId id) {
	const auto touched = touched_.find(id);
	if (touched != touched_.end()) touched->second.changed = true;
}

void StagedRows::mark_reshaped(NodeId id) {
	const auto touched = touched_.find(id);
	if (touched != touched_.end()) touched->second.reshaped = true;
}

bool StagedRows::reshaped(NodeId id) const {
	const auto touched = touched_.find(id);
	return touched != touched_.end() && touched->second.reshaped;
}

void StagedRows::insert(std::shared_ptr<Node> row, size_t position) {
	list();
	const NodeId id = row->id;
	list_.insert(list_.begin() + std::ptrdiff_t(std::min(position, list_.size())), row);
	touched_[id] = Touched{std::move(row), true, true, true};
	shaped_ = true;
	positions_known_ = false;
}

void StagedRows::remove(NodeId id) {
	list();
	const size_t index = index_of(id);
	if (index >= list_.size()) return;
	list_.erase(list_.begin() + std::ptrdiff_t(index));
	const auto touched = touched_.find(id);
	const bool added = touched != touched_.end() && touched->second.added;
	if (touched != touched_.end()) touched_.erase(touched);
	// A row the batch added and removed again leaves nothing behind.
	if (!added) removed_.insert(id);
	shaped_ = true;
	positions_known_ = false;
}

bool StagedRows::move(NodeId id, size_t position) {
	list();
	const size_t index = index_of(id);
	if (index >= list_.size()) return false;
	const size_t target = std::min(position, list_.size() - 1);
	if (target == index) return false;
	std::shared_ptr<const Node> row = list_[index];
	list_.erase(list_.begin() + std::ptrdiff_t(index));
	list_.insert(list_.begin() + std::ptrdiff_t(target), std::move(row));
	shaped_ = true;
	positions_known_ = false;
	return true;
}

void StagedRows::for_each_changed(const std::function<void(Node &)> &fn) {
	for (auto &entry : touched_)
		if (entry.second.changed) fn(*entry.second.row);
}

EditStep StagedRows::step() const {
	EditStep step;
	step.before_state = state_before_;
	step.after_state = state_;
	// The version a row commits: its clone when an edit changed it (or the batch added it), else its
	// committed version (a row only touched stays the committed row: the saved baseline compares it by
	// pointer).
	const auto version = [&](NodeId id, const std::shared_ptr<const Node> &committed) -> std::shared_ptr<const Node> {
		const auto touched = touched_.find(id);
		return touched != touched_.end() && touched->second.changed ? touched->second.row : committed;
	};
	if (!shaped_) {
		for (const auto &entry : touched_) {
			if (!entry.second.changed) continue;
			const size_t index = find_(entry.first);
			if (index >= committed_.size()) continue;
			step.swaps.push_back(RowSwap{committed_[index], entry.second.row, index, index, false});
		}
		std::sort(step.swaps.begin(), step.swaps.end(), by_id);
		return step;
	}
	// The committed rows the batch keeps, in the batch's order, by their committed index; the rows it
	// adds; the rows it removes.
	std::vector<size_t> kept_at, kept_from;
	for (size_t at = 0; at < list_.size(); ++at) {
		const NodeId id = list_[at]->id;
		const auto touched = touched_.find(id);
		if (touched != touched_.end() && touched->second.added) {
			step.swaps.push_back(RowSwap{nullptr, touched->second.row, 0, at, false});
			continue;
		}
		kept_at.push_back(at);
		kept_from.push_back(find_(id));
	}
	for (size_t from = 0; from < committed_.size(); ++from)
		if (removed_.count(committed_[from]->id)) step.swaps.push_back(RowSwap{committed_[from], nullptr, from, 0, false});
	// The rows kept on the longest run in their committed order stay in place; each other one moved.
	const std::vector<size_t> run = longest_ascending(kept_from);
	std::vector<bool> in_order(kept_from.size(), false);
	for (const size_t i : run) in_order[i] = true;
	for (size_t i = 0; i < kept_from.size(); ++i) {
		const std::shared_ptr<const Node> &before = committed_[kept_from[i]];
		const std::shared_ptr<const Node> after = version(before->id, before);
		if (!in_order[i]) step.swaps.push_back(RowSwap{before, after, kept_from[i], kept_at[i], true});
		else if (after != before) step.swaps.push_back(RowSwap{before, after, kept_from[i], kept_at[i], false});
	}
	std::sort(step.swaps.begin(), step.swaps.end(), by_id);
	return step;
}

} // namespace opennova::editor
