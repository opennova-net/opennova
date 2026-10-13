#include "mission_document.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/documents/mission_file_set.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_variable_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/diagnostic.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <formats/def/reserved_items.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {

using namespace mission;

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

// The pool an entity kind's records are in (bms_edit's EntityKind).
EntityKind pool_of(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Building: return EntityKind::Building;
	case K::Marker: return EntityKind::Marker;
	case K::Organic: return EntityKind::Organic;
	default: return EntityKind::Item;
	}
}

// The mission's own text table as the graph scopes it ("<BASE>.BIN"), and the one the game loads in
// its place where the mission has none ("MEDMSSN.BIN"): the by-name table's text row [orig:
// TextResource_LoadMissionTextBin @0x51ed90; runtime/mission/mission_sidecars.h].
std::string mission_text_table(const std::string &path) {
	return strutil::to_upper(mission::sidecar_name(basename_of(path), *mission::sidecar_for_role("text")));
}
std::string mission_text_fallback() { return strutil::to_upper(mission::sidecar_for_role("text")->fallback); }

// The lowest zone id in 1..99 no area trigger row holds [orig editor: dfx2med
// Med_AreaTriggerDialogProc @0x40f400 lists zones 1..99]; 0 with none free.
int free_zone_id(const std::vector<std::shared_ptr<const Node>> &rows) {
	bool taken[100] = {};
	for (const auto &row : rows)
		if (row->kind == k(K::Area)) {
			const int32_t id = static_cast<const AreaRow &>(*row).native.id;
			if (id >= 1 && id <= 99) taken[id] = true;
		}
	for (int id = 1; id <= 99; ++id)
		if (!taken[id]) return id;
	return 0;
}

// A parameter's slot (0 for param1 .. 3 for param4), -1 for another field.
int param_slot(const std::string &id) {
	return id.size() == 6 && id.compare(0, 5, "param") == 0 && id[5] >= '1' && id[5] <= '4' ? id[5] - '1' : -1;
}

std::vector<FieldChoice> choices_of(const MissionChoices &rows) {
	std::vector<FieldChoice> out;
	out.reserve(rows.count);
	for (size_t i = 0; i < rows.count; ++i) out.push_back({rows.rows[i].name, rows.rows[i].value, ""});
	return out;
}

// A row's index among the rows of its kind (the file's order, which a Record reference's index counts).
size_t index_among(const std::vector<std::shared_ptr<const Node>> &rows, const Node *row) {
	size_t index = 0;
	for (const auto &other : rows) {
		if (other.get() == row) return index;
		if (other->kind == row->kind) ++index;
	}
	return SIZE_MAX;
}

// The number each of a path's stops gives its marker (its wp_number), the stops in their order. A marker's
// number is the key the original editor sorts a path's markers by, not its place: its writer copies the
// number the marker holds [orig: JOTACmed.exe sub_44C8E0 @ 0x44c9ff, item +0x90 to the record's +0x30] and
// its rebuild only sorts each path's markers by it [orig: sub_44CFD0 @ 0x44d0e9..0x44d104], never writing
// it, so a shipped path holds keys (00TRa's path 1 holds 0, 1, 2, 2, 3 and 03TR's path 5 1 to 9 and 1648).
// A stop keeps the number its marker was read with on this path (`held`: each marker's path and number as
// read) while the stops' order is the order those numbers sort to (mission::waypoint_path_markers': by
// number, a tie by the marker's place in the file); the stops an edit put in or moved take the numbers
// after the kept stop before them, one apart, and as few stops are renumbered as keeps the order (a kept
// stop with no room before it for the new ones is renumbered too). A path no kept stop orders is numbered
// from 0 by place; no number goes below 0 that the file did not hold.
std::vector<int32_t> stop_numbers(const std::vector<uint32_t> &stops, const std::vector<std::pair<int, int32_t>> &held,
                                  size_t path) {
	const size_t n = stops.size();
	std::vector<int64_t> key(n, 0);
	std::vector<bool> keeps(n, false);
	for (size_t i = 0; i < n; ++i)
		if (stops[i] < held.size() && size_t(held[stops[i]].first) == path) {
			keeps[i] = true;
			key[i] = held[stops[i]].second;
		}
	// Whether stop b may keep its number after kept stop a: next to it, sorting after it; else with room
	// for the stops between, one apart.
	const auto fits = [&](size_t a, size_t b) {
		if (b == a + 1) return key[a] < key[b] || (key[a] == key[b] && stops[a] < stops[b]);
		return key[b] - key[a] >= int64_t(b - a);
	};
	constexpr size_t kNone = SIZE_MAX;
	std::vector<size_t> best(n, 0), before(n, kNone);
	size_t last = kNone;
	for (size_t b = 0; b < n; ++b) {
		if (!keeps[b]) continue;
		// The first kept: the stops before it take the numbers below its own, none below 0.
		if (b == 0 || key[b] - int64_t(b) >= 0) best[b] = 1;
		for (size_t a = 0; a < b; ++a)
			if (keeps[a] && best[a] && best[a] + 1 > best[b] && fits(a, b)) {
				best[b] = best[a] + 1;
				before[b] = a;
			}
		// The last kept: the stops after it take the numbers above its own.
		if (best[b] && key[b] + int64_t(n - 1 - b) <= INT32_MAX && (last == kNone || best[b] > best[last])) last = b;
	}
	std::vector<int32_t> out(n, 0);
	if (last == kNone) {
		for (size_t i = 0; i < n; ++i) out[i] = int32_t(i);
		return out;
	}
	std::vector<bool> kept(n, false);
	for (size_t at = last; at != kNone; at = before[at]) kept[at] = true;
	size_t first = last;
	while (before[first] != kNone) first = before[first];
	for (size_t i = 0; i < first; ++i) out[i] = int32_t(key[first] - int64_t(first - i));
	int64_t number = 0;
	for (size_t i = first; i < n; ++i) {
		number = kept[i] ? key[i] : number + 1;
		out[i] = int32_t(number);
	}
	return out;
}

// The file's paths laid out from the stops the rows hold (`stops`, one list a path, in the paths' order), as
// the original editor lays a path out from its markers (D-MIS-6): each waypoint marker carries the path
// it is a stop of and its number there (stop_numbers), none another, and each path's record is laid out
// from them (mission::lay_out_waypoint_path) [orig: JOTACmed.exe sub_44C8E0 @ 0x44c8e0, the path @ 0x44c9f5
// and the number @ 0x44c9ff; sub_44CFD0 @ 0x44cfd0; sub_44F920 @ 0x44f920]. A stop that names no waypoint
// marker, or one of path 0, puts nothing on a path, and a marker of two stops keeps the last
// (mission.unserializable says each, which refuses the save).
void lay_out_paths(bms::File &file, const std::vector<const std::vector<uint32_t> *> &stops) {
	std::vector<std::pair<int, int32_t>> held(file.markers.size());
	for (size_t i = 0; i < file.markers.size(); ++i)
		held[i] = {file.markers[i].type_id == def::DEF_TYPE_WAYPOINT ? int(file.markers[i].waypoint_id) : -1,
		           file.markers[i].wp_number};
	for (bms::Entity &marker : file.markers)
		if (marker.type_id == def::DEF_TYPE_WAYPOINT && marker.waypoint_id != 0 &&
		    size_t(marker.waypoint_id) < file.waypoint_records.size()) {
			marker.waypoint_id = 0;
			marker.wp_number = 0;
		}
	for (size_t path = 1; path < stops.size() && path < file.waypoint_records.size(); ++path) {
		const std::vector<int32_t> numbers = stop_numbers(*stops[path], held, path);
		for (size_t place = 0; place < stops[path]->size(); ++place) {
			const uint32_t index = (*stops[path])[place];
			if (index >= file.markers.size() || file.markers[index].type_id != def::DEF_TYPE_WAYPOINT) continue;
			file.markers[index].waypoint_id = static_cast<uint8_t>(path);
			file.markers[index].wp_number = numbers[place];
		}
	}
	for (size_t path = 0; path < file.waypoint_records.size(); ++path) mission::lay_out_waypoint_path(file, path);
}

// Each marker's path and number as its row holds them (a marker of another type: no path), the markers in
// their order: what stop_numbers keeps.
std::vector<std::pair<int, int32_t>> held_numbers(const std::vector<std::shared_ptr<const Node>> &rows) {
	std::vector<std::pair<int, int32_t>> held;
	for (const auto &row : rows)
		if (row && row->kind == k(K::Marker)) {
			const bms::Entity &marker = static_cast<const EntityRow &>(*row).native;
			held.emplace_back(marker.type_id == def::DEF_TYPE_WAYPOINT ? int(marker.waypoint_id) : -1, marker.wp_number);
		}
	return held;
}

} // namespace

// --- the rows ----------------------------------------------------------------------------------------

template <> std::string MissionRow::name() const { return native.get_mission_name(); }
template <> std::string EntityRow::name() const { return std::to_string(native.id); }
template <> std::string MissionRecordRow<MissionPath>::name() const {
	if (native.number == 0) return "None";
	if (const char *command = path_command_editor_name(native.number)) return command;
	return std::to_string(native.number);
}
template <> std::string MissionRecordRow<bms::AreaTrigger>::name() const { return "Zone " + std::to_string(native.id); }
template <> std::string MissionRecordRow<mission::EventChain>::name() const { return std::string(); }

template <> size_t MissionRow::footprint() const {
	size_t bytes = sizeof(MissionRow) + ids_footprint() + footprint_of(native.loadout.entries) +
	               footprint_of(native.item_availability) + footprint_of(native.group_records) +
	               footprint_of(native.wav_list_records) + footprint_of(native.bounding_boxes);
	for (const bms::WeaponLoadoutRecord &entry : native.loadout.entries)
		bytes += footprint_of(entry.name) + footprint_of(entry.ammo_primary) + footprint_of(entry.ammo_secondary) +
		         footprint_of(entry.flags);
	for (const bms::ItemAvailabilityEntry &entry : native.item_availability) bytes += footprint_of(entry.name);
	return bytes;
}
template <> size_t EntityRow::footprint() const { return sizeof(EntityRow) + ids_footprint(); }
template <> size_t MissionRecordRow<MissionPath>::footprint() const {
	return sizeof(PathRow) + ids_footprint() + footprint_of(native.record.waypoint_numbers) +
	       footprint_of(native.record.padding) + footprint_of(native.stops);
}
template <> size_t MissionRecordRow<bms::AreaTrigger>::footprint() const { return sizeof(AreaRow) + ids_footprint(); }
template <> size_t MissionRecordRow<mission::EventChain>::footprint() const {
	return sizeof(EventRow) + ids_footprint() + footprint_of(native.triggers) + footprint_of(native.actions);
}

