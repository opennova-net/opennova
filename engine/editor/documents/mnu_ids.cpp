#include "mnu_ids.h"

namespace opennova::editor {

RecordIds shape_ids(const mnu::SchemaRecord &record) {
	RecordIds ids;
	const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(record.shape);
	ids.lists.resize(lists.size());
	for (size_t list = 0; list < lists.size(); ++list) {
		const size_t size = mnu::schema_list_size(record, list);
		ids.lists[list].reserve(size);
		for (size_t i = 0; i < size; ++i) ids.lists[list].push_back(shape_ids(mnu::schema_list_at(record, list, i)));
	}
	return ids;
}

bool ids_match(const mnu::SchemaRecord &record, const RecordIds &ids) {
	const std::vector<mnu::SchemaList> &lists = mnu::schema_lists(record.shape);
	if (ids.lists.size() != lists.size()) return false;
	for (size_t list = 0; list < lists.size(); ++list) {
		const size_t size = mnu::schema_list_size(record, list);
		if (ids.lists[list].size() != size) return false;
		for (size_t i = 0; i < size; ++i)
			if (!ids_match(mnu::schema_list_at(record, list, i), ids.lists[list][i])) return false;
	}
	return true;
}

void for_each_identity(RecordIds &ids, const std::function<void(NodeId &)> &fn) {
	fn(ids.id);
	for (auto &list : ids.lists)
		for (RecordIds &entry : list) for_each_identity(entry, fn);
}

} // namespace opennova::editor
