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
// of the same file gives every record the same place in it.
inline void for_each_identity(RecordIds &ids, const std::function<void(NodeId &)> &fn) {
	fn(ids.id);
	for (auto &list : ids.lists)
		for (RecordIds &entry : list) for_each_identity(entry, fn);
}

// What the identities of a record and of everything it holds add to a row's footprint
// (Node::footprint): each list's entries and what each holds, not the record's own object.
inline size_t footprint_of(const RecordIds &ids) {
	size_t bytes = footprint_of(ids.lists);
	for (const std::vector<RecordIds> &list : ids.lists) {
		bytes += footprint_of(list);
		for (const RecordIds &entry : list) bytes += footprint_of(entry);
	}
	return bytes;
}

} // namespace opennova::editor