const std::string *mission_row_name(const Node &row) {
	switch (static_cast<K>(row.kind)) {
	case K::Event: return &static_cast<const EventRow &>(row).name;
	case K::Area: return &static_cast<const AreaRow &>(row).description;
	case K::WaypointPath: return &static_cast<const PathRow &>(row).description;
	default: return nullptr;
	}
}
std::string *mission_row_name(Node &row) {
	return const_cast<std::string *>(mission_row_name(static_cast<const Node &>(row)));
}

const char *mission_name_field(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Event: return "name";
	case K::Area:
	case K::WaypointPath: return "description";
	default: return nullptr;
	}
}

size_t mission_name_width(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Event: return mission::kMisEventNameChars;
	case K::Area:
	case K::WaypointPath: return mission::kMisDescriptionChars;
	default: return 0;
	}
}

bool is_mission_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Mission; }

int32_t next_free_ssn(const std::vector<std::shared_ptr<const Node>> &rows) {
	int32_t largest = 0;
	for (const auto &row : rows)
		if (is_entity_kind(row->kind)) largest = std::max(largest, static_cast<const EntityRow &>(*row).native.id);
	return ssn_after(largest);
}

int entity_pool_of(NodeKind kind) { return is_entity_kind(kind) ? entity_pool(pool_of(kind)) : -1; }

std::string mission_scope(const DocumentBase &document) { return strutil::to_upper(basename_of(document.path())); }

std::string mission_event_scope(const std::string &mission) {
	return strutil::to_upper(basename_of(mission)) + "/" + kMissionEventSection;
}

namespace {

// The header's dialog bank slot as read (its second terrain slot, the original editor's cnv_file; "" none).
std::string dialog_slot_of(const MissionDocument &document) {
	const MissionRow *header = document.mission_row();
	return header ? strutil::fixed_string(header->native.header.terrain + 16, 16) : std::string();
}

} // namespace

std::string mission_dialog_bank(const MissionDocument &document) {
	return mission::dialog_bank_name(basename_of(document.path()), dialog_slot_of(document));
}

// --- the document ------------------------------------------------------------------------------------

const MissionRow *MissionDocument::mission_row() const {
	for (const auto &row : rows())
		if (row && row->kind == k(K::Mission)) return static_cast<const MissionRow *>(row.get());
	return nullptr;
}

std::vector<const Node *> MissionDocument::rows_of(MissionKind kind) const {
	std::vector<const Node *> out;
	for (const auto &row : rows())
		if (row && row->kind == k(kind)) out.push_back(row.get());
	return out;
}

NodeId MissionDocument::entity_holder(int64_t ssn) const {
	if (ssn < INT32_MIN || ssn > INT32_MAX) return 0;
	const Lookups &first = lookups();
	const auto found = first.ssns.find(int32_t(ssn));
	return found == first.ssns.end() ? 0 : found->second;
}

NodeId MissionDocument::zone_holder(int64_t id) const {
	if (id < INT32_MIN || id > INT32_MAX) return 0;
	const Lookups &first = lookups();
	const auto found = first.zones.find(int32_t(id));
	return found == first.zones.end() ? 0 : found->second;
}

const Node *MissionDocument::row_of(MissionKind kind, size_t index) const {
	const Lookups &made = lookups();
	const size_t at = size_t(kind);
	return at < made.by_kind.size() && index < made.by_kind[at].size() ? made.by_kind[at][index] : nullptr;
}

size_t MissionDocument::index_of(const Node &row) const {
	const Lookups &made = lookups();
	const auto found = made.places.find(row.id);
	return found == made.places.end() ? SIZE_MAX : found->second;
}

bool MissionDocument::on_player_route(const Node &row) const { return lookups().route.count(row.id) != 0; }

NodeId MissionDocument::numbered_holder(int64_t number) const {
	const Lookups &made = lookups();
	if (number < INT32_MIN || number > INT32_MAX) return 0;
	const auto found = made.numbered.find(int32_t(number));
	return found == made.numbered.end() ? 0 : found->second;
}

const std::vector<MissionDocument::StopPlace> &MissionDocument::stops_of(const Node &marker) const {
	static const std::vector<StopPlace> kNone;
	const Lookups &made = lookups();
	const auto found = made.stops.find(marker.id);
	return found == made.stops.end() ? kNone : found->second;
}

size_t MissionDocument::count_of(MissionKind kind) const {
	const Lookups &made = lookups();
	return size_t(kind) < made.by_kind.size() ? made.by_kind[size_t(kind)].size() : 0;
}

size_t MissionDocument::group_members(int64_t group) const {
	const Lookups &made = lookups();
	return group >= 0 && size_t(group) < made.groups.size() ? made.groups[size_t(group)] : 0;
}

std::string MissionDocument::record_title(const NodeAddress &address) const {
	std::string title = mission_record_label(*this, address, nullptr);
	return title.empty() ? TableDocument::record_title(address) : title;
}

const std::string *MissionDocument::item_class(const bms::Entity &entity) const {
	if (!item_classes_) return nullptr;
	const auto found = item_classes_->find(mission::entity_item_id(entity));
	return found == item_classes_->end() ? nullptr : &found->second;
}

bool MissionDocument::compose(bms::File &out) const {
	if (!compose_mission(rows(), out)) return false;
	// Each record's AI class its item's row's, as the original's writer copies it over the record on every save,
	// zero-filled past it [orig: JOTACmed.exe sub_44C8E0 @ 0x44c8e0, the memset; @ 0x44cabe..0x44caf2].
	for (std::vector<bms::Entity> *pool : {&out.items, &out.buildings, &out.markers, &out.organics})
		for (bms::Entity &entity : *pool)
			if (const std::string *ai_class = item_class(entity)) {
				std::memset(entity.name1, 0, sizeof(entity.name1));
				std::memcpy(entity.name1, ai_class->data(), std::min(ai_class->size(), sizeof(entity.name1)));
			}
	return true;
}

bool compose_mission(const std::vector<std::shared_ptr<const Node>> &rows, bms::File &out) {
	const MissionRow *mission = nullptr;
	for (const auto &row : rows)
		if (row && row->kind == k(K::Mission)) mission = static_cast<const MissionRow *>(row.get());
	if (!mission) return false;
	// The mission row's file holds its header and its own tables; every other record is its row's.
	out = mission->native;
	out.items.clear();
	out.buildings.clear();
	out.markers.clear();
	out.organics.clear();
	out.waypoint_records.clear();
	out.area_triggers.clear();
	std::vector<EventChain> chains;
	std::vector<const std::vector<uint32_t> *> stops;
	for (const auto &row : rows) {
		if (!row) continue;
		switch (static_cast<K>(row->kind)) {
		case K::Item: out.items.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Building: out.buildings.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Marker: out.markers.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Organic: out.organics.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::WaypointPath:
			out.waypoint_records.push_back(static_cast<const PathRow &>(*row).native.record);
			stops.push_back(&static_cast<const PathRow &>(*row).native.stops);
			break;
		case K::Area: out.area_triggers.push_back(static_cast<const AreaRow &>(*row).native); break;
		case K::Event: chains.push_back(static_cast<const EventRow &>(*row).native); break;
		default: break;
		}
	}
	join_event_chains(chains, out);
	lay_out_paths(out, stops);
	sync_counts(out);
	return true;
}

// The first of the paths' stops no save can write (D-MIS-6), in words; "" for none: a stop of path 0, one
// naming no waypoint marker of the file, a marker two stops name (mission.unserializable says each).
std::string unwritable_stop(const MissionDocument &document) {
	const std::vector<const Node *> markers = document.rows_of(K::Marker);
	std::set<uint32_t> carried;
	for (const Node *row : document.rows_of(K::WaypointPath)) {
		const MissionPath &path = static_cast<const PathRow &>(*row).native;
		for (const uint32_t stop : path.stops) {
			const std::string where = "Path " + std::to_string(path.number) + "'s stop naming marker " + std::to_string(stop);
			if (path.number == 0) return where + ": path 0 is no path.";
			if (stop >= markers.size() || static_cast<const EntityRow &>(*markers[stop]).native.type_id != def::DEF_TYPE_WAYPOINT)
				return where + ": no waypoint marker of the mission is that one.";
			if (!carried.insert(stop).second) return where + ": another stop names that marker.";
		}
	}
	return std::string();
}

