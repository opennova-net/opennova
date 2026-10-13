// The .mis beside a mission (mission_names_file.h).
#include "mission_names_file.h"

#include <algorithm>
#include <array>
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

// A reason as a sentence: its full stop where it has none.
std::string sentence(std::string words) {
	if (!words.empty() && words.back() != '.' && words.back() != '!' && words.back() != '?') words += '.';
	return words;
}

// Why a .mis's events (`held`) and the zones it describes (`names`) are not a mission's (its events, its areas' zone
// ids); "" where they are: its events the mission's by their index, each holding the same lines, and every zone it
// describes one the mission has.
std::string unmatched_why(const std::vector<mission::EventChain> &held, const mission::MisNames &names,
                          const std::vector<mission::EventChain> &events, const std::set<int32_t> &zones) {
	if (held.size() != events.size())
		return "it holds " + counted(held.size(), "event", "events") + ", the mission " + counted(events.size(), "event", "events");
	for (size_t i = 0; i < events.size(); ++i)
		if (!same_lines(held[i], events[i]))
			return "its event " + std::to_string(i + 1) + " holds other triggers or actions than the mission's";
	for (size_t id = 1; id < names.areas.size(); ++id)
		if (!names.areas[id].empty() && !zones.count(int32_t(id)))
			return "it describes zone " + std::to_string(id) + ", which no area of the mission has";
	return std::string();
}

uint64_t bytes_fingerprint(const std::vector<uint8_t> &bytes) {
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
}

// What the mission takes of a matching .mis: each name its setter takes (mission_table's name field:
// mission::mis_name_allowed; a group's, bms::set_group_name where the group's record names none), each it would
// refuse said in words.
void take_kept(const bms::File &mission, size_t event_count, MissionNamesFile &out) {
	std::string why;
	out.kept.events.assign(event_count, std::string());
	for (size_t i = 0; i < event_count && i < out.names.events.size(); ++i) {
		const std::string &name = out.names.events[i];
		if (name.empty()) continue;
		if (mission::mis_name_allowed(name, mission::kMisEventNameChars, why)) out.kept.events[i] = name;
		else out.refused.push_back("event " + std::to_string(i + 1) + "'s name (" + why + ")");
	}
	for (size_t id = 1; id < out.names.areas.size(); ++id) {
		const std::string &name = out.names.areas[id];
		if (name.empty()) continue;
		if (mission::mis_name_allowed(name, mission::kMisDescriptionChars, why)) out.kept.areas[id] = name;
		else out.refused.push_back("zone " + std::to_string(id) + "'s description (" + why + ")");
	}
	if (!out.names.paths[0].empty()) out.refused.push_back("path 0's description (path 0 is no path)");
	for (size_t n = 1; n < out.names.paths.size(); ++n) {
		const std::string &name = out.names.paths[n];
		if (name.empty()) continue;
		if (mission::mis_name_allowed(name, mission::kMisDescriptionChars, why)) out.kept.paths[n] = name;
		else out.refused.push_back("path " + std::to_string(n) + "'s description (" + why + ")");
	}
	for (size_t v = 0; v < out.names.variables.size(); ++v) {
		const std::string &name = out.names.variables[v];
		if (name.empty()) continue;
		if (mission::mis_name_allowed(name, mission::kMisDescriptionChars, why)) out.kept.variables[v] = name;
		else out.refused.push_back("V" + std::to_string(v) + "'s name (" + why + ")");
	}
	for (size_t g = 1; g < out.groups.size() && g < mission.group_records.size(); ++g) {
		const std::string &name = out.groups[g];
		bms::GroupRecord probe = mission.group_records[g];
		if (name.empty() || !name_bytes_empty(probe)) continue;
		if (bms::set_group_name(probe, name, why)) out.kept_groups[g] = name;
		else out.refused.push_back("group " + std::to_string(g) + "'s description (" + why + ")");
	}
}

