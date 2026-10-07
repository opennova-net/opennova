#include "mission_document.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <base/io/strutil.h>
#include <editor/documents/mission_file_set.h>
#include <editor/documents/mission_labels.h>
#include <editor/graph/reference_kinds.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/diagnostic.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
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

// The order the game's lookups scan the pools in: organics, items, buildings, markers [orig:
// Entity_KillByNetId @0x43DBD0, Entity_HandleAlertStateEvent @0x43DEE0: pools 0, 1, 2, 3].
int lookup_order(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Organic: return 0;
	case K::Item: return 1;
	case K::Building: return 2;
	case K::Marker: return 3;
	default: return 4;
	}
}

// The SSN a parameter holds for the player [orig: 04TR's watchdog SingleIsWithinArea(10000, zone 6),
// docs/mission/bms-event-runtime-re.md 7.3]: no record of the file carries it (where the engine gives
// the player that SSN is not in the records read), so it names none of them.
constexpr int64_t kPlayerSsn = 10000;

// The waypoint markers whose record +0x60 the spawn keeps as the waypoint's name id (entity+672): type
// 6005, which also looks the name up, and type 6006 [orig: Entity_SpawnFromBMSRecord @0x40f05a
// `cmp [edi], 1775h`, @0x40f0ad; @0x40f157 `cmp [edi], 1776h`, @0x40f176].
constexpr int32_t kWaypointType = 6005, kWaypointRepeatType = 6006;

// The waypoint list's commands [orig editor: dfx2med Med_ParamWaypointList @0x449c60 names them;
// docs/world/world-wac-ai-re.md section 11].
const char *path_command_name(int number) {
	switch (number) {
	case 123: return "Goto SSN (not driver, gunner)";
	case 124: return "Goto SSN (not driver)";
	case 125: return "Goto SSN (any)";
	case 126: return "Goto group";
	case 127: return "Goto player";
	default: return nullptr;
	}
}

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

// The sections of a written mission in the loader's order [orig: Mission_LoadBMSFile @0x40f7b6], as
// the bytes read lay them out (the two chunks by the lengths the file's header holds, bytes 578 and
// 582, which the parse derives again from the records it kept; the tables by the records parsed):
// the first whose bytes differ between the bytes read and the writer's (the header's count and
// chunk length words apart, which the chunks' own difference explains); "" for none.
std::string first_differing_section(const bms::File &file, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	struct Section {
		const char *name;
		size_t size;
	};
	const auto word = [&a](size_t at) { return at + 1 < a.size() ? size_t(a[at]) | (size_t(a[at + 1]) << 8) : size_t(0); };
	const Section sections[] = {
		{"header", bms::kHeaderSize},
		{"loadout chunk", word(578)},
		{"availability chunk", word(582)},
		{"items", file.items.size() * bms::kEntitySize},
		{"buildings", file.buildings.size() * bms::kEntitySize},
		{"markers", file.markers.size() * bms::kEntitySize},
		{"organics", file.organics.size() * bms::kEntitySize},
		{"waypoint paths", size_t(bms::kWaypointRecordCount) * bms::kWaypointRecordSize},
		{"groups", size_t(bms::kGroupRecordCount) * bms::kGroupRecordSize},
		{"layers", size_t(bms::kLayerRecordCount) * bms::kLayerRecordSize},
		{"area triggers", file.area_triggers.size() * bms::kAreaTriggerSize},
		{"event counts", 12},
		{"events", file.events.size() * bms::kEventSize},
		{"triggers", file.triggers.size() * bms::kTriggerSize},
		{"actions", file.actions.size() * bms::kActionSize},
		{"bounding box count", 4},
		{"bounding boxes", file.bounding_boxes.size() * bms::kBoundingBoxSize},
	};
	// The header's words the chunks' and the tables' sizes decide (its count and chunk lengths, bytes
	// 576..579 and 582..583: bms.cpp's write).
	const auto derived = [](size_t offset) { return (offset >= 576 && offset < 580) || (offset >= 582 && offset < 584); };
	size_t at = 0;
	for (const Section &section : sections) {
		if (at + section.size > a.size() || at + section.size > b.size()) return section.name;
		for (size_t i = 0; i < section.size; ++i)
			if (a[at + i] != b[at + i] && !(at == 0 && derived(i))) return section.name;
		at += section.size;
	}
	return a.size() == b.size() ? std::string() : std::string("length");
}

} // namespace

