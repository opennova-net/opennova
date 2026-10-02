// The mission type's findings (mission_validation.h): its table, and validate_file over a mission
// document: the source findings its parse made (a rewrite that differs, the events' runs and their
// order), then what the game makes of the records (ADR 0046 S14): an SSN or a zone id two records
// carry, a degenerate zone, a zone id outside the editor's range, an event index past the table, a
// stop naming a marker the file lacks, a path counted past its slots or holding one stop, an entity on
// an empty path or starting past its count, a group past the tables, a pool past the game's limits,
// two game mode bits, a trigger type the evaluator lacks, a bounding box with a corner past the other.
// What an entity's pool makes of its item's TYPE is the mission's use check (graph/use_checks.cpp,
// mission.pool), which reads the item through the graph.
#include "mission_validation.h"

#include <iterator>
#include <map>
#include <string>
#include <utility>

#include <editor/documents/mission_document.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

namespace {

using namespace mission;
using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

constexpr const char *kRewriteSections = "with its sections as the game reads them";
constexpr const char *kRewriteRuns = "with each event's triggers and actions where the event stands";

constexpr FindingCodeEntry<MissionFinding> kFindingEntries[] = {
	{ MissionFinding::RewriteDiffers, { "mission.rewrite_differs", FindingFix::Rewrite, kRewriteSections } },
	{ MissionFinding::InvalidInput, { "mission.invalid_input", FindingFix::None, nullptr, true } },
	{ MissionFinding::EventOrder, { "mission.event_order", FindingFix::Rewrite, kRewriteRuns } },
	{ MissionFinding::SsnDuplicate, { "mission.ssn_duplicate" } },
	{ MissionFinding::ZoneDuplicate, { "mission.zone_duplicate" } },
	{ MissionFinding::ZoneDegenerate, { "mission.zone_degenerate" } },
	{ MissionFinding::ZoneId, { "mission.zone_id" } },
	{ MissionFinding::EventMissing, { "mission.event_missing" } },
	{ MissionFinding::MarkerMissing, { "mission.marker_missing" } },
	{ MissionFinding::PathCount, { "mission.path_count" } },
	{ MissionFinding::PathOneShot, { "mission.path_one_shot" } },
	{ MissionFinding::PathEmpty, { "mission.path_empty" } },
	{ MissionFinding::PathStart, { "mission.path_start" } },
	{ MissionFinding::GroupRange, { "mission.group_range" } },
	{ MissionFinding::PoolLimit, { "mission.pool_limit" } },
	{ MissionFinding::GameMode, { "mission.game_mode" } },
	{ MissionFinding::TriggerType, { "mission.trigger_type" } },
	{ MissionFinding::BoundingBox, { "mission.bounding_box" } },
	{ MissionFinding::Pool, { "mission.pool" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(MissionFinding::kCount),
              "every MissionFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the mission's rows follow MissionFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Missions);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// The severity of a source finding: the events' runs block (an error); a rewrite that differs and
// a layout the writer lays out again are what Save does (an info each).
DiagnosticSeverity source_severity(MissionFinding code) {
	return code == MissionFinding::InvalidInput ? DiagnosticSeverity::Error : DiagnosticSeverity::Info;
}

// The pools' limits, past which the game warns (fatal if dismissed) and loads on [orig:
// BMS_LoadAndValidateHeader @0x40e326].
constexpr size_t kMaxItems = 1200, kMaxBuildings = 1200, kMaxMarkers = 768, kMaxOrganics = 256;
// The group tables hold 64 [orig: docs/mission/bms-event-runtime-re.md 7.2].
constexpr int64_t kLastGroup = 63;
// The zone ids the original editor offers [orig editor: dfx2med Med_AreaTriggerDialogProc @0x40f400].
constexpr int32_t kFirstZoneId = 1, kLastZoneId = 99;
// A waypoint number from 1 to 122 names a path; 0 none, 123..127 a command.
constexpr int32_t kLastPathNumber = 122;
// The trigger main types the evaluator has a case for [orig: EventTrigger_EvaluateCondition
// @0x453620's jump table, 1..7].
constexpr int32_t kFirstMainType = 1, kLastMainType = 7;

// The order the game's lookups scan the pools in: organics, items, buildings, markers [orig:
// Entity_KillByNetId @0x43DBD0].
int lookup_order(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Organic: return 0;
	case K::Item: return 1;
	case K::Building: return 2;
	case K::Marker: return 3;
	default: return 4;
	}
}

struct Checker {
	const MissionDocument &document;
	std::vector<Diagnostic> &findings;

	Diagnostic &on(const NodeAddress &address, MissionFinding code, DiagnosticSeverity severity, std::string message,
	               const char *field = "") {
		Diagnostic d = make_finding(code, severity, std::move(message), document.path(), field);
		d.record = document.record_name(address);
		d.row_id = address.row;
		d.child_id = address.child;
		d.record_kind = address.kind;
		findings.push_back(std::move(d));
		return findings.back();
	}
	void on_file(MissionFinding code, DiagnosticSeverity severity, std::string message) {
		findings.push_back(make_finding(code, severity, std::move(message), document.path()));
	}

	void entities() {
		// Each SSN's rows: the one the lookups find (first in pool order, then in the file), then the
		// others, which no lookup reaches.
		std::map<int32_t, std::vector<const Node *>> by_ssn;
		size_t items = 0, buildings = 0, markers = 0, organics = 0;
		const std::vector<const Node *> paths = document.rows_of(K::WaypointPath);
		for (const auto &row : document.rows()) {
			if (!row || !is_entity_kind(row->kind)) continue;
			const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
			const NodeAddress address{row->id, row->kind, 0};
			by_ssn[entity.id].push_back(row.get());
			items += row->kind == k(K::Item);
			buildings += row->kind == k(K::Building);
			markers += row->kind == k(K::Marker);
			organics += row->kind == k(K::Organic);
			if (entity.group_id > kLastGroup)
				on(address, MissionFinding::GroupRange, DiagnosticSeverity::Error,
				   "Group " + std::to_string(entity.group_id) + " is past the 64 groups the game's tables hold.", "group");
			// On a path: one with no stop walks nowhere; a start node at or past the count reads the
			// slot word there as written [orig: AIWaypoint_UpdateTarget @0x457476 reads the raw slot].
			if (entity.waypoint_id >= 1 && entity.waypoint_id <= kLastPathNumber && size_t(entity.waypoint_id) < paths.size()) {
				const MissionPath &path = static_cast<const PathRow &>(*paths[entity.waypoint_id]).native;
				const size_t count = path.record.waypoint_numbers.size();
				if (count == 0)
					on(address, MissionFinding::PathEmpty, DiagnosticSeverity::Info,
					   "Waypoint path " + std::to_string(entity.waypoint_id) + " has no stop: the entity walks nowhere.",
					   "waypoint_id");
				else if (entity.wp_number < 0 || size_t(entity.wp_number) >= count)
					on(address, MissionFinding::PathStart, DiagnosticSeverity::Warning,
					   "Waypoint number " + std::to_string(entity.wp_number) + " is past the " + std::to_string(count) +
					           " stops of path " + std::to_string(entity.waypoint_id) + ": the game reads the slot word there as written.",
					   "wp_number");
			}
		}
		for (auto &[ssn, rows] : by_ssn) {
			if (rows.size() < 2) continue;
			const Node *found = rows[0];
			for (const Node *row : rows)
				if (lookup_order(row->kind) < lookup_order(found->kind)) found = row;
			for (const Node *row : rows)
				if (row != found)
					on({row->id, row->kind, 0}, MissionFinding::SsnDuplicate, DiagnosticSeverity::Warning,
					   "SSN " + std::to_string(ssn) + " is also " + document.kind_label(found->kind) + " " +
					           document.record_name({found->id, found->kind, 0}) +
					           "'s: the game's lookups find that one (organics, items, buildings, markers first) and never this.",
					   "id");
		}
		const auto limit = [&](size_t count, size_t max, const char *pool) {
			if (count > max)
				on_file(MissionFinding::PoolLimit, DiagnosticSeverity::Warning,
				        "The mission places " + std::to_string(count) + " " + pool + ", past the " + std::to_string(max) +
				                " the game allows: it warns at the load (fatal if dismissed) and goes on.");
		};
		limit(items, kMaxItems, "items");
		limit(buildings, kMaxBuildings, "buildings");
		limit(markers, kMaxMarkers, "markers");
		limit(organics, kMaxOrganics, "organics");
	}

	void paths() {
		const size_t markers = document.rows_of(K::Marker).size();
		for (const Node *row : document.rows_of(K::WaypointPath)) {
			const PathRow &path = static_cast<const PathRow &>(*row);
			const NodeAddress address{row->id, row->kind, 0};
			const bms::WaypointRecord &record = path.native.record;
			if (record.marker_count > kMaxWaypointPathMarkers)
				on(address, MissionFinding::PathCount, DiagnosticSeverity::Warning,
				   "The path stores a count of " + std::to_string(record.marker_count) +
				           ", past its 32 slots: the walk reads the next record's words as stops.",
				   "marker_count");
			else if (record.marker_count == 1)
				on(address, MissionFinding::PathOneShot, DiagnosticSeverity::Info,
				   "A path of one stop: the load makes it not loop [orig: Mission_LoadBMSFile @0x40fb72].", "marker_count");
			const std::vector<RecordIds> &ids = path.ids.lists.empty() ? std::vector<RecordIds>() : path.ids.lists[0];
			for (size_t i = 0; i < record.waypoint_numbers.size() && i < ids.size(); ++i)
				if (record.waypoint_numbers[i] >= markers)
					on({row->id, k(K::Stop), ids[i].id}, MissionFinding::MarkerMissing, DiagnosticSeverity::Warning,
					   "The stop names marker " + std::to_string(record.waypoint_numbers[i]) + ", and the mission has " +
					           std::to_string(markers) + ": the game reads the pool's zeroed entry (position 0, radius 0).",
					   "marker");
		}
	}

	void areas() {
		std::map<int32_t, const Node *> first;
		for (const Node *row : document.rows_of(K::Area)) {
			const bms::AreaTrigger &area = static_cast<const AreaRow &>(*row).native;
			const NodeAddress address{row->id, row->kind, 0};
			if (const auto found = first.find(area.id); found != first.end())
				on(address, MissionFinding::ZoneDuplicate, DiagnosticSeverity::Warning,
				   "Zone " + std::to_string(area.id) + " is also " + document.record_name({found->second->id, found->second->kind, 0}) +
				           "'s: which of the two the game's triggers and actions take is not known.",
				   "id");
			else
				first.emplace(area.id, row);
			if (area.x_min == area.x_max || area.y_min == area.y_max)
				on(address, MissionFinding::ZoneDegenerate, DiagnosticSeverity::Warning,
				   "The zone has no width on an axis: the game makes every trigger naming it read false and every "
				   "action naming it do nothing [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000].");
			if (area.id < kFirstZoneId || area.id > kLastZoneId)
				on(address, MissionFinding::ZoneId, DiagnosticSeverity::Info,
				   "Zone id " + std::to_string(area.id) + " is outside the 1 to 99 the game's editor offers.", "id");
		}
	}

	void events() {
		const std::vector<const Node *> events = document.rows_of(K::Event);
		const auto event_index = [&](const NodeAddress &address, int32_t index, const char *what) {
			if (index >= 0 && size_t(index) < events.size()) return;
			on(address, MissionFinding::EventMissing, DiagnosticSeverity::Error,
			   std::string("The ") + what + " names event " + std::to_string(index + 1) + ", and the mission has " +
			           std::to_string(events.size()) + ": the game reads past its event table.",
			   "param1");
		};
		const auto group_index = [&](const NodeAddress &address, int32_t index, const char *field) {
			if (index <= kLastGroup) return;
			on(address, MissionFinding::GroupRange, DiagnosticSeverity::Error,
			   "Group " + std::to_string(index) + " is past the 64 groups the game's tables hold.", field);
		};
		for (const Node *row : events) {
			const EventRow &event = static_cast<const EventRow &>(*row);
			if (event.ids.lists.size() < 2) continue;
			for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i) {
				const bms::Trigger &trigger = event.native.triggers[i];
				const NodeAddress address{row->id, k(K::Trigger), event.ids.lists[0][i].id};
				const int32_t main = int32_t(trigger.main_type);
				if (main < kFirstMainType || main > kLastMainType)
					on(address, MissionFinding::TriggerType, DiagnosticSeverity::Warning,
					   "Main type " + std::to_string(main) + " is none the game's evaluator has a case for: the trigger reads false.",
					   "main_type");
				else {
					const MissionChoices subs = trigger_sub_types(main);
					bool known = subs.count == 0;
					for (size_t s = 0; s < subs.count; ++s) known = known || subs.rows[s].value == trigger.sub_type;
					if (!known)
						on(address, MissionFinding::TriggerType, DiagnosticSeverity::Warning,
						   "Sub-type " + std::to_string(trigger.sub_type) + " is none the game's evaluator has a case for under main type " +
						           std::to_string(main) + ": the trigger reads false.",
						   "sub_type");
				}
				for (int slot = 0; slot < 4; ++slot) {
					const int32_t value = slot == 0 ? trigger.param1 : slot == 1 ? trigger.param2 : slot == 2 ? trigger.param3 : trigger.param4;
					const ParamKind kind = trigger_param_kind(trigger, slot);
					if (kind == ParamKind::Event) event_index(address, value, "trigger");
					if (kind == ParamKind::Group) group_index(address, value, slot == 0 ? "param1" : slot == 1 ? "param2" : slot == 2 ? "param3" : "param4");
				}
			}
			for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i) {
				const bms::Action &action = event.native.actions[i];
				const NodeAddress address{row->id, k(K::Action), event.ids.lists[1][i].id};
				for (int slot = 0; slot < 4; ++slot) {
					const int32_t value = slot == 0 ? action.param1 : slot == 1 ? action.param2 : slot == 2 ? action.param3 : action.param4;
					const ParamKind kind = action_param_kind(action, slot);
					if (kind == ParamKind::Event) event_index(address, value, "action");
					if (kind == ParamKind::Group) group_index(address, value, slot == 0 ? "param1" : slot == 1 ? "param2" : slot == 2 ? "param3" : "param4");
				}
			}
		}
	}

	void header() {
		const MissionRow *mission = document.mission_row();
		if (!mission) return;
		const NodeAddress address{mission->id, mission->kind, 0};
		const uint32_t modes = uint32_t(mission->native.header.attrib_flags) & bms::kGameModeMask;
		if (modes & (modes - 1)) {
			const uint32_t picked = bms::selected_game_mode(mission->native.header.attrib_flags);
			std::string name = std::to_string(picked);
			for (const FieldChoice &choice : MissionDocument::schema(k(K::Mission))[mission_table().kind(k(K::Mission))->find("attrib_flags")].choices)
				if (uint32_t(choice.value) == picked) name = choice.name;
			on(address, MissionFinding::GameMode, DiagnosticSeverity::Warning,
			   "More than one game mode bit is set: the game plays " + name + ", the first in its decode order.", "attrib_flags");
		}
		const RecordIds &ids = mission->ids;
		const size_t boxes = mission_table().kind(k(K::Mission))->lists().size() - 1; // the last list: the bounding boxes
		for (size_t i = 0; i < mission->native.bounding_boxes.size() && boxes < ids.lists.size() && i < ids.lists[boxes].size(); ++i) {
			const bms::BoundingBox &box = mission->native.bounding_boxes[i];
			if (box.min_x > box.max_x || box.min_y > box.max_y || box.min_z > box.max_z)
				on({mission->id, k(K::BoundingBox), ids.lists[boxes][i].id}, MissionFinding::BoundingBox, DiagnosticSeverity::Info,
				   "A corner of the box lies past the other on an axis: the load swaps them.");
		}
	}
};

} // namespace