// The locks a matching .mis gives the mission (S25): each locked entity of the file, in its order, to the mission's
// first entity of its SSN not given one yet, over the four pools flat in the writer's order (items, buildings,
// markers, organics); one the mission has none of said in words. The .mis is read with no items.def at hand, so every
// `begin item` lands in the read's item pool whatever its TYPE; an SSN's TYPE puts all its entities in one pool, so
// the n-th of an SSN in the file is the n-th of it in the mission, a building's, a marker's or an organic's as an
// item's.
void take_locks(const bms::File &mission, MissionNamesFile &out) {
	std::vector<int32_t> ids;
	for (const std::vector<bms::Entity> *pool : {&mission.items, &mission.buildings, &mission.markers, &mission.organics})
		for (const bms::Entity &entity : *pool) ids.push_back(entity.id);
	out.kept_locks.assign(ids.size(), 0);
	out.kept_bases.assign(ids.size(), 0);
	std::vector<bool> given(ids.size(), false);
	for (const MissionNamesFile::Lock &lock : out.locks) {
		if (!lock.lock) continue;
		bool found = false;
		for (size_t i = 0; i < ids.size() && !found; ++i)
			if (ids[i] == lock.id && !given[i]) {
				given[i] = true;
				out.kept_locks[i] = lock.lock;
				out.kept_bases[i] = lock.base;
				found = true;
			}
		if (!found)
			out.refused.push_back("SSN " + std::to_string(lock.id) +
			                      "'s height lock (the mission has no entity of that SSN: left unlocked)");
	}
}

// The Set of a row's name field.
Edit set_name(const NodeAddress &address, const char *field, const std::string &value) {
	Edit edit;
	edit.operation = EditOperation::Set;
	edit.address = address;
	edit.field = field;
	edit.value = value;
	return edit;
}

Edit file_value(const char *field, uint64_t fingerprint) {
	Edit edit;
	edit.operation = EditOperation::SetFileValue;
	edit.field = field;
	edit.value = fingerprint_hex(fingerprint);
	return edit;
}

} // namespace

std::string fingerprint_hex(uint64_t fingerprint) {
	static const char digits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 15; i >= 0; --i, fingerprint >>= 4) out[size_t(i)] = digits[fingerprint & 0xF];
	return out;
}

bool fingerprint_from_hex(const std::string &text, uint64_t &out) {
	if (text.size() != 16) return false;
	uint64_t value = 0;
	for (const char ch : text) {
		int digit = -1;
		if (ch >= '0' && ch <= '9') digit = ch - '0';
		else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
		if (digit < 0) return false;
		value = (value << 4) | uint64_t(digit);
	}
	out = value;
	return true;
}

MissionNamesFile read_names_file(const bms::File &mission, const std::vector<mission::EventChain> &events,
                                 const std::vector<uint8_t> &bytes) {
	MissionNamesFile out;
	out.fingerprint = bytes_fingerprint(bytes);
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
	{
		int pool = 0;
		for (const std::vector<bms::Entity> *entities : {&read.items, &read.buildings, &read.markers, &read.organics}) {
			for (const bms::Entity &entity : *entities)
				out.locks.push_back({pool, entity.id, entity.mis_height_lock, entity.mis_extra_bheight});
			++pool;
		}
	}
	mission::RunReport report;
	if (!mission::split_event_chains(read, out.events, report)) {
		out.found = Found::Unreadable;
		out.why = "its events' triggers and actions do not lie in their events' runs";
		return out;
	}
	// The editor's own: what it reads, written again, is the bytes read (a file the original editor wrote says when
	// it saved it in its first line, and writes keys the editor's writer does not). The file read holds the .mis's
	// own frame (mission_mis.h), which the writer writes as it stands, so a file the editor wrote over the terrain it
	// sampled reads as its own whatever the terrain.
	{
		bms::File again = read;
		mission::sync_counts(again);
		std::string written, refused;
		out.ours = mission::write_mis_text(again, written, refused, nullptr, &out.names) && written == text;
	}
	std::set<int32_t> zones;
	for (const bms::AreaTrigger &area : mission.area_triggers) zones.insert(area.id);
	out.why = unmatched_why(out.events, out.names, events, zones);
	if (!out.why.empty()) {
		out.found = Found::Unmatched;
		return out;
	}
	out.found = Found::Matched;
	take_kept(mission, events.size(), out);
	take_locks(mission, out);
	return out;
}

