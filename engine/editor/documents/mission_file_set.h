#pragma once

#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/value.h>

namespace opennova::editor {

// The files the game finds by a mission's name (ADR 0046 S14; the witnessed table is
// runtime/mission/mission_sidecars.h), each as the reference the mission makes of it: the row's
// role, the kind of edge, the kind of file it is, and whether the game runs without it (an optional
// one makes no finding when the project lacks it; the import still looks for it, so a mission
// imported with its dependencies brings the set). The mission's extraction emits one edge per row
// (mission_references), Rename renames the files the project has as the mission's companions
// (rename_transaction: plan_rename's companions), and a dialog's use names the bank from the record
// that uses it.
struct MissionFileSetRow {
	const char *role;   // the sidecar table's role ("text", "script", ...)
	ReferenceKind kind; // the edge's kind
	bool optional;      // the game runs without it: no finding when the project lacks it
	const char *words;  // "its string table"
};

// One row per sidecar role, in the sidecar table's order (runtime/mission/mission_sidecars.h).
const std::vector<MissionFileSetRow> &mission_file_set();
// The row of a role; null for an unknown one.
const MissionFileSetRow *mission_file_set_row(const std::string &role);

// A file of the mission's set the project has: its role, the file (project-relative), its name and
// the name it takes beside the mission renamed `new_mission` (the mission's new base name plus the
// row's extension, the file's own extension kept where the row's alternate was found).
struct MissionFileSetMember {
	std::string role;
	std::string path;
	std::string old_name;
	std::string new_name;
};
// The members a mission's set has in the scan, in the table's order, each once (a role's main name
// first, else its alternate), for a mission named `mission` (its logical name, "ASH_I5B.bms") renamed
// to `new_mission`.
std::vector<MissionFileSetMember> mission_file_set_members(const AssetScan &scan, const std::string &mission,
                                                           const std::string &new_mission);

} // namespace opennova::editor
