// The mission type's findings (mission_validation.h): its table, and validate_file over a mission
// document: the source findings its parse made (a rewrite that differs, the events' runs and their
// order), then what the game makes of the records (ADR 0046 S14): an SSN or a zone id two records
// carry (the one no lookup finds offered an id of its own, DI-11), an SSN a lookup scanning fewer
// pools finds no row of, a degenerate zone, a zone id outside
// the editor's range, an event index past the table, a
// stop naming a marker the file lacks, a path counted past its slots or holding one stop, an entity on
// an empty path or starting past its count, a group past the tables, a pool past the game's limits,
// two game mode bits, a trigger type the evaluator lacks, a bounding box with a corner past the other.
// What an entity's pool makes of its item's TYPE is the mission's use check (graph/use_checks.cpp,
// mission.pool), which reads the item through the graph.
#include "mission_validation.h"

#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/gameprofile/game_type.h>
#include <editor/documents/mission_document.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <runtime/world/entity.h>
#include <runtime/world/spawn_select.h>

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
	// A record no lookup finds by its SSN or its zone id: an id of its own (DI-11, Diagnostic::planned).
	{ MissionFinding::SsnDuplicate, { "mission.ssn_duplicate", FindingFix::EditRecord } },
	{ MissionFinding::SsnUnscanned, { "mission.ssn_unscanned" } },
	{ MissionFinding::ZoneDuplicate, { "mission.zone_duplicate", FindingFix::EditRecord } },
	{ MissionFinding::ZoneDegenerate, { "mission.zone_degenerate" } },
	{ MissionFinding::ZoneId, { "mission.zone_id" } },
	// Read with no bound, past the table, no refusal witnessed (the gate follows retail): listed.
	{ MissionFinding::EventMissing, listed_code("mission.event_missing") },
	{ MissionFinding::MarkerMissing, { "mission.marker_missing" } },
	{ MissionFinding::PathCount, { "mission.path_count" } },
	{ MissionFinding::PathOneShot, { "mission.path_one_shot" } },
	{ MissionFinding::PathEmpty, { "mission.path_empty" } },
	{ MissionFinding::PathStart, { "mission.path_start" } },
	// Past the 64 groups the tables hold, read as written, no refusal witnessed: listed.
	{ MissionFinding::GroupRange, listed_code("mission.group_range") },
	{ MissionFinding::PoolLimit, { "mission.pool_limit" } },
	{ MissionFinding::GameMode, { "mission.game_mode" } },
	{ MissionFinding::TriggerType, { "mission.trigger_type" } },
	{ MissionFinding::BoundingBox, { "mission.bounding_box" } },
	{ MissionFinding::Pool, { "mission.pool" } },
	{ MissionFinding::NoStart, { "mission.no_start" } },
	// An entity the game leaves off the ground (DI-28): its z set where the game's rule stands it, the fix the
	// project check plans with its finding (Diagnostic::planned).
	{ MissionFinding::OffGround, { "mission.off_ground", FindingFix::EditRecord } },
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
// BMS_LoadAndValidateHeader @0x40e326]: the pools' capacities (runtime/world/entity.h
// retail_pool_capacity: organics 0, items 1, buildings 2, markers 3).
const size_t kMaxItems = world::retail_pool_capacity(1), kMaxBuildings = world::retail_pool_capacity(2),
             kMaxMarkers = world::retail_pool_capacity(3), kMaxOrganics = world::retail_pool_capacity(0);
// The group tables hold 64 (bms::kGroupRecordCount) [orig: docs/mission/bms-event-runtime-re.md 7.2].
constexpr int64_t kLastGroup = bms::kGroupRecordCount - 1;
// The zone ids the original editor offers [orig editor: dfx2med Med_AreaTriggerDialogProc @0x40f400].
constexpr int32_t kFirstZoneId = 1, kLastZoneId = 99;
// A waypoint number from 1 to 122 names a path; 0 none, 123..127 a command (kFirstPathCommand on).
constexpr int32_t kLastPathNumber = kFirstPathCommand - 1;
// The trigger main types the evaluator has a case for, bms::TriggerMainType's Group to Player [orig:
// EventTrigger_EvaluateCondition @0x453620's jump table, 1..7].
constexpr int32_t kFirstMainType = int32_t(bms::TriggerMainType::Group),
                  kLastMainType = int32_t(bms::TriggerMainType::Player);