MissionNamesFile read_names_file(const MissionDocument &document, const std::vector<uint8_t> &bytes) {
	bms::File file;
	if (!document.compose(file)) {
		MissionNamesFile out;
		out.found = Found::Unreadable;
		out.fingerprint = bytes_fingerprint(bytes);
		out.why = "the mission could not be composed to read it against";
		return out;
	}
	std::vector<mission::EventChain> events;
	for (const Node *row : document.rows_of(K::Event)) events.push_back(static_cast<const EventRow &>(*row).native);
	return read_names_file(file, events, bytes);
}

std::string names_file_stale(const MissionDocument &document, const MissionNamesFile &file) {
	if (file.found != Found::Matched) return std::string();
	std::vector<mission::EventChain> events;
	std::set<int32_t> zones;
	for (const auto &row : document.rows()) {
		if (row && row->kind == k(K::Event)) events.push_back(static_cast<const EventRow &>(*row).native);
		if (row && row->kind == k(K::Area)) zones.insert(static_cast<const AreaRow &>(*row).native.id);
	}
	return unmatched_why(file.events, file.names, events, zones);
}

mission::MisNames mission_names(const MissionDocument &document) {
	mission::MisNames out;
	std::array<bool, mission::kMisAreaSlots> seen{};
	for (const auto &row : document.rows()) {
		if (!row) continue;
		if (row->kind == k(K::Event)) {
			out.events.push_back(static_cast<const EventRow &>(*row).name);
		} else if (row->kind == k(K::Area)) {
			// The first area of a zone id: the original's slot holds one (a second of the id the writer refuses).
			const AreaRow &area = static_cast<const AreaRow &>(*row);
			const int32_t id = area.native.id;
			if (id > 0 && size_t(id) < out.areas.size() && !seen[size_t(id)]) {
				seen[size_t(id)] = true;
				out.areas[size_t(id)] = area.description;
			}
		} else if (row->kind == k(K::WaypointPath)) {
			const PathRow &path = static_cast<const PathRow &>(*row);
			if (path.native.number > 0 && size_t(path.native.number) < out.paths.size())
				out.paths[size_t(path.native.number)] = path.description;
		}
	}
	if (const auto *state = dynamic_cast<const MissionFileState *>(document.file_state())) out.variables = state->variables;
	return out;
}

bool same_names(const mission::MisNames &a, const mission::MisNames &b) {
	const std::string empty;
	const size_t events = std::max(a.events.size(), b.events.size());
	for (size_t i = 0; i < events; ++i) {
		const std::string &x = i < a.events.size() ? a.events[i] : empty, &y = i < b.events.size() ? b.events[i] : empty;
		if (x != y) return false;
	}
	return a.areas == b.areas && a.paths == b.paths && a.variables == b.variables;
}

bool mission_holds_names(const MissionDocument &document) {
	const mission::MisNames names = mission_names(document);
	const auto any = [](const auto &list) {
		return std::any_of(list.begin(), list.end(), [](const std::string &name) { return !name.empty(); });
	};
	if (any(names.events) || any(names.areas) || any(names.paths) || any(names.variables)) return true;
	if (const MissionRow *mission = document.mission_row()) {
		for (const bms::GroupRecord &group : mission->native.group_records)
			if (!bms::group_name(group).empty()) return true;
	}
	return false;
}

std::vector<int32_t> mission_locks(const MissionDocument &document) {
	std::vector<int32_t> out;
	for (const K kind : {K::Item, K::Building, K::Marker, K::Organic})
		for (const Node *row : document.rows_of(kind)) out.push_back(static_cast<const EntityRow &>(*row).native.mis_height_lock);
	return out;
}