// --- the rows ----------------------------------------------------------------------------------------

template <> std::string MissionRow::name() const { return native.get_mission_name(); }
template <> std::string EntityRow::name() const { return std::to_string(native.id); }
template <> std::string PathRow::name() const {
	if (native.number == 0) return "None";
	if (const char *command = path_command_name(native.number)) return command;
	return std::to_string(native.number);
}
template <> std::string AreaRow::name() const { return "Zone " + std::to_string(native.id); }
template <> std::string MissionRecordRow<mission::EventChain>::name() const { return std::string(); }

template <> size_t MissionRow::footprint() const {
	size_t bytes = sizeof(MissionRow) + ids_footprint() + footprint_of(native.loadout.entries) +
	               footprint_of(native.item_availability) + footprint_of(native.group_records) +
	               footprint_of(native.layer_records) + footprint_of(native.bounding_boxes);
	for (const bms::WeaponLoadoutRecord &entry : native.loadout.entries)
		bytes += footprint_of(entry.name) + footprint_of(entry.ammo_primary) + footprint_of(entry.ammo_secondary) +
		         footprint_of(entry.flags);
	for (const bms::ItemAvailabilityEntry &entry : native.item_availability) bytes += footprint_of(entry.name);
	return bytes;
}
template <> size_t EntityRow::footprint() const { return sizeof(EntityRow) + ids_footprint(); }
template <> size_t PathRow::footprint() const {
	return sizeof(PathRow) + ids_footprint() + footprint_of(native.record.waypoint_numbers) +
	       footprint_of(native.record.padding);
}
template <> size_t AreaRow::footprint() const { return sizeof(AreaRow) + ids_footprint(); }
template <> size_t MissionRecordRow<mission::EventChain>::footprint() const {
	return sizeof(EventRow) + ids_footprint() + footprint_of(native.triggers) + footprint_of(native.actions);
}

bool is_mission_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Mission; }

int32_t next_free_ssn(const std::vector<std::shared_ptr<const Node>> &rows) {
	int32_t largest = 0;
	for (const auto &row : rows)
		if (is_entity_kind(row->kind)) largest = std::max(largest, static_cast<const EntityRow &>(*row).native.id);
	const int32_t next = largest + 1;
	return next == int32_t(kPlayerSsn) ? next + 1 : next;
}

std::string mission_scope(const DocumentBase &document) { return strutil::to_upper(basename_of(document.path())); }