SerializeResult MissionDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const SourceIssue &issue : issues())
			if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	// A path's stops are its waypoint markers' in the file: one no marker can carry is not written.
	if (const std::string why = unwritable_stop(*this); !why.empty()) {
		SourceIssue issue;
		issue.message = why + " A save puts each stop on its path through its waypoint marker (mission.unserializable).";
		result.issues.push_back(std::move(issue));
		return result;
	}
	bms::File file;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!compose(file) || !bms::write(file, bytes, error)) {
		SourceIssue issue;
		issue.message = error.empty() ? "The mission could not be written." : error;
		result.issues.push_back(std::move(issue));
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

bool MissionDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                            std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
                            Diagnostic &error) {
	if (!is_mission_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a mission.", path());
		return false;
	}
	bms::File file;
	std::string message;
	if (!bms::parse(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, message, path());
		return false;
	}
	std::vector<MissionFinding> codes;
	const auto note = [&](MissionFinding code, bool blocks, std::string text) {
		SourceIssue issue;
		issue.blocks = blocks;
		issue.message = std::move(text);
		issues.push_back(std::move(issue));
		codes.push_back(code);
	};
	// The events' runs as chains. A file the chains cannot hold blocks (its tables are not what the
	// rows would write back); one laid out otherwise than the events stand loses only its layout.
	std::vector<EventChain> chains;
	RunReport report;
	const bool split = split_event_chains(file, chains, report);
	if (!split) {
		const auto holds = [](RunLayout layout) { return layout == RunLayout::Canonical || layout == RunLayout::Reordered; };
		const bool in_triggers = !holds(report.triggers);
		const RunLayout layout = in_triggers ? report.triggers : report.actions;
		const std::string table = in_triggers ? "trigger" : "action";
		const std::string what = layout == RunLayout::OutOfRange ? "an event's " + table + "s run past the " + table + " table"
		                         : layout == RunLayout::Shared  ? "a " + table + " lies in two events' runs"
		                                                        : "a " + table + " lies in no event's run";
		const bool past = layout == RunLayout::OutOfRange;
		note(past ? MissionFinding::RunsPastTable : MissionFinding::InvalidInput, true,
		     "The mission's events cannot be edited: " + what +
		             " (the game reads each event's run by its first index and its count)." +
		             (past ? std::string()
		                   : std::string(" The game reads each run as written, ") +
		                             (layout == RunLayout::Shared ? "resolving the record in both"
		                                                          : "never reading the record no run holds") +
		                             " [orig: EventTrigger_LoadAllData @ 0x453eb0]: a build packs the mission as it stands."));
		if (report.event >= 0) {
			const size_t first_event_row = 1 + file.items.size() + file.buildings.size() + file.markers.size() +
			                               file.organics.size() + size_t(bms::kWaypointRecordCount) +
			                               file.area_triggers.size();
			issues.back().record = "Event " + std::to_string(report.event + 1);
			issues.back().locator = std::to_string(first_event_row + size_t(report.event));
		}
		// The events stand, their chains empty: the document is blocked, nothing writes them.
		chains.clear();
		for (const bms::Event &event : file.events) chains.push_back({event, {}, {}});
	} else if (!report.canonical()) {
		note(MissionFinding::EventOrder, false,
		     "The mission's triggers and actions are not laid out as its events stand: Save lays them out that "
		     "way, each event's run where the event is (the game reads either alike).");
	}
	// What the writer would write against the bytes read: a difference the game reads alike (a
	// loadout chunk the game's sanitizer repairs, bytes past a table's records).
	std::vector<uint8_t> written;
	if (split && report.canonical() && bms::write(file, written, message) && written != bytes) {
		const std::string section = bms::first_differing_section(file, bytes, written);
		note(MissionFinding::RewriteDiffers, false,
		     section == "length" ? std::string("The file holds bytes past what the writer writes: Save drops them.")
		                         : "The file's " + section + " holds bytes the writer writes otherwise: Save writes " +
		                                   "the section as the game reads it.");
	}
	// The paths whose records name other stops than their markers carry (no shipped mission has one; a file
	// an earlier OpenNova wrote, whose stop edits wrote the record alone): the game walks the record's
	// [orig: AIWaypoint_UpdateTarget @0x457476], the editor holds the markers' as the original editor does,
	// and a save lays each record out from them, so the route the game walks changes (a warning, naming them).
	{
		bms::File laid = file;
		std::vector<size_t> rebuilt;
		for (size_t i = 0; i < laid.waypoint_records.size(); ++i) {
			mission::lay_out_waypoint_path(laid, i);
			const bms::WaypointRecord &was = file.waypoint_records[i], &now = laid.waypoint_records[i];
			const size_t slots = std::min<size_t>(now.waypoint_numbers.size(), was.waypoint_numbers.size());
			if (was.marker_count == now.marker_count && was.waypoint_numbers.size() == now.waypoint_numbers.size() &&
			    std::equal(now.waypoint_numbers.begin(), now.waypoint_numbers.begin() + std::ptrdiff_t(slots),
			               was.waypoint_numbers.begin()))
				continue;
			rebuilt.push_back(i);
		}
		if (!rebuilt.empty()) {
			std::string paths;
			for (size_t i = 0; i < rebuilt.size(); ++i)
				paths += (i == 0 ? "" : i + 1 == rebuilt.size() ? " and " : ", ") + std::to_string(rebuilt[i]);
			const bool one = rebuilt.size() == 1;
			note(MissionFinding::PathRebuilt, false,
			     std::string(one ? "Path " : "Paths ") + paths + (one ? "'s record lists" : "' records list") +
			             " other stops than the waypoint markers carry (a file an earlier OpenNova wrote): the game walks "
			             "the record's, the editor holds the markers' as the original editor does, and Save lays " +
			             (one ? "the record" : "each record") + " out from them, so the route the game walks changes.");
		}
	}
	issue_codes_ = std::move(codes);

	// The .mis the session read before this read of the file (S24 Names): what it holds against the mission; where
	// it matches, its names are the rows' and its group descriptions the names of the groups the .bms holds none
	// of (the original editor's .bms writer writes them as zeros, bms.h GroupRecord). A read the session handed
	// none to (a save's read back, one from bytes) leaves what the last one found.
	const bool names_read = names_file_pending_;
	MissionNamesFile names;
	if (names_read) {
		const CompanionFile *beside = pending_names_file_.get();
		if (beside && beside->state == CompanionFile::State::Read) {
			names = read_names_file(file, chains, beside->bytes);
		} else if (beside && beside->state == CompanionFile::State::Unreadable) {
			names.found = MissionNamesFile::Found::Unopened;
			names.why = beside->error;
		} else {
			names.found = MissionNamesFile::Found::None;
		}
		names_file_pending_ = false;
		pending_names_file_.reset();
	}
	// What the mission takes of a matching one (MissionNamesFile::kept: what the setters take; the rest listed).
	const bool take = names.found == MissionNamesFile::Found::Matched;
	if (take)
		for (size_t g = 1; g < file.group_records.size() && g < names.kept_groups.size(); ++g) {
			std::string refused;
			if (!names.kept_groups[g].empty()) bms::set_group_name(file.group_records[g], names.kept_groups[g], refused);
		}

	auto mission = std::make_shared<MissionRow>(k(K::Mission));
	mission->native = file;
	mission->native.items.clear();
	mission->native.buildings.clear();
	mission->native.markers.clear();
	mission->native.organics.clear();
	mission->native.waypoint_records.clear();
	mission->native.area_triggers.clear();
	mission->native.events.clear();
	mission->native.triggers.clear();
	mission->native.actions.clear();
	shape(*mission);
	rows.push_back(mission);
	// Each entity's height lock, the one a matching .mis gives it (S25; MissionNamesFile::kept_locks, in the writer's
	// order), else none (the .bms has no room for one).
	size_t entity_at = 0;
	const auto entities = [&](K kind, const std::vector<bms::Entity> &records) {
		for (const bms::Entity &record : records) {
			auto row = std::make_shared<EntityRow>(k(kind), record);
			if (take) {
				row->native.mis_height_lock = entity_at < names.kept_locks.size() ? names.kept_locks[entity_at] : 0;
				// A locked entity's base as the .mis keeps it, absolute (D-MIS-4).
				if (row->native.mis_height_lock != 0 && entity_at < names.kept_bases.size())
					row->native.mis_extra_bheight = names.kept_bases[entity_at];
			}
			++entity_at;
			shape(*row);
			rows.push_back(row);
		}
	};
	entities(K::Item, file.items);
	entities(K::Building, file.buildings);
	entities(K::Marker, file.markers);
	entities(K::Organic, file.organics);
	for (size_t i = 0; i < file.waypoint_records.size(); ++i) {
		MissionPath path{file.waypoint_records[i], int(i), {}};
		for (const int marker : mission::waypoint_path_markers(file, i)) path.stops.push_back(uint32_t(marker));
		auto row = std::make_shared<PathRow>(k(K::WaypointPath), std::move(path));
		if (take && i < names.kept.paths.size()) row->description = names.kept.paths[i];
		shape(*row);
		rows.push_back(row);
	}
	std::set<int32_t> described; // an area of a zone id the first of it the .mis's description goes to
	for (const bms::AreaTrigger &area : file.area_triggers) {
		auto row = std::make_shared<AreaRow>(k(K::Area), area);
		if (take && area.id > 0 && size_t(area.id) < names.kept.areas.size() && described.insert(area.id).second)
			row->description = names.kept.areas[size_t(area.id)];
		shape(*row);
		rows.push_back(row);
	}
	for (size_t i = 0; i < chains.size(); ++i) {
		auto row = std::make_shared<EventRow>(k(K::Event), std::move(chains[i]));
		if (take && i < names.kept.events.size()) row->name = names.kept.events[i];
		shape(*row);
		rows.push_back(row);
	}
	// The variables' names a matching .mis gives the mission (its `//var` lines, mission_mis.h kMisVariableLine):
	// the document's own, no .bms byte holding them (MissionFileState::variables).
	if (take && std::any_of(names.kept.variables.begin(), names.kept.variables.end(),
	                        [](const std::string &name) { return !name.empty(); })) {
		auto made = std::make_shared<MissionFileState>();
		made->variables = names.kept.variables;
		state = std::move(made);
	}
	if (names_read) {
		names_file_ = std::move(names);
		outside_names_ = MissionNamesFile();
		names_left_ = MissionNamesLeft();
		spent_overwrite_ = 0;
	}
	return true;
}

// --- the .mis beside it ------------------------------------------------------------------------------------

namespace {

uint64_t bytes_fingerprint(const std::vector<uint8_t> &bytes) {
	return io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size());
}

const MissionFileState *mission_file_state(const FileState *state) { return dynamic_cast<const MissionFileState *>(state); }

} // namespace

void MissionDocument::take_names_file(const CompanionFile &file) {
	++names_state_;
	names_file_pending_ = true;
	pending_names_file_ = std::make_shared<const CompanionFile>(file);
}

const MissionNamesFile &MissionDocument::names_file() const {
	// Read the names again named the bytes the last save found in its place: those are the mission's now.
	const MissionFileState *state = mission_file_state(file_state());
	if (state && state->mis_known != 0 && outside_names_.found != MissionNamesFile::Found::Unread &&
	    state->mis_known == outside_names_.fingerprint)
		return outside_names_;
	return names_file_;
}

uint64_t MissionDocument::names_file_overwrite() const {
	const MissionFileState *state = mission_file_state(file_state());
	return state && state->mis_overwrite != spent_overwrite_ ? state->mis_overwrite : 0;
}

bool MissionDocument::companion_unsaved() const {
	const MissionNamesFile &known = names_file();
	return known.found != MissionNamesFile::Found::Unread &&
	       (!same_names(mission_names(*this), known.kept) || !same_locks(mission_locks(*this), known.kept_locks));
}