bool same_locks(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
	for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
		if ((i < a.size() ? a[i] : 0) != (i < b.size() ? b[i] : 0)) return false;
	return true;
}

bool mission_holds_locks(const MissionDocument &document) {
	const std::vector<int32_t> locks = mission_locks(document);
	return std::any_of(locks.begin(), locks.end(), [](int32_t lock) { return lock != 0; });
}

bool mission_names_text(const MissionDocument &document, const std::vector<int32_t> *base_heights, std::string &text,
                        std::string &error) {
	bms::File file;
	if (!document.compose(file)) {
		error = "The mission could not be composed.";
		return false;
	}
	const mission::MisNames names = mission_names(document);
	// A locked entity whose base no .mis gave it (locked in the editor since its last write) takes the ground under
	// it as its base, the height handed at this save; from then on its base is what the .mis keeps, which a
	// sculpt leaves (D-MIS-4: the original editor keeps a locked item's base absolute [orig: JOTACmed.exe
	// sub_402770 @ 0x402c9f..0x402ccd]).
	if (base_heights) {
		size_t at = 0;
		for (std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
			for (bms::Entity &entity : *pool) {
				if (entity.mis_height_lock != 0 && entity.mis_extra_bheight == 0 && at < base_heights->size())
					entity.mis_extra_bheight = (*base_heights)[at];
				++at;
			}
	}
	return mission::write_mis_text(file, text, error, base_heights, &names);
}

namespace {

// The edit that gives V`index` the name `name` ("" none): a file-wide value (kMisVariableField).
Edit variable_edit(size_t index, const std::string &name) {
	Edit edit;
	edit.operation = EditOperation::SetFileValue;
	edit.field = kMisVariableField;
	edit.position = index;
	edit.value = name;
	return edit;
}

// The variables' names as the document holds them (none where it holds no file state).
std::array<std::string, mission::kVariableNameCount> held_variables(const MissionDocument &document) {
	if (const auto *state = dynamic_cast<const MissionFileState *>(document.file_state())) return state->variables;
	return {};
}

} // namespace

std::vector<Edit> take_matching_names(const MissionDocument &document, const MissionNamesFile &file) {
	std::vector<Edit> out;
	if (file.found != Found::Unmatched && file.found != Found::Matched) return out;
	std::string refused;
	const std::vector<const Node *> events = document.rows_of(K::Event);
	for (size_t i = 0; i < events.size() && i < file.events.size() && i < file.names.events.size(); ++i) {
		const EventRow &row = static_cast<const EventRow &>(*events[i]);
		const std::string &name = file.names.events[i];
		if (name.empty() || name == row.name || !same_lines(file.events[i], row.native) ||
		    !mission::mis_name_allowed(name, mission::kMisEventNameChars, refused))
			continue;
		out.push_back(set_name({row.id, row.kind, 0}, "name", name));
	}
	std::set<int32_t> seen;
	for (const Node *node : document.rows_of(K::Area)) {
		const AreaRow &row = static_cast<const AreaRow &>(*node);
		const int32_t id = row.native.id;
		if (id <= 0 || size_t(id) >= file.names.areas.size() || !seen.insert(id).second) continue;
		const std::string &name = file.names.areas[size_t(id)];
		if (name.empty() || name == row.description || !mission::mis_name_allowed(name, mission::kMisDescriptionChars, refused))
			continue;
		out.push_back(set_name({row.id, row.kind, 0}, "description", name));
	}
	for (const Node *node : document.rows_of(K::WaypointPath)) {
		const PathRow &row = static_cast<const PathRow &>(*node);
		if (row.native.number <= 0 || size_t(row.native.number) >= file.names.paths.size()) continue;
		const std::string &name = file.names.paths[size_t(row.native.number)];
		if (name.empty() || name == row.description || !mission::mis_name_allowed(name, mission::kMisDescriptionChars, refused))
			continue;
		out.push_back(set_name({row.id, row.kind, 0}, "description", name));
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
				out.push_back(set_name({mission->id, k(K::Group), collection.ids[g]}, "name", name));
			}
		}
	}
	// A variable's name (its number is the mission's own, whatever its events).
	const auto held = held_variables(document);
	for (size_t v = 0; v < file.names.variables.size(); ++v) {
		const std::string &name = file.names.variables[v];
		if (name.empty() || name == held[v] || !mission::mis_name_allowed(name, mission::kMisDescriptionChars, refused))
			continue;
		out.push_back(variable_edit(v, name));
	}
	return out;
}

