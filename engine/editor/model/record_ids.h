#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include <editor/model/node.h>
#include <editor/model/value.h>

namespace opennova::editor {

// The identities of one record and of everything it holds (ADR 0046 S9h; S13 D10 moved them into the
// model from the menu, where they began), kept beside the native record in the same shape: one vector
// per list the record's kind holds (its table's lists, model/table_shape.h), each entry the
// identities of the record at that index. Every structural edit applies the same vector operation at
// the same index to the native list and to its identities, so an identity names one record for as
// long as it lives. A table row's own identity is its Node's (TableRow::ids leaves `id` 0).
struct RecordIds {
	NodeId id = 0;
	std::vector<std::vector<RecordIds>> lists;
};

// Every identity in one fixed order: the record, then each of its lists in order, each record of a
// list with everything it holds before the next. A load assigns identities in this order, so a reload
// of the same file gives every record the same place in it. Walked from an explicit stack, never a
// call a level (a menu nests its windows thousands deep).
inline void for_each_identity(RecordIds &ids, const std::function<void(NodeId &)> &fn) {
	struct Level {
		RecordIds *ids;
		size_t list, index;
	};
	fn(ids.id);
	std::vector<Level> stack{{&ids, 0, 0}};
	while (!stack.empty()) {
		Level &top = stack.back();
		if (top.list == top.ids->lists.size()) {
			stack.pop_back();
			continue;
		}
		std::vector<RecordIds> &list = top.ids->lists[top.list];
		if (top.index == list.size()) {
			++top.list;
			top.index = 0;
			continue;
		}
		RecordIds &entry = list[top.index++];
		fn(entry.id);
		stack.push_back({&entry, 0, 0});
	}
}

// What the identities of a record and of everything it holds add to a row's footprint
// (Node::footprint): each list's entries and what each holds, not the record's own object. Summed
// over an explicit stack.
inline size_t footprint_of(const RecordIds &ids) {
	size_t bytes = 0;
	std::vector<const RecordIds *> stack{&ids};
	while (!stack.empty()) {
		const RecordIds &at = *stack.back();
		stack.pop_back();
		bytes += footprint_of(at.lists);
		for (const std::vector<RecordIds> &list : at.lists) {
			bytes += footprint_of(list);
			for (const RecordIds &entry : list) stack.push_back(&entry);
		}
	}
	return bytes;
}

} // namespace opennova::editor
