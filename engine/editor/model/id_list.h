#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/value.h>

namespace opennova::editor {

// The one list edit of a document type's records (ADR 0046 S13 D8): a list of native records and
// the list of their identities beside it, one per record in the same order, changed together by an
// Add, a Duplicate, a Remove or a Move of the collection they are (Document::edit_collection, whose
// edit the core has resolved and checked), so an identity names one record for as long as it
// lives. The record an edit names is found by its identity (edit.address.child). An Add puts
// `fresh`, a Duplicate a copy of the record, at the edit's position (the end past it) under an
// identity from `allocate`, which `added` receives; a Remove takes the record out; a Move takes it
// to its position (the last past it). Where the others then stand is the core's to read
// (Document::renumber_references). False, with `error`, for a record the list does not hold, lists
// of two lengths and any other operation.
template <class T>
bool edit_id_list(std::vector<T> &records, std::vector<NodeId> &ids, const Edit &edit, T fresh,
                  const std::function<NodeId()> &allocate, NodeId &added, std::string &error) {
	if (records.size() != ids.size()) {
		error = "The list's records and their identities disagree.";
		return false;
	}
	const auto at = [&](size_t index) { return std::ptrdiff_t(std::min(index, ids.size())); };
	const bool adding = edit.operation == EditOperation::Add;
	const size_t index = adding ? ids.size()
	                            : size_t(std::find(ids.begin(), ids.end(), edit.address.child) - ids.begin());
	if (!adding && index == ids.size()) {
		error = "The record no longer exists.";
		return false;
	}
	switch (edit.operation) {
	case EditOperation::Add:
	case EditOperation::Duplicate: {
		T record = adding ? std::move(fresh) : records[index];
		const std::ptrdiff_t place = at(edit.position);
		records.insert(records.begin() + place, std::move(record));
		added = allocate();
		ids.insert(ids.begin() + place, added);
		return true;
	}
	case EditOperation::Remove:
		records.erase(records.begin() + std::ptrdiff_t(index));
		ids.erase(ids.begin() + std::ptrdiff_t(index));
		return true;
	case EditOperation::Move: {
		const std::ptrdiff_t to = std::ptrdiff_t(std::min(edit.position, ids.size() - 1));
		T record = std::move(records[index]);
		const NodeId id = ids[index];
		records.erase(records.begin() + std::ptrdiff_t(index));
		ids.erase(ids.begin() + std::ptrdiff_t(index));
		records.insert(records.begin() + to, std::move(record));
		ids.insert(ids.begin() + to, id);
		return true;
	}
	default:
		error = "This list cannot take that edit.";
		return false;
	}
}

} // namespace opennova::editor