namespace {

// Read the names again: the rows' names made the ones a matching .mis holds (every name of the rows, those it holds
// none of emptied; a group's where its record names none), and the file-wide value naming those bytes the ones the
// names were read from. How many names change: `changed`.
std::vector<Edit> read_again_edits(const MissionDocument &document, const MissionNamesFile &file, size_t &changed) {
	std::vector<Edit> out;
	std::array<bool, mission::kMisAreaSlots> seen{};
	size_t event = 0;
	for (const auto &node : document.rows()) {
		if (!node) continue;
		if (node->kind == k(K::Event)) {
			const EventRow &row = static_cast<const EventRow &>(*node);
			const std::string name = event < file.kept.events.size() ? file.kept.events[event] : std::string();
			++event;
			if (name != row.name) out.push_back(set_name({row.id, row.kind, 0}, "name", name));
		} else if (node->kind == k(K::Area)) {
			const AreaRow &row = static_cast<const AreaRow &>(*node);
			const int32_t id = row.native.id;
			const bool first = id > 0 && size_t(id) < seen.size() && !seen[size_t(id)];
			if (first) seen[size_t(id)] = true;
			const std::string name = first ? file.kept.areas[size_t(id)] : std::string();
			if (name != row.description) out.push_back(set_name({row.id, row.kind, 0}, "description", name));
		} else if (node->kind == k(K::WaypointPath)) {
			const PathRow &row = static_cast<const PathRow &>(*node);
			if (row.native.number <= 0 || size_t(row.native.number) >= file.kept.paths.size()) continue;
			const std::string &name = file.kept.paths[size_t(row.native.number)];
			if (name != row.description) out.push_back(set_name({row.id, row.kind, 0}, "description", name));
		}
	}
	if (const MissionRow *mission = document.mission_row()) {
		const NodeAddress owner{mission->id, mission->kind, 0};
		for (const Document::Collection &collection : document.collections_of(owner)) {
			if (collection.spec.kind != k(K::Group)) continue;
			for (size_t g = 1; g < collection.ids.size() && g < file.kept_groups.size() && g < mission->native.group_records.size(); ++g)
				if (!file.kept_groups[g].empty() && bms::group_name(mission->native.group_records[g]) != file.kept_groups[g])
					out.push_back(set_name({mission->id, k(K::Group), collection.ids[g]}, "name", file.kept_groups[g]));
		}
	}
	const auto held = held_variables(document);
	for (size_t v = 0; v < held.size(); ++v)
		if (file.kept.variables[v] != held[v]) out.push_back(variable_edit(v, file.kept.variables[v]));
	changed = out.size();
	out.push_back(file_value(kMisKnownField, file.fingerprint));
	return out;
}

// What Write it from the mission writes over, by whose the file there is.
enum class Over { Original, Unmatched, Unreadable, Outside, Own };

std::string write_over_detail(const std::string &mis, Over over) {
	std::string lost;
	switch (over) {
	case Over::Original:
		lost = ", which the original editor wrote: what the editor's writer does not write goes with it (its weapon "
		       "availability, teleport targets, wav list and wind, the areas', paths' and groups' colours and speeds, the "
		       "item keys the .bms has no field for, a group's description past the 20 bytes of its name, and each item's "
		       "base height as the original editor sampled it)";
		break;
	case Over::Unmatched: lost = ", whose names of the events that no longer match go with it"; break;
	case Over::Unreadable: lost = ", which does not read as a mission's text"; break;
	case Over::Outside: lost = ", which another program saved there"; break;
	case Over::Own: break;
	}
	return "The next Save writes " + mis + " from the mission, its names as the mission holds them, over what the file "
	       "holds now" + lost + ". An edit of the mission, which Undo takes back until then; once written, a later "
	       "change to the file is left as it is again.";
}

} // namespace