bool MissionDocument::names_file_due(const CompanionFile &now) {
	++names_state_;
	names_left_ = MissionNamesLeft();
	writing_over_ = 0;
	const MissionNamesFile &known = names_file();
	if (known.found == MissionNamesFile::Found::Unread) return false; // read from bytes alone: nothing beside it known
	switch (now.state) {
	case CompanionFile::State::Absent:
		// None there, or gone since: the editor's to make where the mission holds a name the .mis carries, or a height
		// lock (S25). Holding none, it has none to save: its names are saved as none (no .mis is no name, no lock), and
		// nothing stays unsaved.
		if (mission_holds_names(*this) || mission_holds_locks(*this)) return true;
		names_file_ = MissionNamesFile();
		names_file_.found = MissionNamesFile::Found::None;
		outside_names_ = MissionNamesFile();
		return false;
	case CompanionFile::State::Unreadable:
		names_left_.unwritten = "the file system would not read it to see what it holds (" + now.error + ")";
		return false;
	case CompanionFile::State::Read: break;
	}
	const uint64_t at = bytes_fingerprint(now.bytes);
	const uint64_t overwrite = names_file_overwrite();
	if (overwrite != 0 && overwrite == at) {
		writing_over_ = at;
		return true;
	}
	if (at != known.fingerprint) {
		// Bytes other than those read or written: another program's, left as they are, read for what they hold.
		names_left_.outside = true;
		outside_names_ = read_names_file(*this, now.bytes);
		return false;
	}
	// The editor's own, matching the mission: written. The original editor's, one that does not match, one that does
	// not read: left as they are (names_file_findings lists each), and where it matched when it was read or written
	// and the mission's records moved since, why it no longer does (the next open takes none of its names). A write
	// that fails says so too.
	names_left_.stale = names_file_stale(*this, known);
	return known.ours && known.found == MissionNamesFile::Found::Matched;
}

bool MissionDocument::names_file_text(std::string &text) {
	std::string error;
	const bool made = mission_names_text(*this, &base_heights_, text, error);
	// The unlocked entities past the heights handed (all of them where the terrain did not read): written height locked
	// at their heights (mission::write_mis_text, D-MIS-17), listed.
	names_left_.locked.clear();
	names_left_.locked_count = 0;
	if (made) {
		size_t at = 0;
		for (const K kind : {K::Item, K::Building, K::Marker, K::Organic})
			for (const Node *row : rows_of(kind)) {
				if (at >= base_heights_.size() && static_cast<const EntityRow &>(*row).native.mis_height_lock == 0) {
					++names_left_.locked_count;
					if (names_left_.locked.size() < MissionNamesLeft::kLockedNamed)
						names_left_.locked.push_back(mission_entity_title(*this, *row, nullptr));
				}
				++at;
			}
	}
	base_heights_.clear();
	++names_state_;
	if (!made) {
		names_left_.unwritten = error;
		writing_over_ = 0;
	}
	return made;
}

void MissionDocument::names_file_written(bool ok, uint64_t fingerprint, const std::string &error) {
	++names_state_;
	if (!ok) {
		names_left_.unwritten = "the file system would not write it (" + error + ")";
		writing_over_ = 0;
		return;
	}
	// Written from the mission: its names the mission's now. Write it from the mission is spent by the write it
	// was for: a later change to the file is left as it is again.
	if (writing_over_ != 0) spent_overwrite_ = writing_over_;
	writing_over_ = 0;
	MissionNamesFile written;
	written.found = MissionNamesFile::Found::Matched;
	written.ours = true;
	written.fingerprint = fingerprint;
	written.kept = mission_names(*this);
	written.kept_locks = mission_locks(*this);
	// What it holds, as a read of it would hold it: the names and the events' lines it was written with.
	written.names = written.kept;
	for (const Node *row : rows_of(K::Event)) written.events.push_back(static_cast<const EventRow &>(*row).native);
	names_file_ = std::move(written);
	outside_names_ = MissionNamesFile();
	// The entities it was written with height locked (D-MIS-17) stay listed until the next write.
	std::vector<std::string> locked = std::move(names_left_.locked);
	const size_t locked_count = names_left_.locked_count;
	names_left_ = MissionNamesLeft();
	names_left_.locked = std::move(locked);
	names_left_.locked_count = locked_count;
}

bool MissionDocument::set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) {
	// A variable's name (mission_variable_name_edits): V`position`, the value its name, "" none.
	if (edit.field == kMisVariableField) {
		const auto *name = std::get_if<std::string>(&edit.value);
		std::string why;
		if (edit.position >= mission::kVariableNameCount || !name ||
		    (!name->empty() && !mission::mis_name_allowed(*name, mission::kMisDescriptionChars, why))) {
			error = make_finding(CoreFinding::DocumentValue, DiagnosticSeverity::Error,
			                     "A variable's name names V0 to V" + std::to_string(mission::kVariableNameIndexMax) +
			                             (why.empty() ? std::string(".") : ": " + why),
			                     path());
			return false;
		}
		auto made = std::make_shared<MissionFileState>();
		if (const MissionFileState *was = mission_file_state(state.get())) *made = *was;
		made->variables[edit.position] = *name;
		state = std::move(made);
		return true;
	}
	const bool overwrite = edit.field == kMisOverwriteField, known = edit.field == kMisKnownField;
	if (!overwrite && !known) {
		error = make_finding(CoreFinding::DocumentValue, DiagnosticSeverity::Error, "A mission has no such file-wide value.", path());
		return false;
	}
	const auto *text = std::get_if<std::string>(&edit.value);
	uint64_t fingerprint = 0;
	if (!text || !fingerprint_from_hex(*text, fingerprint)) {
		error = make_finding(CoreFinding::DocumentValue, DiagnosticSeverity::Error,
		                     std::string(edit.field) + " names a .mis by its bytes' fingerprint: 16 hex digits.", path());
		return false;
	}
	auto made = std::make_shared<MissionFileState>();
	if (const MissionFileState *was = mission_file_state(state.get())) *made = *was;
	(overwrite ? made->mis_overwrite : made->mis_known) = fingerprint;
	state = std::move(made);
	return true;
}

std::shared_ptr<Node> MissionDocument::make_node(NodeKind kind, NodeId,
                                                 const std::vector<std::shared_ptr<const Node>> &rows,
                                                 std::string &error) {
	if (is_entity_kind(kind)) {
		// The item is the Add's field (Edit::field "item"); until it is set the record names item 0.
		auto row = std::make_shared<EntityRow>(kind, new_entity(pool_of(kind), kItemIdOffset, next_free_ssn(rows)));
		shape(*row);
		return row;
	}
	if (kind == k(K::Area)) {
		const int id = free_zone_id(rows);
		if (!id) {
			error = "Every zone id 1 to 99 is taken: remove an area trigger first.";
			return nullptr;
		}
		auto row = std::make_shared<AreaRow>(kind, bms::AreaTrigger{id, 0, 0, 0, 0, 0, 0, 0});
		shape(*row);
		return row;
	}
	if (kind == k(K::Event)) {
		auto row = std::make_shared<EventRow>(kind);
		shape(*row);
		return row;
	}
	error = "A mission keeps its mission row and its 128 waypoint paths; it adds entities, area triggers and events.";
	return nullptr;
}

size_t MissionDocument::row_position(const Node &row, const std::vector<std::shared_ptr<const Node>> &rows,
                                     size_t position) const {
	const int band = mission_band(row.kind);
	if (band < 0) return position;
	// The band's bounds among the other rows: after the last row of an earlier band, before the
	// first of a later one (a moved row is among them: passed over).
	size_t first = 0, last = 0, others = 0;
	for (const auto &other : rows) {
		if (other.get() == &row) continue;
		++others;
		const int of = mission_band(other->kind);
		if (of < band) first = others;
		if (of <= band) last = others;
	}
	return std::min(std::max(position, first), last);
}

void MissionDocument::prepare_duplicate(Node &copy, const Node &original,
                                        const std::vector<std::shared_ptr<const Node>> &rows) const {
	if (is_entity_kind(copy.kind)) static_cast<EntityRow &>(copy).native.id = next_free_ssn(rows);
	// With none free the copy keeps the id, and accept_step refuses the step.
	if (copy.kind == k(K::Area))
		if (const int id = free_zone_id(rows)) static_cast<AreaRow &>(copy).native.id = id;
	// A duplicated event's parameters keep naming the events they named, its original included: the
	// original editor's paste of a copied event renumbers every event index at or past where the copy goes,
	// the copy's own with the rest [orig: JOTACmed.exe sub_44D460 @ 0x44d460, the Event triggers
	// @ 0x44d521 and the ResetEvent actions @ 0x44d550] (renumber_references, D-MIS-9).
}

bool MissionDocument::accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const {
	for (const RowSwap &swap : step.swaps) {
		// The weapon loadout as the writer would write it must read back as the same entries (Save
		// writes it from scratch; the game's reader takes a fourth string as an entry's damage class
		// only when it is a nonzero number or holds no letter, else as the next entry's name [orig:
		// AIProfile_SanitizeConfigData @0x40cfe0]).
		if (swap.after && swap.after->kind == k(K::Mission)) {
			size_t first = 0;
			if (!bms::loadout_reads_back(static_cast<const MissionRow &>(*swap.after).native.loadout, first)) {
				refusal.message = "Weapon loadout entry " + std::to_string(first + 1) +
				        " would read back as another: the game reads an entry's fourth string as its damage class only "
				        "when it is a nonzero number or holds no letter (else as the next entry's name, every later entry "
				        "shifting), and a three-string entry before a name of that form as the same. Give the damage class "
				        "a number.";
				return false;
			}
		}
		// An area trigger the step puts in (a Duplicate with every zone id 1 to 99 taken keeps its
		// original's) never shares a zone id: the resolver would find one of the two for both.
		if (!swap.before && swap.after && swap.after->kind == k(K::Area)) {
			const int32_t id = static_cast<const AreaRow &>(*swap.after).native.id;
			for (const auto &other : rows.rows())
				if (other.get() != swap.after.get() && other->kind == k(K::Area) &&
				    static_cast<const AreaRow &>(*other).native.id == id) {
					refusal.message = "Every zone id 1 to 99 is taken: the new area trigger has none of its own. Remove an "
					        "area trigger first.";
					return false;
				}
		}
		// An entity's AI class is its item's row's on every save (set_item_classes): set there, never on the record.
		if (swap.in_place() && is_entity_kind(swap.before->kind)) {
			const bms::Entity &was = static_cast<const EntityRow &>(*swap.before).native;
			const bms::Entity &now = static_cast<const EntityRow &>(*swap.after).native;
			if (was.type_id == now.type_id && std::memcmp(was.name1, now.name1, sizeof(was.name1)) != 0 && item_class(now)) {
				refusal.message = "An entity's AI class is its item's sid: a save writes it from the item's row, as the "
				                  "original editor does. Set the sid on the item.";
				return false;
			}
		}
		// A waypoint marker's path and place are its path's stops (D-MIS-6): edited there, never on the marker.
		if (swap.in_place() && swap.before->kind == k(K::Marker)) {
			const bms::Entity &was = static_cast<const EntityRow &>(*swap.before).native;
			const bms::Entity &now = static_cast<const EntityRow &>(*swap.after).native;
			if (was.type_id == def::DEF_TYPE_WAYPOINT && now.type_id == def::DEF_TYPE_WAYPOINT &&
			    (was.waypoint_id != now.waypoint_id || was.wp_number != now.wp_number)) {
				refusal.message = "A waypoint marker's path and place are its path's stops: add it to the path's "
				                  "Stops, or move it there.";
				return false;
			}
		}
		const NodeKind kind = swap.before ? swap.before->kind : swap.after ? swap.after->kind : -1;
		if ((kind != k(K::Mission) && kind != k(K::WaypointPath)) || swap.in_place()) continue;
		refusal.message = kind == k(K::Mission) ? "A mission keeps its mission row where it is."
		                              : "A mission keeps its 128 waypoint paths where they are: edit their stops.";
		return false;
	}
	return true;
}