struct Checker {
	const MissionDocument &document;
	std::vector<Diagnostic> &findings;
	// Each SSN's rows, in the file's order (entities(), before events()).
	std::map<int32_t, std::vector<const Node *>> by_ssn;

	// A lookup of an SSN that scans some pools alone (`scanned_pools`, mission::kOrganicPool..kMarkerPool):
	// where the SSN's rows are all of other pools, it finds none, which `outcome` says.
	void unscanned(const NodeAddress &address, int32_t ssn, uint8_t scanned_pools, const char *what,
	               const char *outcome) {
		std::vector<K> pools;
		const std::pair<uint8_t, K> bits[] = {{kOrganicPool, K::Organic}, {kItemPool, K::Item},
		                                      {kBuildingPool, K::Building}, {kMarkerPool, K::Marker}};
		for (const auto &[bit, pool] : bits)
			if (scanned_pools & bit) pools.push_back(pool);
		const auto rows = by_ssn.find(ssn);
		// SSN 0 names none; no row at all is the reference's own finding (reference.missing).
		if (ssn == 0 || rows == by_ssn.end()) return;
		for (const Node *row : rows->second)
			for (const K pool : pools)
				if (row->kind == k(pool)) return;
		const Node *first = rows->second.front();
		std::string scanned;
		for (const K pool : pools) {
			if (!scanned.empty()) scanned += pool == pools.back() ? " and " : ", ";
			scanned += pool == K::Organic ? "organics" : pool == K::Item ? "items" : pool == K::Building ? "buildings" : "markers";
		}
		on(address, MissionFinding::SsnUnscanned, DiagnosticSeverity::Warning,
		   "SSN " + std::to_string(ssn) + " is " + document.record_title({first->id, first->kind, 0}) + "'s, and " + what +
		           " looks it up among the " + scanned + " alone: " + outcome,
		   "param1");
	}

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
		// The SSNs the fixes give the rows no lookup finds, each its own: from one past the mission's
		// largest on (the SSN a new entity takes, next_free_ssn), never the player's.
		int32_t fresh = next_free_ssn(document.rows());
		for (auto &[ssn, rows] : by_ssn) {
			if (rows.size() < 2) continue;
			const Node *found = rows[0];
			for (const Node *row : rows)
				if (entity_pool_of(row->kind) < entity_pool_of(found->kind)) found = row;
			// The lookups by SSN take the first row in pool order [orig: Entity_KillByNetId @0x43DBD0]; an
			// area check tests every organic and item carrying it, not stopping at the first [orig:
			// Entity_IsBmsRefInTriggerBounds @0x43e510; docs/mission/bms-event-runtime-re.md 7.2a].
			const std::string found_title = document.record_title({found->id, found->kind, 0});
			for (const Node *row : rows) {
				if (row == found) continue;
				const bool area = row->kind == k(K::Organic) || row->kind == k(K::Item);
				Diagnostic &d = on({row->id, row->kind, 0}, MissionFinding::SsnDuplicate, DiagnosticSeverity::Warning,
				                   "SSN " + std::to_string(ssn) + " is also " + found_title +
				                           "'s: the game's lookups by SSN find that one (organics, items, buildings, markers "
				                           "first) and never this" +
				                           (area ? std::string(", but an area check (SingleIsWithinArea) tests every organic "
				                                               "and item carrying it, this one too.")
				                                 : std::string(".")),
				                   "id");
				// Its fix (DI-11): an SSN of its own, which the lookups then find it by.
				const int32_t own = fresh;
				fresh = ssn_after(own);
				Edit set;
				set.address = {row->id, row->kind, 0};
				set.field = "id";
				set.value = int64_t(own);
				d.planned.push_back({"Give it SSN " + std::to_string(own),
				                     "Sets its SSN to " + std::to_string(own) +
				                             ", one past the mission's largest (the SSN a new entity takes; never the "
				                             "player's 10000): the game's lookups by SSN then find it. Whatever names SSN " +
				                             std::to_string(ssn) + " still finds " + found_title +
				                             (area ? ", and an area check no longer counts this one under it." : "."),
				                     {set}});
			}
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
		const std::vector<const Node *> rows = document.rows_of(K::Area);
		// The zone ids the fixes give the areas no trigger names, each its own: the first ids of the 1 to 99
		// the game's editor offers that no area has.
		std::set<int32_t> taken;
		for (const Node *row : rows) taken.insert(static_cast<const AreaRow &>(*row).native.id);
		int32_t fresh = kFirstZoneId;
		std::map<int32_t, const Node *> first;
		for (const Node *row : rows) {
			const bms::AreaTrigger &area = static_cast<const AreaRow &>(*row).native;
			const NodeAddress address{row->id, row->kind, 0};
			if (first.count(area.id)) {
				Diagnostic &d = on(address, MissionFinding::ZoneDuplicate, DiagnosticSeverity::Warning,
				                   "An earlier area trigger has zone " + std::to_string(area.id) +
				                           ": the game's resolver takes the first area of an id, so no trigger or action "
				                           "names this one.",
				                   "id");
				// Its fix (DI-11): a zone id of its own, which a trigger or an action can then name it by.
				while (fresh <= kLastZoneId && taken.count(fresh)) ++fresh;
				if (fresh <= kLastZoneId) {
					Edit set;
					set.address = address;
					set.field = "id";
					set.value = int64_t(fresh);
					d.planned.push_back({"Give it zone " + std::to_string(fresh),
					                     "Sets its zone id to " + std::to_string(fresh) +
					                             ", the first of the 1 to 99 the game's editor offers that no area has: a "
					                             "trigger or an action can then name it. Whatever names zone " +
					                             std::to_string(area.id) + " still finds the earlier one.",
					                     {set}});
					taken.insert(fresh);
				}
			} else {
				first.emplace(area.id, row);
			}
			if (zone_box_flat(area))
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
				else if (trigger_reads_sub_type(main)) {
					// Event's and SecondTimeThrough's cases read no sub-type: any is theirs.
					const MissionChoices subs = trigger_sub_types(main);
					bool known = subs.count == 0;
					for (size_t s = 0; s < subs.count; ++s) known = known || subs.rows[s].value == trigger.sub_type;
					if (!known)
						on(address, MissionFinding::TriggerType, DiagnosticSeverity::Warning,
						   "Sub-type " + std::to_string(trigger.sub_type) + " is none the game's evaluator has a case for under main type " +
						           std::to_string(main) + ": the trigger reads false.",
						   "sub_type");
				}
				// The Single tests whose SSN lookup scans fewer pools than the lookups by SSN do, each false
				// for an SSN it finds no row of: the engine's table (mission::trigger_ssn_pools, its
				// witnesses there); the alive test (SingleDestroyed, SingleAlive) reads a marker's SSN as not
				// alive.
				if (const uint8_t pools = trigger_ssn_pools(trigger)) {
					using S = bms::SingleTriggerType;
					const bool alive = trigger.sub_type == int32_t(S::SingleDestroyed) || trigger.sub_type == int32_t(S::SingleAlive);
					unscanned(address, trigger.param1, pools, alive ? "the alive test" : "the test",
					          alive ? "SingleAlive reads false and SingleDestroyed true for it." : "it reads false.");
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
				// The actions whose SSN lookup scans fewer pools than the lookups by SSN do: the engine's
				// table (mission::action_ssn_pools, its witnesses there); a medevac's or a flyover's patient
				// lookup starts no operation when it finds none.
				if (const uint8_t pools = action_ssn_pools(action)) {
					const bool patient = action.action_type == bms::ActionType::Teammates;
					unscanned(address, action.param1, pools, patient ? "the operation's patient lookup" : "the action",
					          patient ? "it finds no patient and starts no operation." : "it finds no record and does nothing.");
				}
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
		// A mission of no game mode bit plays as Co-op 0x10020, the single-player family [orig:
		// Game_StartMission @0x524ce1..0x524d01; SinglePlayer_PopulateMissionList @0x5618b3]: its player
		// starts at a marker of type 6094 (until the team's first death), else at one of type 6001, else
		// stays at the map's origin [orig: Server_PositionPlayerForSpawn @0x50CF60, the lookups @0x50D202 and
		// @0x50D2DB over pool 3, the origin @0x50D3A7..0x50D46F; runtime/world/spawn_select.cpp].
		if (!modes) {
			const world::StartMarkerTypes starts = world::start_marker_types(game_type::for_mission_attribs(0u), 1);
			bool start = false;
			for (const Node *row : document.rows_of(K::Marker)) {
				const int32_t type = static_cast<const EntityRow &>(*row).native.type_id;
				start = start || type == starts.primary || type == starts.fallback;
			}
			if (!start)
				on(address, MissionFinding::NoStart, DiagnosticSeverity::Warning,
				   "No marker is a start (item 106094, the insertion point, or 106001): the mission has no game mode, so the "
				   "game plays it as single player, and its player starts at the map's origin.",
				   "attrib_flags");
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
