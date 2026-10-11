#include "mission_variable_names.h"

#include <algorithm>
#include <cstring>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>

namespace opennova::editor {

namespace {

const bms::File *file_of(const MissionDocument &document) {
	const MissionRow *row = document.mission_row();
	return row ? &row->native : nullptr;
}

bool same_layers(const bms::File &a, const bms::File &b) {
	if (a.layer_records.size() != b.layer_records.size()) return false;
	for (size_t i = 0; i < a.layer_records.size(); ++i)
		if (std::memcmp(a.layer_records[i].name, b.layer_records[i].name, bms::kLayerRecordSize) != 0) return false;
	return true;
}

// A layer record's text as its field reads it: up to its first NUL, all 20 bytes where it has none.
std::string layer_text(const bms::LayerRecord &record) {
	size_t length = 0;
	while (length < bms::kLayerRecordSize && record.name[length] != 0) ++length;
	return std::string(record.name, length);
}

} // namespace

std::vector<mission::VariableName> mission_variable_names(const MissionDocument &document) {
	const bms::File *file = file_of(document);
	return file ? mission::variable_names(*file) : std::vector<mission::VariableName>();
}

std::string mission_variable_name(const MissionDocument &document, int64_t index) {
	for (const mission::VariableName &name : mission_variable_names(document))
		if (name.index == index) return name.name;
	return std::string();
}

mission::LayerTable mission_variable_table(const MissionDocument &document) {
	const bms::File *file = file_of(document);
	return file ? mission::layer_table(*file) : mission::LayerTable::Empty;
}

size_t mission_variable_names_left(const MissionDocument &document) {
	if (mission_variable_table(document) == mission::LayerTable::LayerNames) return 0;
	const size_t named = mission_variable_names(document).size();
	return named < mission::kMaxVariableNames ? mission::kMaxVariableNames - named : 0;
}

bool mission_variable_name_edits(const MissionDocument &document, int64_t index, const std::string &name,
                                 std::vector<Edit> &out, std::string &error) {
	out.clear();
	const MissionRow *row = document.mission_row();
	if (!row) {
		error = "The mission has no mission row.";
		return false;
	}
	if (index < 0 || index > mission::kVariableNameIndexMax) {
		error = "A variable name names V0 to V" + std::to_string(mission::kVariableNameIndexMax) + ".";
		return false;
	}
	std::vector<mission::VariableName> names = mission::variable_names(row->native);
	const auto at = std::find_if(names.begin(), names.end(),
	                             [index](const mission::VariableName &n) { return n.index == index; });
	if (name.empty()) {
		if (at == names.end()) return true;
		names.erase(at);
	} else if (at != names.end()) {
		if (at->name == name) return true;
		at->name = name;
	} else {
		names.push_back({int32_t(index), name});
	}
	bms::File changed = row->native;
	if (!mission::set_variable_names(changed, names, error)) return false;
	// A Set of each Layer record whose text the names change, by the record's identity in the Layers list.
	const NodeAddress owner{row->id, row->kind, 0};
	for (const Document::Collection &collection : document.collections_of(owner)) {
		if (collection.spec.kind != node_kind(MissionKind::Layer)) continue;
		for (size_t i = 0; i < collection.ids.size() && i < changed.layer_records.size(); ++i) {
			const std::string now = layer_text(changed.layer_records[i]);
			if (i < row->native.layer_records.size() && now == layer_text(row->native.layer_records[i])) continue;
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = {row->id, node_kind(MissionKind::Layer), collection.ids[i]};
			edit.field = "name";
			edit.value = now;
			out.push_back(std::move(edit));
		}
	}
	return true;
}

bool mission_variable_table_kept(const bms::File &before, const bms::File &after) {
	if (same_layers(before, after) || mission::layer_table(before) != mission::LayerTable::VariableNames) return true;
	switch (mission::layer_table(after)) {
	case mission::LayerTable::Empty: return true;
	case mission::LayerTable::LayerNames: return false;
	case mission::LayerTable::VariableNames: break;
	}
	bms::File canonical = after;
	std::string error;
	return mission::set_variable_names(canonical, mission::variable_names(after), error) && same_layers(canonical, after);
}

} // namespace opennova::editor