bool MissionDocument::get(const NodeAddress &address, const std::string &field, Value &out) const {
	const Node *node = address.child == 0 ? row(address.row) : nullptr;
	if (node && node->kind == k(K::WaypointPath) && field == "marker_count") {
		out = int64_t(static_cast<const PathRow &>(*node).native.stops.size());
		return true;
	}
	if (node && node->kind == k(K::Marker) && (field == "waypoint_id" || field == "wp_number") &&
	    static_cast<const EntityRow &>(*node).native.type_id == def::DEF_TYPE_WAYPOINT) {
		// The path whose stop it is, and the number a save gives it there (stop_numbers).
		int64_t path = 0, number = 0;
		const size_t index = index_among(rows(), node);
		for (const Node *each : rows_of(K::WaypointPath)) {
			const MissionPath &held = static_cast<const PathRow &>(*each).native;
			if (held.number == 0) continue;
			const auto at = std::find(held.stops.begin(), held.stops.end(), uint32_t(index));
			if (at == held.stops.end()) continue;
			path = held.number;
			number = stop_numbers(held.stops, held_numbers(rows()), size_t(held.number))[size_t(at - held.stops.begin())];
			break;
		}
		out = field == "waypoint_id" ? path : number;
		return true;
	}
	// An entity's AI class as a save writes it: its item's row's (set_item_classes).
	if (node && is_entity_kind(node->kind) && field == "name1")
		if (const std::string *ai_class = item_class(static_cast<const EntityRow &>(*node).native)) {
			out = *ai_class;
			return true;
		}
	return TableDocument::get(address, field, out);
}

void MissionDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	const std::string &id = use.schema->id;
	// The header's tile set: the terrain's tile atlas, loaded as its name with the extension replaced by
	// .TGA through the TGA reader (ADR 0046 S18, kTextureArgTileSet [orig: Terrain_LoadEnvironmentConfig @
	// 0x6109C8..0x6109EE; Terrain_LoadTileSetAtlas @ 0x604A90]).
	if (use.reference == ReferenceKind::Texture && id == "terrain_tile")
		use.loader_arg = texture_role_arg(renderer::TextureRoleId::TerrainTileAtlas, kTextureArgTileSet);
	if (address.kind == k(K::Marker) && address.child == 0 && (id == "waypoint_id" || id == "wp_number"))
		if (const Node *node = row(address.row);
		    node && static_cast<const EntityRow &>(*node).native.type_id == def::DEF_TYPE_WAYPOINT)
			use.read_only = true; // its path's stops set them (get)
	if (is_entity_kind(address.kind) && address.child == 0 && id == "name1")
		if (const Node *node = row(address.row); node && item_class(static_cast<const EntityRow &>(*node).native))
			use.read_only = true; // its item's row sets it on every save (get)
	if (address.kind == k(K::Trigger) && id == "sub_type") use.own_choices = true;
	if (address.kind == k(K::Action) && id == "action_sub_type") use.own_choices = true;
	const int slot = param_slot(id);
	if (slot >= 0 && (address.kind == k(K::Trigger) || address.kind == k(K::Action))) {
		const Node *node = row(address.row);
		const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
		if (record) {
			// The parameter as its record's type reads it: its words, and its values by name.
			ParamKind kind = ParamKind::Raw;
			if (address.kind == k(K::Trigger)) {
				const bms::Trigger &trigger = record.as<bms::Trigger>();
				use.label = trigger_param_label(trigger, slot);
				kind = trigger_param_kind(trigger, slot);
			} else {
				const bms::Action &action = record.as<bms::Action>();
				use.label = action_param_label(action, slot);
				kind = action_param_kind(action, slot);
			}
			if (use.label && !*use.label) use.label = nullptr;
			use.own_choices = param_choices(kind).count > 0;
		}
	}
	// What the file defines and names by id is looked up in the mission's own scope; the player's
	// SSN names no record of it, yet another entity is picked by name there (S15).
	if (use.reference == ReferenceKind::MissionEntity) {
		Value value;
		if (get(address, id, value) && value == Value(int64_t(kPlayerSsn))) {
			use.reference = ReferenceKind::None;
			use.picks = ReferenceKind::MissionEntity;
		}
	}
	const auto by_id = [](ReferenceKind kind) {
		return kind == ReferenceKind::MissionEntity || kind == ReferenceKind::MissionZone;
	};
	if (by_id(use.defines) || by_id(use.reference) || by_id(use.picks)) use.scope = mission_scope(*this);
	// A number that forms a text key (mission_text_edges' forms): picked by the strings of the section
	// the game looks its key up in, the number written (FieldUse::key_prefix): an entity's name index,
	// STRNAME%03i in PeopleNames [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a], and a win slot,
	// the objectives panel's STRWINCOND%03i in WinConditions [orig: HUD_DrawWinConditions @0x5ba940];
	// in the mission's own table, else medmssn.bin [orig: TextResource_LoadMissionTextBin @0x51ed90].
	// The numbers the game looks a key up by: a nonzero name index (the spawn names no entity by 0), a
	// win slot that is not empty (0 and 255 end the panel, mission_labels' mission_value_label).
	const auto keyed = [&](const hud::TextKeyForm &form, int64_t first, int64_t last) {
		use.picks = ReferenceKind::TextId;
		use.scope = mission_text_table(path()) + "/" + form.section;
		use.scope_alternate = mission_text_fallback();
		use.key_prefix = form.prefix;
		use.key_first = first;
		use.key_last = last;
	};
	// A def-type 2044 marker's name index keys its LOCATION%03i in Locations first, any number
	// [orig: Entity_SpawnFromBMSRecord @0x40F196..0x40F1A4].
	if (!address.child && is_entity_kind(address.kind) && id == "name_index") {
		const Node *node = row(address.row);
		const int32_t type = node && is_entity_kind(node->kind) ? static_cast<const EntityRow &>(*node).native.type_id : 0;
		if (type == def::DEF_TYPE_NAMED_LOCATION) keyed(hud::kLocationKey, 0, INT32_MAX);
		else keyed(hud::kPeopleNameKey, 1, INT32_MAX);
	}
	// A waypoint's name id (record +0x60), which a type-6005 or 6006 marker's spawn keeps for the waypoint
	// HUD, keying STRWPNAME%03i in WPNames, any number [orig: Entity_SpawnFromBMSRecord @0x40f0ad,
	// @0x40f176: entity+672; HUD_GetWaypointName @0x59473d]; the type-6005 spawn looks the name up
	// (mission_text_edges).
	if (!address.child && is_entity_kind(address.kind) && id == "ttool_index") {
		const Node *node = row(address.row);
		const int32_t type = node && is_entity_kind(node->kind) ? static_cast<const EntityRow &>(*node).native.type_id : 0;
		if (type == def::DEF_TYPE_WAYPOINT || type == def::DEF_TYPE_KOTH_CENTRE) keyed(hud::kWaypointNameKey, 0, INT32_MAX);
	}
	// A bounding box's value by what its type makes of it (box_value_label).
	if (address.kind == k(K::BoundingBox) && id == "ref_id") {
		const Node *node = row(address.row);
		const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
		if (record)
			if (const char *label = box_value_label(record.as<bms::BoundingBox>().type)) use.label = label;
	}
	// A Mission box's second word is its mission name's last four characters (bms::BoundingBoxType::Mission).
	if (address.kind == k(K::BoundingBox) && id == "reserved0") {
		const Node *node = row(address.row);
		const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
		if (record && record.as<bms::BoundingBox>().type == int32_t(bms::BoundingBoxType::Mission))
			use.label = "Mission name (its last four characters)";
	}
	if (!address.child && address.kind == k(K::Mission) && id.compare(0, 15, "win_conditions[") == 0)
		keyed(hud::kWinConditionKey, 1, 254);
	// A Play dialog's or a Dialog trigger's number forms the dialog's name (audio::dialog_name_of's dlg%03i, a
	// trigger's audio::trigger_dialog_name), which the game finds in the mission's dialog bank (mission_references'
	// Dialog edge): picked by the bank's dialogs, the number written; an action's dialog from 1 (0 plays none
	// [orig: Dialog_PlayByIndex @ 0x527af4]), a trigger's any.
	if (id == "param1" && (address.kind == k(K::Trigger) || address.kind == k(K::Action))) {
		const Node *node = row(address.row);
		const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
		const bool trigger = address.kind == k(K::Trigger);
		const ParamKind kind = !record ? ParamKind::Raw
		                       : trigger ? trigger_param_kind(record.as<bms::Trigger>(), 0)
		                                 : action_param_kind(record.as<bms::Action>(), 0);
		if (kind == ParamKind::Dialog) {
			use.picks = ReferenceKind::Dialog;
			use.scope = strutil::to_upper(mission_dialog_bank(*this));
			use.key_prefix = "dlg";
			use.key_first = trigger ? INT32_MIN : 1;
			use.key_last = INT32_MAX;
		}
	}
	// Show text's number forms Triggered Text's ID%03i in the mission's table [orig: HUD_DisplayTriggeredText
	// @0x51F190]: picked by that section's strings, the number written (S24, the board's chip).
	if (id == "param1" && address.kind == k(K::Action)) {
		const Node *node = row(address.row);
		const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
		if (record && record.as<bms::Action>().action_type == bms::ActionType::OutputText)
			keyed(hud::kTriggeredTextKey, 0, INT32_MAX);
	}
}

