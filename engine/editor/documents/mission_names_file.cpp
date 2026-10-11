// The .mis beside a mission (mission_names_file.h).
#include "mission_names_file.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/io/hash.h>
#include <editor/documents/mission_document.h>
#include <editor/project/project_files.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

namespace {

using K = MissionKind;
using Found = MissionNamesFile::Found;
constexpr NodeKind k(K kind) { return node_kind(kind); }

// The words the .mis writes of a trigger and an action, each one line of eight numbers (mission_mis_writer):
// what a trigger or an action of the .mis is to the mission's.
bool same_trigger(const bms::Trigger &a, const bms::Trigger &b) {
	return a.condition_flags == b.condition_flags && a.main_type == b.main_type && a.sub_type == b.sub_type &&
	       a.param1 == b.param1 && a.param2 == b.param2 && a.param3 == b.param3 && a.param4 == b.param4 &&
	       a.unknown7 == b.unknown7;
}
bool same_action(const bms::Action &a, const bms::Action &b) {
	return a.reserved0 == b.reserved0 && a.action_type == b.action_type && a.param1 == b.param1 && a.param2 == b.param2 &&
	       a.param3 == b.param3 && a.param4 == b.param4 && a.action_sub_type == b.action_sub_type &&
	       a.reserved1 == b.reserved1;
}
// Whether two events hold the same trigger and action lines.
bool same_lines(const mission::EventChain &a, const mission::EventChain &b) {
	if (a.triggers.size() != b.triggers.size() || a.actions.size() != b.actions.size()) return false;
	for (size_t i = 0; i < a.triggers.size(); ++i)
		if (!same_trigger(a.triggers[i], b.triggers[i])) return false;
	for (size_t i = 0; i < a.actions.size(); ++i)
		if (!same_action(a.actions[i], b.actions[i])) return false;
	return true;
}

bool name_bytes_empty(const bms::GroupRecord &record) {
	for (const char byte : record.name)
		if (byte != 0) return false;
	return true;
}

std::string counted(size_t count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

} // namespace

MissionNamesFile read_names_file(const bms::File &mission, const std::vector<mission::EventChain> &events,
                                 const std::vector<uint8_t> &bytes) {
	MissionNamesFile out;
	out.fingerprint = io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
	const std::string text(bytes.begin(), bytes.end());
	bms::File read;
	std::string error;
	std::vector<mission::MisIssue> issues;
	if (!mission::parse_mis_text(text, {}, read, error, &issues) || !mission::parse_mis_names(text, out.names, error)) {
		out.found = Found::Unreadable;
		out.why = error;
		return out;
	}
	for (size_t g = 0; g < read.group_records.size() && g < out.groups.size(); ++g)
		out.groups[g] = bms::group_name(read.group_records[g]);
	mission::RunReport report;
	if (!mission::split_event_chains(read, out.events, report)) {
		out.found = Found::Unreadable;
		out.why = "its events' triggers and actions do not lie in their events' runs";
		return out;
	}
	// The editor's own: what it reads, written again, is the bytes read (a file the original editor wrote says when
	// it saved it in its first line, and writes keys the editor's writer does not).
	{
		bms::File again = read;
		mission::sync_counts(again);
		std::string written, refused;
		out.ours = mission::write_mis_text(again, written, refused, nullptr, &out.names) && written == text;
	}
	if (out.events.size() != events.size()) {
		out.found = Found::Unmatched;
		out.why = "it holds " + counted(out.events.size(), "event", "events") + ", the mission " +
		          counted(events.size(), "event", "events");
		return out;
	}
	for (size_t i = 0; i < events.size(); ++i)
		if (!same_lines(out.events[i], events[i])) {
			out.found = Found::Unmatched;
			out.why = "its event " + std::to_string(i + 1) + " holds other triggers or actions than the mission's";
			return out;
		}
	std::set<int32_t> zones;
	for (const bms::AreaTrigger &area : mission.area_triggers) zones.insert(area.id);
	for (size_t id = 1; id < out.names.areas.size(); ++id)
		if (!out.names.areas[id].empty() && !zones.count(int32_t(id))) {
			out.found = Found::Unmatched;
			out.why = "it describes zone " + std::to_string(id) + ", which no area of the mission has";
			return out;
		}
	out.found = Found::Matched;
	return out;
}

mission::MisNames mission_names(const MissionDocument &document) {
	mission::MisNames out;
	for (const Node *row : document.rows_of(K::Event)) out.events.push_back(static_cast<const EventRow &>(*row).name);
	for (const Node *row : document.rows_of(K::Area)) {
		const AreaRow &area = static_cast<const AreaRow &>(*row);
		// The first area of a zone id: the original's slot holds one (a second of the id the writer refuses).
		if (area.native.id > 0 && size_t(area.native.id) < out.areas.size() && out.areas[size_t(area.native.id)].empty())
			out.areas[size_t(area.native.id)] = area.description;
	}
	for (const Node *row : document.rows_of(K::WaypointPath)) {
		const PathRow &path = static_cast<const PathRow &>(*row);
		if (path.native.number >= 0 && size_t(path.native.number) < out.paths.size())
			out.paths[size_t(path.native.number)] = path.description;
	}
	return out;
}

bool mission_names_text(const MissionDocument &document, std::string &text, std::string &error) {
	bms::File file;
	if (!document.compose(file)) {
		error = "The mission could not be composed.";
		return false;
	}
	const mission::MisNames names = mission_names(document);
	return mission::write_mis_text(file, text, error, nullptr, &names);
}

std::vector<Edit> take_matching_names(const MissionDocument &document) {
	std::vector<Edit> out;
	const MissionNamesFile &file = document.names_file();
	if (file.found != Found::Unmatched && file.found != Found::Matched) return out;
	const auto set = [&](const NodeAddress &address, const char *field, const std::string &value) {
		Edit edit;
		edit.operation = EditOperation::Set;
		edit.address = address;
		edit.field = field;
		edit.value = value;
		out.push_back(std::move(edit));
	};
	std::string refused;
	const std::vector<const Node *> events = document.rows_of(K::Event);
	for (size_t i = 0; i < events.size() && i < file.events.size() && i < file.names.events.size(); ++i) {
		const EventRow &row = static_cast<const EventRow &>(*events[i]);
		const std::string &name = file.names.events[i];
		if (name.empty() || name == row.name || !same_lines(file.events[i], row.native) ||
		    !mission::mis_name_allowed(name, mission::kMisEventNameChars, refused))
			continue;
		set({row.id, row.kind, 0}, "name", name);
	}
	std::set<int32_t> seen;
	for (const Node *node : document.rows_of(K::Area)) {
		const AreaRow &row = static_cast<const AreaRow &>(*node);
		const int32_t id = row.native.id;
		if (id <= 0 || size_t(id) >= file.names.areas.size() || !seen.insert(id).second) continue;
		const std::string &name = file.names.areas[size_t(id)];
		if (name.empty() || name == row.description || !mission::mis_name_allowed(name, mission::kMisDescriptionChars, refused))
			continue;
		set({row.id, row.kind, 0}, "description", name);
	}
	for (const Node *node : document.rows_of(K::WaypointPath)) {
		const PathRow &row = static_cast<const PathRow &>(*node);
		if (row.native.number < 0 || size_t(row.native.number) >= file.names.paths.size()) continue;
		const std::string &name = file.names.paths[size_t(row.native.number)];
		if (name.empty() || name == row.description || !mission::mis_name_allowed(name, mission::kMisDescriptionChars, refused))
			continue;
		set({row.id, row.kind, 0}, "description", name);
	}
	// A group the .bms names none of, by the description the .mis gives it (what the original editor's .bms writer
	// drops, bms.h GroupRecord).
	if (const MissionRow *mission = document.mission_row()) {
		const NodeAddress owner{mission->id, mission->kind, 0};
		for (const Document::Collection &collection : document.collections_of(owner)) {
			if (collection.spec.kind != k(K::Group)) continue;
			for (size_t g = 1; g < collection.ids.size() && g < mission->native.group_records.size() && g < file.groups.size(); ++g) {
				const std::string &name = file.groups[g];
				bms::GroupRecord probe = mission->native.group_records[g];
				if (name.empty() || !name_bytes_empty(probe) || !bms::set_group_name(probe, name, refused)) continue;
				set({mission->id, k(K::Group), collection.ids[g]}, "name", name);
			}
		}
	}
	return out;
}

void names_file_findings(const MissionDocument &document, std::vector<Diagnostic> &out) {
	const MissionNamesFile &file = document.names_file();
	const std::string mis = mission::mission_base_name(basename_of(document.path())) + ".mis";
	const uint64_t overwrite = document.names_file_overwrite();
	// Write it from the mission: the bytes a save then writes over.
	const auto write_over = [&](Diagnostic &d, uint64_t fingerprint) {
		if (fingerprint == 0 || overwrite == fingerprint) return;
		Edit edit;
		edit.operation = EditOperation::SetFileValue;
		edit.field = kMisOverwriteField;
		edit.value = int64_t(fingerprint);
		d.planned.push_back({"Write it from the mission",
		                     "The next Save writes " + mis + " from the mission, its names as the mission holds them, over "
		                     "what the file holds now (an edit of the mission, which Undo takes back until then).",
		                     {edit}});
	};
	const auto take = [&](Diagnostic &d) {
		std::vector<Edit> edits = take_matching_names(document);
		if (edits.empty()) return;
		const size_t count = edits.size();
		d.planned.push_back({"Take the names that still match",
		                     "Gives the mission the " + counted(count, "name", "names") + " of " + mis +
		                     " whose records the mission still holds alike: an event's where its triggers and actions are "
		                     "the same, an area's by its zone id, a path's, a group's where the mission names it none.",
		                     std::move(edits)});
	};
	if (file.outside) {
		Diagnostic d = make_finding(MissionFinding::NamesOutside, DiagnosticSeverity::Warning,
		                            mis + " changed outside the editor since it was read: the last Save left it as it is, "
		                                  "and the names the mission holds are not in it.",
		                            document.path());
		write_over(d, file.outside_fingerprint);
		out.push_back(std::move(d));
	} else if ((file.found == Found::Unmatched || file.found == Found::Unreadable) && overwrite != file.fingerprint) {
		const bool unread = file.found == Found::Unreadable;
		const std::string said =
		        unread ? mis + " does not read as a mission's text (" + file.why + "): the editor takes no name from it"
		               : mis + " holds another mission's events (" + file.why + "): the editor takes no name from it";
		Diagnostic d = make_finding(MissionFinding::NamesUnmatched, DiagnosticSeverity::Warning,
		                            said + (file.ours ? ", and Save writes it from the mission (the editor made it)."
		                                              : ", and Save leaves it as it is."),
		                            document.path());
		if (!unread) take(d);
		if (!file.ours) write_over(d, file.fingerprint);
		out.push_back(std::move(d));
	}
	if (!file.unwritten.empty())
		out.push_back(make_finding(MissionFinding::NamesUnwritten, DiagnosticSeverity::Warning,
		                           mis + " was not written: " + file.unwritten + " The mission's own file was saved.",
		                           document.path()));
}

} // namespace opennova::editor