void names_file_findings(const MissionDocument &document, std::vector<Diagnostic> &out) {
	const MissionNamesFile &file = document.names_file();
	if (file.found == Found::Unread) return;
	const MissionNamesLeft &left = document.names_left();
	const MissionNamesFile &outside = document.names_outside_file();
	const std::string mis = mission::mission_base_name(basename_of(document.path())) + ".mis";
	const uint64_t overwrite = document.names_file_overwrite();
	const bool unsaved = document.companion_unsaved();
	// What a save that leaves the file means for the names and the height locks the mission holds.
	const bool names_differ = !same_names(mission_names(document), file.kept);
	const bool locks_differ = !same_locks(mission_locks(document), file.kept_locks);
	const std::string held = names_differ && locks_differ ? "The names and the height locks"
	                         : locks_differ                ? "The height locks"
	                                                        : "The names";
	const std::string kept = unsaved ? " " + held + " the mission holds are not in it: they stay unsaved until it is "
	                                   "written, and the unsaved prompt asks before the mission is closed."
	                                 : std::string();
	// Write it from the mission: the bytes a save then writes over.
	const auto write_over = [&](Diagnostic &d, uint64_t fingerprint, Over over) {
		if (fingerprint == 0 || overwrite == fingerprint) return;
		d.planned.push_back({"Write it from the mission", write_over_detail(mis, over), {file_value(kMisOverwriteField, fingerprint)}});
	};
	const auto take = [&](Diagnostic &d) {
		std::vector<Edit> edits = take_matching_names(document, file);
		if (edits.empty()) return;
		const size_t count = edits.size();
		d.planned.push_back({"Take the names that still match",
		                     "Gives the mission the " + counted(count, "name", "names") + " of " + mis +
		                     " whose records the mission still holds alike: an event's where its triggers and actions are "
		                     "the same, an area's by its zone id, a path's, a group's where the mission names it none. An "
		                     "edit of the mission, one step Undo takes back; the file is written over only by Write it from "
		                     "the mission.",
		                     std::move(edits)});
	};
	if (left.outside && outside.found != Found::Unread && outside.fingerprint != file.fingerprint &&
	    overwrite != outside.fingerprint) {
		std::string said = mis + " changed outside the editor since the editor read or wrote it: the last Save left it as it is.";
		if (outside.found == Found::Unmatched) said += " It holds other events than the mission now (" + outside.why + ").";
		if (outside.found == Found::Unreadable) said += " It does not read as a mission's text now (" + outside.why + ").";
		Diagnostic d = make_finding(MissionFinding::NamesOutside, DiagnosticSeverity::Warning, said + kept, document.path());
		if (outside.found == Found::Matched) {
			size_t changed = 0;
			std::vector<Edit> edits = read_again_edits(document, outside, changed);
			d.planned.push_back({"Read the names again",
			                     "Gives the mission the names " + mis + " holds now, as the other program saved them, in "
			                     "place of those it holds (" + counted(changed, "name changes", "names change") +
			                     "). An edit of the mission, one step Undo takes back.",
			                     std::move(edits)});
		}
		write_over(d, outside.fingerprint,
		           outside.found == Found::Unreadable ? Over::Unreadable
		           : outside.ours || outside.found == Found::Unopened ? Over::Outside
		                                                               : Over::Original);
		out.push_back(std::move(d));
	} else if ((file.found == Found::Unmatched || file.found == Found::Unreadable || file.found == Found::Unopened) &&
	           (file.fingerprint == 0 || overwrite != file.fingerprint)) {
		std::string said;
		switch (file.found) {
		case Found::Unmatched: said = mis + " holds another mission's events (" + file.why + ")"; break;
		case Found::Unreadable: said = mis + " does not read as a mission's text (" + file.why + ")"; break;
		default: said = mis + " could not be read (" + file.why + ")"; break;
		}
		Diagnostic d = make_finding(MissionFinding::NamesUnmatched, DiagnosticSeverity::Warning,
		                            said + ": the editor took no name from it, and Save leaves it as it is." + kept,
		                            document.path());
		if (file.found == Found::Unmatched) take(d);
		if (file.found != Found::Unopened)
			write_over(d, file.fingerprint,
			           file.found == Found::Unreadable ? Over::Unreadable : file.ours ? Over::Unmatched : Over::Original);
		out.push_back(std::move(d));
	} else if (!left.stale.empty() && overwrite != file.fingerprint) {
		// The mission changed under the file a save left (an event's lines, an area's zone id): the next open takes
		// none of its names.
		Diagnostic d = make_finding(MissionFinding::NamesUnmatched, DiagnosticSeverity::Warning,
		                            mis + " no longer matches the mission (" + left.stale + "): the last Save left it as "
		                                  "it is, so the mission's next open takes none of its names." + kept,
		                            document.path());
		write_over(d, file.fingerprint, file.ours ? Over::Own : Over::Original);
		out.push_back(std::move(d));
	} else if (file.found == Found::Matched && !file.ours && unsaved && overwrite != file.fingerprint) {
		Diagnostic d = make_finding(MissionFinding::NamesForeign, DiagnosticSeverity::Warning,
		                            mis + " is the original editor's (it writes what the editor's writer does not): Save "
		                                  "leaves it as it is." + kept,
		                            document.path());
		write_over(d, file.fingerprint, Over::Original);
		out.push_back(std::move(d));
	}
	if (!left.unwritten.empty() && unsaved)
		out.push_back(make_finding(MissionFinding::NamesUnwritten, DiagnosticSeverity::Warning,
		                           mis + " was not written: " + sentence(left.unwritten) + " The mission's own file was saved." + kept,
		                           document.path()));
	if (!left.locked.empty()) {
		// D-MIS-17: no height under them known, so written locked at their heights, which the original editor reads
		// exact over whatever terrain it loads (a note: the save stands).
		// The first titles alone, the rest counted (a mission of thousands on a terrain that does not read).
		const size_t more = left.locked_count > left.locked.size() ? left.locked_count - left.locked.size() : 0;
		std::string what;
		for (size_t i = 0; i < left.locked.size(); ++i)
			what += (i == 0 ? "" : i + 1 == left.locked.size() && !more ? " and " : ", ") + left.locked[i];
		if (more) what += " and " + std::to_string(more) + " more";
		out.push_back(make_finding(MissionFinding::NamesLocked, DiagnosticSeverity::Info,
		                           mis + " was written with " + counted(left.locked_count, "entity", "entities") +
		                                   " height locked at " + (left.locked_count == 1 ? "its height" : "their heights") +
		                                   " (" + what + "): the mission's terrain did not read, so the height of the "
		                                   "ground under them was not known. The original editor reads them at these "
		                                   "heights whatever terrain it loads; once the terrain reads, a save writes them "
		                                   "unlocked again.",
		                           document.path()));
	}
	if (!file.refused.empty()) {
		std::string what;
		for (size_t i = 0; i < file.refused.size(); ++i)
			what += (i == 0 ? "" : i + 1 == file.refused.size() ? " and " : ", ") + file.refused[i];
		out.push_back(make_finding(MissionFinding::NamesRefused, DiagnosticSeverity::Warning,
		                           mis + " names what the mission does not take, which the editor left out: " + what + ".",
		                           document.path()));
	}
}

} // namespace opennova::editor