bool MissionDocument::record_choices(const NodeAddress &address, const FieldUse &use,
                                     std::vector<FieldChoice> &out) const {
	const Node *node = row(address.row);
	const RecordHandle record = node ? record_in(*node, address) : RecordHandle();
	if (!record) return false;
	const std::string &id = use.schema->id;
	const int slot = param_slot(id);
	MissionChoices choices;
	ParamKind kind = ParamKind::Raw;
	if (address.kind == k(K::Trigger)) {
		const bms::Trigger &trigger = record.as<bms::Trigger>();
		if (id == "sub_type") choices = trigger_sub_types(int32_t(trigger.main_type));
		else if (slot >= 0) choices = param_choices(kind = trigger_param_kind(trigger, slot));
	} else if (address.kind == k(K::Action)) {
		const bms::Action &action = record.as<bms::Action>();
		if (id == "action_sub_type") choices = action_sub_types(int32_t(action.action_type));
		else if (slot >= 0) choices = param_choices(kind = action_param_kind(action, slot));
	}
	if (!choices.count) return false;
	out = choices_of(choices);
	// A team by the one name the words give it everywhere (S15: the red team is team 2 in the round's end
	// and the area actions alike), the original editor's own word kept as its name.
	if (kind == ParamKind::Team)
		for (FieldChoice &choice : out) choice.label = team_words(choice.value);
	return true;
}

void MissionDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	const Node *node = row(address.row);
	if (!node || address.child) return;
	if (is_entity_kind(node->kind)) {
		const EntityRow &entity = static_cast<const EntityRow &>(*node);
		// What the picker shows beside the SSN: the item the record is.
		facts.value = std::to_string(entity_item_id(entity.native));
		// The lookups by SSN scan the pools in order and take the first row of the SSN [orig:
		// Entity_KillByNetId @0x43DBD0, Entity_HandleAlertStateEvent @0x43DEE0]: a later one is
		// found by none of them (the graph resolves to the first).
		const Lookups &first = lookups();
		const auto found = first.ssns.find(entity.native.id);
		if (found != first.ssns.end() && found->second != node->id) {
			facts.inert = true;
			// An area check tests every organic and item of the SSN [orig: Entity_IsBmsRefInTriggerBounds
			// @0x43e510]: this one too.
			facts.inert_reason = node->kind == k(K::Organic) || node->kind == k(K::Item)
			                             ? "another entity has this SSN, and the game's lookups by SSN find the first in "
			                               "pool order (organics, items, buildings, markers); an area check tests this one too"
			                             : "another entity has this SSN, and the game's lookups by SSN find the first in "
			                               "pool order (organics, items, buildings, markers)";
		}
		return;
	}
	if (node->kind == k(K::Area)) {
		// The resolver scans the table for the id and takes the first area of it, in file order [orig:
		// EventTrigger_ResolveZoneTriggerRefs @0x453000, the scan @0x453077; bms-event-runtime-re.md 7.3]:
		// a later one of the same id is named by no trigger or action.
		const Lookups &first = lookups();
		const auto found = first.zones.find(static_cast<const AreaRow &>(*node).native.id);
		if (found != first.zones.end() && found->second != node->id) {
			facts.inert = true;
			facts.inert_reason = "an earlier area trigger has this zone id, and the game's resolver takes the first of it";
		}
	}
}

const MissionDocument::Lookups &MissionDocument::lookups() const {
	if (lookups_.made && lookups_.load_generation == load_generation() && lookups_.revision == revision())
		return lookups_;
	Lookups made;
	made.by_kind.resize(kMissionKindCount);
	made.groups.assign(256, 0);
	std::unordered_map<int32_t, int> order; // the pool order of each SSN's first holder so far
	for (const auto &row : rows()) {
		if (!row || row->kind < 0 || size_t(row->kind) >= kMissionKindCount) continue;
		std::vector<const Node *> &of_kind = made.by_kind[size_t(row->kind)];
		made.places.emplace(row->id, of_kind.size());
		of_kind.push_back(row.get());
		if (is_entity_kind(row->kind)) {
			const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
			const int mine = entity_pool_of(row->kind);
			const auto held = order.find(entity.id);
			if (held == order.end() || mine < held->second) {
				order[entity.id] = mine;
				made.ssns[entity.id] = row->id;
			}
			++made.groups[entity.group_id];
		} else if (row->kind == k(K::Area)) {
			made.zones.emplace(static_cast<const AreaRow &>(*row).native.id, row->id);
		}
	}
	// The first type-6088 marker of each wp_number.
	const std::vector<const Node *> &markers = made.by_kind[size_t(K::Marker)];
	for (const Node *row : markers) {
		const bms::Entity &marker = static_cast<const EntityRow &>(*row).native;
		if (marker.type_id == kNumberedMarkerType) made.numbered.emplace(marker.wp_number, row->id);
	}
	// Each marker's stops, the paths in their order.
	for (const Node *row : made.by_kind[size_t(K::WaypointPath)]) {
		const MissionPath &path = static_cast<const PathRow &>(*row).native;
		for (size_t i = 0; i < path.stops.size(); ++i)
			if (path.stops[i] < markers.size()) made.stops[markers[path.stops[i]]->id].push_back({path.number, i});
	}
	// The player's route: the first path with the flag, its stops by their marker's index, as many as
	// the game walks (bms::player_route_stop_count: its signed compare walks none for a count of 2^31
	// or more) [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e50, the count capped at 128 @0x502efc].
	for (const Node *row : made.by_kind[size_t(K::WaypointPath)]) {
		const MissionPath &path = static_cast<const PathRow &>(*row).native;
		if (!bms::is_player_route(path.record)) continue;
		// The stops a save lays into the record's slots (its first 32), as many as the game walks.
		const size_t count = std::min({path.stops.size(), kMaxWaypointPathMarkers, bms::kPlayerRouteMaxStops});
		for (size_t i = 0; i < count; ++i)
			if (path.stops[i] < markers.size()) made.route.insert(markers[path.stops[i]]->id);
		break;
	}
	made.made = true;
	made.load_generation = load_generation();
	made.revision = revision();
	lookups_ = std::move(made);
	return lookups_;
}

// --- the references between the rows ------------------------------------------------------------------

bool MissionDocument::renumber_references(const StagedRows &rows, const RecordShift &shift,
                                          std::vector<Edit> &sites, std::string &error) const {
	const bool markers = shift.reference == ReferenceKind::MissionMarker;
	// The groups and the paths are fixed tables: no edit moves them.
	if (!markers && shift.reference != ReferenceKind::MissionEvent) return true;
	const auto set = [&sites](const NodeAddress &address, const char *field, size_t now) {
		Edit edit;
		edit.address = address;
		edit.field = field;
		edit.value = int64_t(now);
		sites.push_back(std::move(edit));
	};
	for (const std::shared_ptr<const Node> &node : rows.rows()) {
		if (markers && node->kind == k(K::WaypointPath)) {
			const PathRow &path = static_cast<const PathRow &>(*node);
			if (path.ids.lists.empty()) continue;
			const std::vector<RecordIds> &ids = path.ids.lists[0];
			const std::vector<uint32_t> &stops = path.native.stops;
			for (size_t i = 0; i < stops.size() && i < ids.size(); ++i) {
				const size_t now = shift.now(int64_t(stops[i]));
				if (now == size_t(stops[i])) continue;
				if (now == RecordShift::kRemoved) {
					error = "Path " + std::to_string(path.native.number) + "'s stop " + std::to_string(i + 1) +
					        " visits this marker: remove the stop first.";
					return false;
				}
				set({node->id, k(K::Stop), ids[i].id}, "marker", now);
			}
		}
		// A type-6005 marker's advance trigger, an event by its index (none below 0) [orig:
		// Entity_SpawnFromBMSRecord @0x40f0b3; EventTrigger_MarkLinkedSpawnPoints @0x452ce0], moves with its
		// event as the original editor moves it, 0 like any index [orig: JOTACmed.exe sub_44D460 @ 0x44d460,
		// @ 0x44d59c; sub_411C90 @ 0x411dd8..0x411ded]; a removal sets it to -1 first (removal_edits).
		if (!markers && is_entity_kind(node->kind)) {
			const bms::Entity &marker = static_cast<const EntityRow &>(*node).native;
			if (marker.type_id != def::DEF_TYPE_WAYPOINT || marker.wp_adv_trigger < 0) continue;
			const size_t now = shift.now(marker.wp_adv_trigger);
			if (now == size_t(marker.wp_adv_trigger)) continue;
			if (now == RecordShift::kRemoved) {
				error = record_title({node->id, node->kind, 0}) + " advances on this event: clear its waypoint advance trigger first.";
				return false;
			}
			set({node->id, node->kind, 0}, "wp_adv_trigger", now);
		}
		if (!markers && node->kind == k(K::Event)) {
			const EventRow &event = static_cast<const EventRow &>(*node);
			if (event.ids.lists.size() < 2) continue;
			// Every event's, the ones the step put in included: a copy keeps naming the event its record names
			// wherever that event now stands (D-MIS-9).
			const auto renumber = [&](NodeKind kind, size_t list, size_t i, int32_t held, const char *what) {
				const size_t now = shift.now(held);
				if (now == size_t(held)) return true;
				if (now == RecordShift::kRemoved) {
					error = "Event " + std::to_string(index_among(rows.rows(), node.get()) + 1) + "'s " + what + " " +
					        std::to_string(i + 1) + " names this event: remove that " + what + " first.";
					return false;
				}
				set({node->id, kind, event.ids.lists[list][i].id}, "param1", now);
				return true;
			};
			for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
				if (trigger_param_kind(event.native.triggers[i], 0) == ParamKind::Event &&
				    !renumber(k(K::Trigger), 0, i, event.native.triggers[i].param1, "trigger"))
					return false;
			for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
				if (action_param_kind(event.native.actions[i], 0) == ParamKind::Event &&
				    !renumber(k(K::Action), 1, i, event.native.actions[i].param1, "action"))
					return false;
		}
	}
	if (markers) copies_keep_their_route(rows, shift, sites);
	return true;
}