const FindingCodeRow &finding_code(MissionFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable mission_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_mission_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *mission = dynamic_cast<const MissionDocument *>(&document);
	if (!mission) return findings;
	// The source findings, each under the code its parse gave it, on the record its locator names
	// as the file was loaded, wherever that record is now (source_address; gone: the file alone).
	const std::vector<SourceIssue> &issues = mission->issues();
	const std::vector<MissionFinding> &codes = mission->issue_codes();
	for (size_t i = 0; i < issues.size(); ++i) {
		const SourceIssue &issue = issues[i];
		const MissionFinding code = i < codes.size() ? codes[i]
		                            : issue.blocks   ? MissionFinding::InvalidInput
		                                             : MissionFinding::RewriteDiffers;
		Diagnostic d = make_finding(code, source_severity(code), issue.message, document.path(), issue.field);
		d.record = issue.record;
		d.line = issue.line;
		if (!issue.locator.empty()) {
			const NodeAddress at = mission->source_address(issue.locator);
			if (at.row) {
				d.row_id = at.row;
				d.child_id = at.child;
				d.record_kind = at.kind;
			}
		}
		findings.push_back(std::move(d));
	}
	if (document.blocked()) return findings;
	Checker checker{*mission, findings};
	checker.header();
	checker.entities();
	checker.paths();
	checker.areas();
	checker.events();
	return findings;
}

} // namespace opennova::editor