std::string mission_dialog_bank(const MissionDocument &document) {
	const MissionRow *header = document.mission_row();
	const std::string slot =
	        header ? strutil::fixed_string(header->native.header.terrain + 16, 16) : std::string();
	return mission::dialog_bank_name(basename_of(document.path()), slot);
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

int MissionDocument::location_of(const Node &row) const {
	const Lookups &made = lookups();
	const auto found = made.locations.find(row.id);
	return found == made.locations.end() ? 0 : found->second;
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

bool MissionDocument::compose(bms::File &out) const { return compose_mission(rows(), out); }

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
	for (const auto &row : rows) {
		if (!row) continue;
		switch (static_cast<K>(row->kind)) {
		case K::Item: out.items.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Building: out.buildings.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Marker: out.markers.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::Organic: out.organics.push_back(static_cast<const EntityRow &>(*row).native); break;
		case K::WaypointPath: out.waypoint_records.push_back(static_cast<const PathRow &>(*row).native.record); break;
		case K::Area: out.area_triggers.push_back(static_cast<const AreaRow &>(*row).native); break;
		case K::Event: chains.push_back(static_cast<const EventRow &>(*row).native); break;
		default: break;
		}
	}
	join_event_chains(chains, out);
	sync_counts(out);
	return true;
}

SerializeResult MissionDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const SourceIssue &issue : issues())
			if (issue.blocks) result.issues.push_back(issue);
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
                            std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues,
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
		note(MissionFinding::InvalidInput, true,
		     "The mission's events cannot be edited: " + what +
		             " (the game reads each event's run by its first index and its count).");
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
		const std::string section = first_differing_section(file, bytes, written);
		note(MissionFinding::RewriteDiffers, false,
		     section == "length" ? std::string("The file holds bytes past what the writer writes: Save drops them.")
		                         : "The file's " + section + " holds bytes the writer writes otherwise: Save writes " +
		                                   "the section as the game reads it.");
	}
	issue_codes_ = std::move(codes);

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
	const auto entities = [&](K kind, const std::vector<bms::Entity> &records) {
		for (const bms::Entity &record : records) {
			auto row = std::make_shared<EntityRow>(k(kind), record);
			shape(*row);
			rows.push_back(row);
		}
	};
	entities(K::Item, file.items);
	entities(K::Building, file.buildings);
	entities(K::Marker, file.markers);
	entities(K::Organic, file.organics);
	for (size_t i = 0; i < file.waypoint_records.size(); ++i) {
		auto row = std::make_shared<PathRow>(k(K::WaypointPath), MissionPath{file.waypoint_records[i], int(i)});
		shape(*row);
		rows.push_back(row);
	}
	for (const bms::AreaTrigger &area : file.area_triggers) {
		auto row = std::make_shared<AreaRow>(k(K::Area), area);
		shape(*row);
		rows.push_back(row);
	}
	for (EventChain &chain : chains) {
		auto row = std::make_shared<EventRow>(k(K::Event), std::move(chain));
		shape(*row);
		rows.push_back(row);
	}
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
	if (copy.kind == k(K::Event)) {
		// A parameter naming the event it is in (a ResetEvent that re-arms its own event) names the
		// copy: the one event the step puts in. One naming another event names that event still.
		EventRow &event = static_cast<EventRow &>(copy);
		const size_t self = index_among(rows, &original);
		for (size_t i = 0; i < event.native.triggers.size(); ++i)
			if (trigger_param_kind(event.native.triggers[i], 0) == ParamKind::Event &&
			    size_t(event.native.triggers[i].param1) == self)
				event.links.push_back({0, uint32_t(i), 0});
		for (size_t i = 0; i < event.native.actions.size(); ++i)
			if (action_param_kind(event.native.actions[i], 0) == ParamKind::Event &&
			    size_t(event.native.actions[i].param1) == self)
				event.links.push_back({1, uint32_t(i), 0});
	}
}

bool MissionDocument::accept_list_edit(const Node &row, const ListChange &change, std::string &error) const {
	if (row.kind != k(K::WaypointPath)) return true;
	// A stop's path, and for a Move the one it goes to: one whose stored count exceeds its slots
	// takes no stop edit (D-MIS-6).
	for (const Located *owner : {change.owner, change.destination}) {
		if (!owner || owner->record.kind != k(K::WaypointPath)) continue;
		const MissionPath &path = owner->record.as<MissionPath>();
		if (path.record.marker_count <= kMaxWaypointPathMarkers) continue;
		error = "Path " + std::to_string(path.number) + " stores a count of " + std::to_string(path.record.marker_count) +
		        ", past its 32 slots: what the game's editor writes for such a path is not known, so its stops stay "
		        "as they are.";
		return false;
	}
	return true;
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
		const NodeKind kind = swap.before ? swap.before->kind : swap.after ? swap.after->kind : -1;
		if ((kind != k(K::Mission) && kind != k(K::WaypointPath)) || swap.in_place()) continue;
		refusal.message = kind == k(K::Mission) ? "A mission keeps its mission row where it is."
		                              : "A mission keeps its 128 waypoint paths where they are: edit their stops.";
		return false;
	}
	return true;
}

void MissionDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	const std::string &id = use.schema->id;
	// The header's tile set: the terrain's tile atlas, loaded as its name with the extension replaced by
	// .TGA through the TGA reader (ADR 0046 S18, kTextureArgTileSet [orig: Terrain_LoadEnvironmentConfig @
	// 0x6109C8..0x6109EE; Terrain_LoadTileSetAtlas @ 0x604A90]).
	if (use.reference == ReferenceKind::Texture && id == "terrain_tile")
		use.loader_arg = texture_role_arg(TextureRoleId::TerrainTileAtlas, kTextureArgTileSet);
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
		if (get(address, id, value) && value == Value(kPlayerSsn)) {
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
	const auto keyed = [&](const char *section, const char *prefix, int64_t first, int64_t last) {
		use.picks = ReferenceKind::TextId;
		use.scope = strutil::to_upper(mission_base_name(basename_of(path()))) + ".BIN/" + section;
		use.scope_alternate = "MEDMSSN.BIN";
		use.key_prefix = prefix;
		use.key_first = first;
		use.key_last = last;
	};
	if (!address.child && is_entity_kind(address.kind) && id == "name_index") keyed("PeopleNames", "STRNAME", 1, INT32_MAX);
	// A waypoint's name id (record +0x60), which a type-6005 or 6006 marker's spawn keeps for the waypoint
	// HUD, keying STRWPNAME%03i in WPNames, any number [orig: Entity_SpawnFromBMSRecord @0x40f0ad,
	// @0x40f176: entity+672; HUD_GetWaypointName @0x59473d]; the type-6005 spawn looks the name up
	// (mission_text_edges).
	if (!address.child && is_entity_kind(address.kind) && id == "ttool_index") {
		const Node *node = row(address.row);
		const int32_t type = node && is_entity_kind(node->kind) ? static_cast<const EntityRow &>(*node).native.type_id : 0;
		if (type == kWaypointType || type == kWaypointRepeatType) {
			use.applies = Applicability::Reads;
			keyed("WPNames", "STRWPNAME", 0, INT32_MAX);
		}
	}
	if (!address.child && address.kind == k(K::Mission) && id.compare(0, 15, "win_conditions[") == 0)
		keyed("WinConditions", "STRWINCOND", 1, 254);
	// A Play dialog's or a Dialog trigger's number forms the dialog's name, dlg%03i, which the game finds in the
	// mission's dialog bank (mission_references' Dialog edge): picked by the bank's dialogs, the number written; an
	// action's dialog from 1 (0 plays none [orig: Dialog_PlayByIndex @ 0x527af4]), a trigger's any.
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
	int location = 0;
	for (const auto &row : rows()) {
		if (!row || row->kind < 0 || size_t(row->kind) >= kMissionKindCount) continue;
		std::vector<const Node *> &of_kind = made.by_kind[size_t(row->kind)];
		made.places.emplace(row->id, of_kind.size());
		of_kind.push_back(row.get());
		if (is_entity_kind(row->kind)) {
			const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
			const int mine = lookup_order(row->kind);
			const auto held = order.find(entity.id);
			if (held == order.end() || mine < held->second) {
				order[entity.id] = mine;
				made.ssns[entity.id] = row->id;
			}
			++made.groups[entity.group_id];
			// [orig: Entity_SpawnFromBMSRecord @0x40f182..0x40f221: each def-type 2044 marker registers the
			// next location name, in spawn order] (mission_references' LOCATION keys).
			if (row->kind == k(K::Marker) && entity.type_id == 2044) made.locations.emplace(row->id, ++location);
		} else if (row->kind == k(K::Area)) {
			made.zones.emplace(static_cast<const AreaRow &>(*row).native.id, row->id);
		}
	}
	// The player's route: the first path with the flag, its stops by their marker's index [orig:
	// NetPacket_WriteWorldStateLoad0x0F @0x502e50, the count capped at 128 @0x502efc].
	const std::vector<const Node *> &markers = made.by_kind[size_t(K::Marker)];
	for (const Node *row : made.by_kind[size_t(K::WaypointPath)]) {
		const bms::WaypointRecord &path = static_cast<const PathRow &>(*row).native.record;
		if (!(uint32_t(path.flags) & uint32_t(bms::WaypointFlags::PlayerRoute))) continue;
		const size_t count = std::min<size_t>({size_t(path.marker_count), path.waypoint_numbers.size(), size_t(128)});
		for (size_t i = 0; i < count; ++i)
			if (path.waypoint_numbers[i] < markers.size()) made.route.insert(markers[path.waypoint_numbers[i]]->id);
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
	// The events the edit put in, by the index each stands at now, in order (the copies of a paste,
	// a duplicate): what an EventLink of one of them names.
	std::vector<size_t> put;
	std::vector<bool> fresh;
	if (!markers) {
		std::vector<bool> found(shift.after, false);
		for (const size_t to : shift.to)
			if (to != RecordShift::kRemoved && to < shift.after) found[to] = true;
		fresh.assign(shift.after, false);
		for (size_t i = 0; i < shift.after; ++i)
			if (!found[i]) {
				put.push_back(i);
				fresh[i] = true;
			}
	}
	size_t event_index = 0;
	for (const std::shared_ptr<const Node> &node : rows.rows()) {
		if (markers && node->kind == k(K::WaypointPath)) {
			const PathRow &path = static_cast<const PathRow &>(*node);
			if (path.ids.lists.empty()) continue;
			const std::vector<RecordIds> &ids = path.ids.lists[0];
			const std::vector<uint32_t> &stops = path.native.record.waypoint_numbers;
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
		if (!markers && node->kind == k(K::Event)) {
			const EventRow &event = static_cast<const EventRow &>(*node);
			// Its links are read only by the step that put it in.
			const bool linked = event_index < fresh.size() && fresh[event_index] && !event.links.empty();
			++event_index;
			if (event.ids.lists.size() < 2) continue;
			const auto renumber = [&](NodeKind kind, size_t list, size_t i, int32_t held, const char *what) {
				size_t now = shift.now(held);
				if (linked)
					for (const EventLink &link : event.links)
						if (link.list == list && link.index == i) now = link.put < put.size() ? put[link.put] : RecordShift::kRemoved;
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
	return true;
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
	std::vector<Edit> namers;
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
				const std::vector<uint32_t> &stops = path.native.record.waypoint_numbers;
				for (size_t i = 0; i < stops.size() && i < path.ids.lists[0].size(); ++i)
					if (stops[i] == index) namers.push_back(remove_of({other->id, k(K::Stop), path.ids.lists[0][i].id}));
			}
		}
		if (node->kind == k(K::Event)) {
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
	// The namers, each once, before the records themselves (one the removal names too goes once).
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
	const std::string table = strutil::to_upper(mission_base_name(basename_of(document.path()))) + ".BIN";
	const auto text = [&](const std::string &field, const char *section, const char *key, int64_t number) {
		char name[32];
		std::snprintf(name, sizeof(name), "%s%03i", key, int(number));
		GraphEdge edge;
		edge.source = document.path();
		if (placed) {
			edge.record = document.record_path(address);
			edge.locator = document.locator(address);
		}
		edge.address = address;
		edge.field = field;
		edge.kind = ReferenceKind::TextId;
		edge.value = name;
		edge.scope = table + "/" + section;
		edge.scope_alternate = "MEDMSSN.BIN";
		out.push_back(std::move(edge));
	};
	const bms::Header &head = header->native.header;
	if (!address.child && is_entity_kind(row->kind)) {
		const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
		// [orig: Entity_SpawnFromBMSRecord @0x40f182..0x40f221: each def-type 2044 marker registers the
		// next location name, in spawn order]
		if (const int location = document.location_of(*row)) text(std::string(), "Locations", "LOCATION", location);
		// [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a: sprintf("STRNAME%03i", rec+4), gated
		// on the index being nonzero]
		if (const int64_t name = number_of("name_index", entity.name_index)) text("name_index", "PeopleNames", "STRNAME", name);
		// [orig: Entity_SpawnFromBMSRecord @0x40f0be..0x40f0e0: a type-6005 record's sprintf("STRWPNAME%03i",
		// rec+0x60) looked up in WPNames for the waypoint's name, any number]
		if (entity.type_id == kWaypointType) {
			text("ttool_index", "WPNames", "STRWPNAME", number_of("ttool_index", entity.ttool_index));
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
			text(field, "WinConditions", "STRWINCOND", win);
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
	const auto slot_text = [&](const uint8_t *ids, int64_t slot, const char *section, const char *key) {
		if (slot >= 1 && slot <= 8) text("param1", section, key, ids[slot - 1]);
	};
	const int64_t param1 = number_of("param1", action.param1), param2 = number_of("param2", action.param2);
	switch (action.action_type) {
	case bms::ActionType::SubGoalWon: slot_text(head.win_conditions, param1, "WinConditions", "STRWINMSG"); break;
	case bms::ActionType::SubGoalLost: slot_text(head.lose_conditions, param1, "LoseConditions", "STRLOSEMSG"); break;
	case bms::ActionType::ShowWinSubgoal:
		if (param2 != 0) slot_text(head.win_conditions, param1, "WinConditions", "STRWINDIRECTIVE");
		break;
	case bms::ActionType::ShowLoseSubgoal:
		if (param2 != 0) slot_text(head.lose_conditions, param1, "LoseConditions", "STRLOSEDIRECTIVE");
		break;
	case bms::ActionType::OutputText: text("param1", "Triggered Text", "ID", param1); break;
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
	const std::string bank = mission_dialog_bank(*mission);
	for (const MissionFileSetRow &row : mission_file_set()) {
		const mission::Sidecar *sidecar = mission::sidecar_for_role(row.role);
		if (!sidecar) continue;
		GraphEdge edge;
		edge.source = file;
		edge.field = row.role;
		edge.kind = row.kind;
		edge.value = mission::sidecar_name(file, *sidecar);
		edge.fallback = sidecar->fallback ? std::string(sidecar->fallback) : mission::sidecar_alternate_name(file, *sidecar);
		edge.optional = row.optional;
		if (row.role == std::string("dialog")) edge.value = bank;
		if (row.role == std::string("dialog_sounds")) {
			edge.value = mission::dialog_sounds_name(bank);
			edge.fallback = mission::dialog_sounds_name(bank, true);
		}
		// A row the reader reads only beside another's file (the dialog's sounds, beside its .dbf).
		if (const mission::Sidecar *needed = sidecar->needs ? mission::sidecar_for_role(sidecar->needs) : nullptr)
			edge.needs = needed->role == std::string("dialog") ? bank : mission::sidecar_name(file, *needed);
		out.edges.push_back(std::move(edge));
	}
	// The dialog a trigger or an action names (a Play dialog's, a Dialog trigger's), dlg%03i of its number, in the
	// mission's dialog bank.
	for (const Node *row : mission->rows_of(K::Event)) {
		const EventRow &event = static_cast<const EventRow &>(*row);
		if (event.ids.lists.size() < 2) continue;
		const auto plays = [&](NodeKind kind, size_t list, size_t i, int32_t number) {
			const NodeAddress address{row->id, kind, event.ids.lists[list][i].id};
			GraphEdge edge;
			edge.source = file;
			edge.record = document.record_path(address);
			edge.locator = document.locator(address);
			edge.address = address;
			edge.field = "param1";
			edge.kind = ReferenceKind::Dialog;
			char name[32];
			std::snprintf(name, sizeof(name), "dlg%03i", int(number));
			edge.value = name;
			edge.scope = strutil::to_upper(bank);
			edge.rewritable = true;
			edge.key_prefix = "dlg";
			out.edges.push_back(std::move(edge));
		};
		for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
			if (trigger_param_kind(event.native.triggers[i], 0) == ParamKind::Dialog)
				plays(k(K::Trigger), 0, i, event.native.triggers[i].param1);
		// [orig: Dialog_PlayByIndex @ 0x527af4: a dialog of 0 plays nothing]
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			if (action_param_kind(event.native.actions[i], 0) == ParamKind::Dialog && event.native.actions[i].param1 != 0)
				plays(k(K::Action), 1, i, event.native.actions[i].param1);
	}
}

} // namespace opennova::editor