std::vector<std::vector<NodeId>> MissionDocument::place_lists(const Node &row) const {
	if (row.kind != k(K::WaypointPath)) return {};
	const PathRow &path = static_cast<const PathRow &>(row);
	std::vector<NodeId> stops;
	if (!path.ids.lists.empty()) {
		stops.reserve(path.ids.lists[0].size());
		for (const RecordIds &stop : path.ids.lists[0]) stops.push_back(stop.id);
	}
	return {std::move(stops)};
}

bool MissionDocument::renumber_places(const StagedRows &rows, NodeId row, size_t list, const RecordShift &shift,
                                      std::vector<Edit> &sites, std::string &error) const {
	(void)error;
	const Node *node = rows.find(row);
	if (list != 0 || !node || node->kind != k(K::WaypointPath)) return true;
	const int number = static_cast<const PathRow &>(*node).native.number;
	std::vector<MissionStopNamer> namers;
	for (const std::shared_ptr<const Node> &each : rows.rows()) mission_stop_namers(*each, number, namers);
	for (const MissionStopNamer &namer : namers) {
		const size_t now = shift.now(namer.place);
		// A stop the edit removed: its number kept, naming the stop that took its place.
		if (now == RecordShift::kRemoved || now == size_t(namer.place)) continue;
		Edit edit;
		edit.address = namer.record;
		edit.field = namer.field;
		edit.value = int64_t(now);
		sites.push_back(std::move(edit));
	}
	return true;
}

void mission_stop_namers(const Node &row, int path, std::vector<MissionStopNamer> &out) {
	if (path <= 0 || path >= kFirstPathCommand) return;
	if (is_entity_kind(row.kind)) {
		const bms::Entity &entity = static_cast<const EntityRow &>(row).native;
		if (row.kind != k(K::Marker) && entity.waypoint_id == path)
			out.push_back({{row.id, row.kind, 0}, "wp_number", entity.wp_number});
		return;
	}
	if (row.kind != k(K::Event)) return;
	const EventRow &event = static_cast<const EventRow &>(row);
	if (event.ids.lists.size() < 2) return;
	for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i) {
		const bms::Trigger &trigger = event.native.triggers[i];
		if (trigger_param_kind(trigger, 2) == ParamKind::PathNode && trigger.param2 == path)
			out.push_back({{row.id, k(K::Trigger), event.ids.lists[0][i].id}, "param3", trigger.param3});
	}
	for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i) {
		const bms::Action &action = event.native.actions[i];
		if (action_param_kind(action, 2) == ParamKind::PathNode && action.param2 == path)
			out.push_back({{row.id, k(K::Action), event.ids.lists[1][i].id}, "param3", action.param3});
	}
}

size_t mission_removed_stop_namers(const MissionDocument &document, const std::vector<Edit> &edits) {
	// Each path's places the Removes take.
	std::map<int, std::set<int64_t>> taken;
	for (const Edit &edit : edits) {
		if (edit.operation != EditOperation::Remove || edit.address.kind != k(K::Stop)) continue;
		Document::Placement at;
		if (!document.placement(edit.address, at)) continue;
		const Node *row = document.row(edit.address.row);
		if (!row || row->kind != k(K::WaypointPath)) continue;
		taken[static_cast<const PathRow &>(*row).native.number].insert(int64_t(at.index));
	}
	if (taken.empty()) return 0;
	size_t count = 0;
	std::vector<MissionStopNamer> namers;
	for (const MissionKind kind : {MissionKind::Item, MissionKind::Building, MissionKind::Organic, MissionKind::Event})
		for (const Node *row : document.rows_of(kind))
			for (const auto &[path, places] : taken) {
				namers.clear();
				mission_stop_namers(*row, path, namers);
				for (const MissionStopNamer &namer : namers) count += places.count(namer.place);
			}
	return count;
}

// A waypoint marker the edit put in (a Duplicate's copy, a Paste's) stays a stop of the path it carries, among
// the stops its number sorts with, as the original editor's copy stays on its route: its copy is the whole item
// [orig: JOTACmed.exe MissionItem_CloneTranslated @ 0x455e70, the qmemcpy keeping +0x8c and +0x90], and its
// rebuild puts every waypoint marker on the path it carries [orig: sub_44CFD0 @ 0x44d028..0x44d063], sorted by
// number. The stop goes after the last of the path's stops whose marker, held on the path, sorts before the
// copy (by number, a tie by the marker's place: a duplicate right after its original); `sites` takes the Adds.
void MissionDocument::copies_keep_their_route(const StagedRows &rows, const RecordShift &shift,
                                              std::vector<Edit> &sites) const {
	std::vector<bool> kept(shift.after, false);
	for (const size_t to : shift.to)
		if (to < shift.after) kept[to] = true;
	const std::vector<std::pair<int, int32_t>> held = held_numbers(rows.rows());
	std::map<int, std::vector<size_t>> routes; // a path's stops as the step leaves them, the Adds put in
	std::map<int, const Node *> paths;
	for (const std::shared_ptr<const Node> &node : rows.rows())
		if (node->kind == k(K::WaypointPath)) {
			const MissionPath &path = static_cast<const PathRow &>(*node).native;
			paths[path.number] = node.get();
			std::vector<size_t> &route = routes[path.number];
			for (const uint32_t stop : path.stops) route.push_back(shift.now(int64_t(stop)));
		}
	for (size_t copy = 0; copy < shift.after && copy < held.size(); ++copy) {
		const int path = held[copy].first;
		if (kept[copy] || path <= 0 || path >= kFirstPathCommand || !paths.count(path)) continue;
		std::vector<size_t> &route = routes[path];
		if (std::find(route.begin(), route.end(), copy) != route.end()) continue;
		size_t at = 0;
		for (size_t i = 0; i < route.size(); ++i) {
			const size_t stop = route[i];
			if (stop >= held.size() || held[stop].first != path) continue;
			if (held[stop].second < held[copy].second || (held[stop].second == held[copy].second && stop < copy)) at = i + 1;
		}
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {paths[path]->id, k(K::Stop), 0};
		add.field = "marker";
		add.value = int64_t(copy);
		add.position = at;
		sites.push_back(std::move(add));
		route.insert(route.begin() + std::ptrdiff_t(at), copy);
	}
}

bool MissionDocument::removal_edits(const std::vector<NodeAddress> &records, std::vector<Edit> &out,
                                    std::string &error) const {
	std::set<NodeId> removed;
	for (const NodeAddress &record : records) removed.insert(record.child ? record.child : record.row);
	const auto remove_of = [](const NodeAddress &address) {
		Edit edit;
		edit.operation = EditOperation::Remove;
		edit.address = address;
		return edit;
	};
	std::vector<Edit> namers, cleared;
	for (const NodeAddress &record : records) {
		const Node *node = row(record.row);
		if (!node) {
			error = "The selected record no longer exists.";
			return false;
		}
		if (record.child) continue;
		const size_t index = index_among(rows(), node);
		if (node->kind == k(K::Marker)) {
			// The stops that visit it go first (the core then renumbers the later markers' stops).
			for (const auto &other : rows()) {
				if (other->kind != k(K::WaypointPath)) continue;
				const PathRow &path = static_cast<const PathRow &>(*other);
				if (path.ids.lists.empty()) continue;
				const std::vector<uint32_t> &stops = path.native.stops;
				for (size_t i = 0; i < stops.size() && i < path.ids.lists[0].size(); ++i)
					if (stops[i] == index) namers.push_back(remove_of({other->id, k(K::Stop), path.ids.lists[0][i].id}));
			}
		}
		if (node->kind == k(K::Event)) {
			// A waypoint marker advancing on the event advances on none: the original editor's delete of an
			// event sets such a marker's advance trigger to -1 and says so [orig: JOTACmed.exe sub_455B20
			// @ 0x455b20, WP_EVENT_DELETED].
			for (const auto &other : rows()) {
				if (!is_entity_kind(other->kind) || removed.count(other->id)) continue;
				const bms::Entity &marker = static_cast<const EntityRow &>(*other).native;
				if (marker.type_id != def::DEF_TYPE_WAYPOINT || marker.wp_adv_trigger != int32_t(index)) continue;
				Edit clear;
				clear.address = {other->id, other->kind, 0};
				clear.field = "wp_adv_trigger";
				clear.value = int64_t(-1);
				cleared.push_back(std::move(clear));
			}
			// What names the event by its index goes first where the removal takes it too, itself or
			// with its event, whatever the order the records were named in; a namer the removal leaves
			// refuses it with its site (S13 D8's convention).
			for (const auto &other : rows()) {
				if (other->kind != k(K::Event)) continue;
				const EventRow &event = static_cast<const EventRow &>(*other);
				if (event.ids.lists.size() < 2) continue;
				const auto namer = [&](NodeKind kind, size_t list, size_t i, const char *what) {
					const NodeId id = event.ids.lists[list][i].id;
					if (!removed.count(id) && !removed.count(other->id)) {
						error = "Event " + std::to_string(index_among(rows(), other.get()) + 1) + "'s " + what + " " +
						        std::to_string(i + 1) + " names this event: remove that " + what + " first.";
						return false;
					}
					namers.push_back(remove_of({other->id, kind, id}));
					return true;
				};
				for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
					if (trigger_param_kind(event.native.triggers[i], 0) == ParamKind::Event &&
					    event.native.triggers[i].param1 == int32_t(index) && !namer(k(K::Trigger), 0, i, "trigger"))
						return false;
				for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
					if (action_param_kind(event.native.actions[i], 0) == ParamKind::Event &&
					    event.native.actions[i].param1 == int32_t(index) && !namer(k(K::Action), 1, i, "action"))
						return false;
			}
		}
	}
	// The cleared advance triggers, then the namers, each once, before the records themselves (one the
	// removal names too goes once).
	for (Edit &edit : cleared) out.push_back(std::move(edit));
	std::set<NodeId> listed;
	for (const Edit &edit : namers)
		if (listed.insert(edit.address.child).second) out.push_back(edit);
	for (const NodeAddress &record : records)
		if (listed.insert(record.child ? record.child : record.row).second) out.push_back(remove_of(record));
	return true;
}

// --- the references no field's value is ---------------------------------------------------------------

