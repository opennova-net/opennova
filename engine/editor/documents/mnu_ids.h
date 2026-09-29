#pragma once

#include <functional>
#include <vector>

#include <editor/model/value.h>
#include <formats/mnu/mnu_schema.h>

namespace opennova::editor {

// The identities of one menu record and of everything it holds (ADR 0046 S9h), kept
// beside the native record in the same shape: one vector per list the record's shape
// holds (mnu::schema_lists), each entry the identities of the record at that index.
// Every structural edit applies the same vector operation at the same index to the native
// list and to its identities, so an identity names one record for as long as it lives.
struct RecordIds {
	NodeId id = 0;
	std::vector<std::vector<RecordIds>> lists;
};

// Zero identities in the shape of a native record and everything it holds.
RecordIds shape_ids(const mnu::SchemaRecord &record);

// The invariant: `ids` has the shape of the native record (every list the same length,
// all the way down).
bool ids_match(const mnu::SchemaRecord &record, const RecordIds &ids);

// Every identity in one fixed order: the record, then each of its lists in order, each
// record of a list with everything it holds before the next. A load assigns identities in
// this order, so a reload of the same file gives every record the same place in it.
void for_each_identity(RecordIds &ids, const std::function<void(NodeId &)> &fn);

} // namespace opennova::editor