void mission_text_edges(const MissionDocument &document, const NodeAddress &address, std::vector<GraphEdge> &out,
                        bool placed, const char *as_field, int64_t as_value) {
	const Node *row = document.row(address.row);
	const MissionRow *header = document.mission_row();
	if (!row || !header) return;
	// A field's number as the record holds it, or as asked (as_field).
	const auto number_of = [&](const std::string &field, int64_t held) {
		return as_field && field == as_field ? as_value : held;
	};
	// The mission's own table, else the one the game loads in its place where the mission has none,
	// never both [orig: TextResource_LoadMissionTextBin @0x51ed90]: the edge's alternate, which the
	// graph reads only where the project has no table of the mission's name.
	const std::string table = mission_text_table(document.path());
	const auto text = [&](const std::string &field, const hud::TextKeyForm &form, int64_t number) {
		GraphEdge edge;
		edge.source = document.path();
		if (placed) {
			edge.record = document.record_path(address);
			edge.locator = document.locator(address);
		}
		edge.address = address;
		edge.field = field;
		edge.kind = ReferenceKind::TextId;
		edge.value = hud::text_key(form, int(number));
		edge.scope = table + "/" + form.section;
		edge.scope_alternate = mission_text_fallback();
		out.push_back(std::move(edge));
	};
	const bms::Header &head = header->native.header;
	if (!address.child && is_entity_kind(row->kind)) {
		const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
		// [orig: Entity_SpawnFromBMSRecord @0x40F17C..0x40F20C: a def-type 2044 marker names itself by
		// sprintf("LOCATION%03i", rec+4), its own name_index, any number (0 keys LOCATION000); the
		// runtime's install_mission_location_names, D-NET-511]
		if (entity.type_id == def::DEF_TYPE_NAMED_LOCATION)
			text("name_index", hud::kLocationKey, number_of("name_index", entity.name_index));
		// [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a: sprintf("STRNAME%03i", rec+4), gated
		// on the index being nonzero]
		if (const int64_t name = number_of("name_index", entity.name_index)) text("name_index", hud::kPeopleNameKey, name);
		// [orig: Entity_SpawnFromBMSRecord @0x40f0be..0x40f0e0: a type-6005 record's sprintf("STRWPNAME%03i",
		// rec+0x60) looked up in WPNames for the waypoint's name, any number]
		if (entity.type_id == def::DEF_TYPE_WAYPOINT) {
			text("ttool_index", hud::kWaypointNameKey, number_of("ttool_index", entity.ttool_index));
			// Where the name is witnessed shown, a stop of the player's route (the waypoint HUD, which
			// shows gametext's STRWPNAMEDEFAULT for a missing one [orig: HUD_GetWaypointName @0x59476f..
			// 0x59477b]), a key the table lacks is a finding; any other waypoint's (the shipped missions'
			// patrol stops, mostly id 0) none.
			if (!document.on_player_route(*row)) out.back().optional = true;
		}
		return;
	}
	if (!address.child && row->kind == k(K::Mission)) {
		// The objectives panel's rows: the win slots 1..8 until a 0 or 255 id, each its STRWINCOND
		// [orig: HUD_DrawWinConditions @0x5ba940, the break @0x5ba9e0].
		for (int slot = 0; slot < 8; ++slot) {
			const std::string field = "win_conditions[" + std::to_string(slot) + "]";
			const int64_t win = number_of(field, head.win_conditions[slot]);
			if (win == 0 || win == 255) break;
			text(field, hud::kWinConditionKey, win);
		}
		return;
	}
	if (address.kind != k(K::Action) || row->kind != k(K::Event)) return;
	const Document::RecordPath path = document.path_in(*row, address.child);
	const EventRow &event = static_cast<const EventRow &>(*row);
	if (path.size() != 1 || path[0].index >= event.native.actions.size()) return;
	const bms::Action &action = event.native.actions[path[0].index];
	// What the actions read of the table, each by the text id of the slot (1..8) its first parameter
	// names: SubGoalWon's chat line STRWINMSG, SubGoalLost's STRLOSEMSG [orig: EventAction_Dispatch
	// case 14 @0x454500, the key @0x454552; case 15 @0x4545e0, the key @0x45460c]; a shown
	// ShowWin/LoseSubgoal's directive STRWINDIRECTIVE / STRLOSEDIRECTIVE (one not shown returns before
	// the lookup) [orig: cases 35, 36 @0x4546af, @0x454724 -> HUD_ShowObjectiveNotification @0x5BA2E0,
	// the inactive return @0x5ba2f3, the keys @0x5ba316 / @0x5ba34b]; OutputText's line, Triggered
	// Text's ID%03i [orig: HUD_DisplayTriggeredText @0x51F190]. A slot past the eight reads a byte
	// outside the header's tables, which the port does not model (runtime/world World::
	// show_objective_notification): no edge.
	const auto slot_text = [&](const uint8_t *ids, int64_t slot, const hud::TextKeyForm &form) {
		if (slot >= 1 && slot <= 8) text("param1", form, ids[slot - 1]);
	};
	const int64_t param1 = number_of("param1", action.param1), param2 = number_of("param2", action.param2);
	switch (action.action_type) {
	case bms::ActionType::SubGoalWon: slot_text(head.win_conditions, param1, hud::kWinMessageKey); break;
	case bms::ActionType::SubGoalLost: slot_text(head.lose_conditions, param1, hud::kLoseMessageKey); break;
	case bms::ActionType::ShowWinSubgoal:
		if (param2 != 0) slot_text(head.win_conditions, param1, hud::kWinDirectiveKey);
		break;
	case bms::ActionType::ShowLoseSubgoal:
		if (param2 != 0) slot_text(head.lose_conditions, param1, hud::kLoseDirectiveKey);
		break;
	case bms::ActionType::OutputText: text("param1", hud::kTriggeredTextKey, param1); break;
	default: break;
	}
}

void mission_references(const Document &document, Extracted &out) {
	const auto *mission = dynamic_cast<const MissionDocument *>(&document);
	const MissionRow *header = mission ? mission->mission_row() : nullptr;
	if (!header) return;
	// The text keys the records' numbers form (mission_text_edges): the entities' in the rows' order,
	// the objectives panel's, then the actions' in the events' order.
	for (const auto &row : document.rows())
		if (row && is_entity_kind(row->kind)) mission_text_edges(*mission, {row->id, row->kind, 0}, out.edges, true);
	mission_text_edges(*mission, {header->id, header->kind, 0}, out.edges, true);
	for (const Node *row : mission->rows_of(K::Event)) {
		const EventRow &event = static_cast<const EventRow &>(*row);
		if (event.ids.lists.size() < 2) continue;
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			mission_text_edges(*mission, {row->id, k(K::Action), event.ids.lists[1][i].id}, out.edges, true);
	}
	// The files the game finds by the mission's name (documents/mission_file_set.h), one edge each
	// from the file itself: the name its reader builds, then the alternate or the fallback the reader
	// takes next; an optional one makes no finding when the project lacks it.
	const std::string &file = document.path();
	// The dialog bank the mission loads: its own <base>.dbf, or the one its header names; its sounds beside it.
	const std::string dialog_slot = dialog_slot_of(*mission);
	const std::string bank = mission_dialog_bank(*mission);
	for (const MissionFileSetRow &row : mission_file_set()) {
		const mission::Sidecar *sidecar = mission::sidecar_for_role(row.role);
		if (!sidecar) continue;
		// What the row's reader opens (mission::sidecar_names): the dialog rows by the bank the header picks,
		// a row read only beside another's file naming that file (the dialog's sounds, beside its .dbf).
		const mission::SidecarNames names = mission::sidecar_names(file, *sidecar, dialog_slot);
		GraphEdge edge;
		edge.source = file;
		edge.field = row.role;
		edge.kind = row.kind;
		edge.value = names.name;
		edge.fallback = sidecar->fallback ? std::string(sidecar->fallback) : names.alternate;
		edge.optional = row.optional;
		edge.needs = names.needs;
		out.edges.push_back(std::move(edge));
	}
	// The dialog a trigger or an action names, in the mission's dialog bank: a Dialog trigger's the readers'
	// "dlg%.3d" of its number (audio::trigger_dialog_name), a Play dialog's "dlg%03i" (audio::dialog_name_of),
	// 0 playing nothing [orig: Dialog_PlayByIndex @ 0x527af4].
	for (const Node *row : mission->rows_of(K::Event)) {
		const EventRow &event = static_cast<const EventRow &>(*row);
		if (event.ids.lists.size() < 2) continue;
		const auto plays = [&](NodeKind kind, size_t list, size_t i, const std::string &name) {
			if (name.empty()) return;
			const NodeAddress address{row->id, kind, event.ids.lists[list][i].id};
			GraphEdge edge;
			edge.source = file;
			edge.record = document.record_path(address);
			edge.locator = document.locator(address);
			edge.address = address;
			edge.field = "param1";
			edge.kind = ReferenceKind::Dialog;
			edge.value = name;
			edge.scope = strutil::to_upper(bank);
			edge.rewritable = true;
			edge.key_prefix = "dlg";
			out.edges.push_back(std::move(edge));
		};
		for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
			if (trigger_param_kind(event.native.triggers[i], 0) == ParamKind::Dialog)
				plays(k(K::Trigger), 0, i, audio::trigger_dialog_name(event.native.triggers[i].param1));
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			if (action_param_kind(event.native.actions[i], 0) == ParamKind::Dialog)
				plays(k(K::Action), 1, i, audio::dialog_name_of(event.native.actions[i].param1));
	}
	// Each event as a script of the mission's name finds it, by its index (ReferenceKind::MissionEventLatch: its
	// event() condition reads the event's latch [orig: WacCmd_Event @ 0x4ED1E0]), defined by the event's record,
	// so the event's Used by lists the script's lines.
	const std::string scope = mission_event_scope(file);
	size_t index = 0;
	for (const Node *row : mission->rows_of(K::Event)) {
		const NodeAddress address{row->id, row->kind, 0};
		GraphSymbol symbol;
		symbol.kind = ReferenceKind::MissionEventLatch;
		symbol.display = std::to_string(index++);
		symbol.name = symbol.display;
		symbol.file = file;
		symbol.record = document.record_path(address);
		symbol.record_key = document.record_identity(address);
		symbol.locator = document.locator(address);
		symbol.address = address;
		symbol.scope = scope;
		out.symbols.push_back(std::move(symbol));
	}
}

} // namespace opennova::editor
