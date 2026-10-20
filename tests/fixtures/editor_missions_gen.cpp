// Generator + guard for fixtures/editor/missions: the missions the editor's mission tests sweep
// (tests/editor/mission_sweep.h) in place of the 115 the game install ships. The
// record kinds and the field forms those 115 hold were enumerated once over them (2026-10-19, read
// only); each of the three missions below holds them as small integer data, authored through the
// bms_edit free functions and the event chains and written by bms::write, so what the editor legs
// hold of every shipped record (open, worded, set to itself, in the viewport) they hold of these. No
// retail byte is carried.
//
//   logic.bms    every trigger and action type the format's tables name (the evaluator's and the
//                dispatcher's cases, mission_params.h: the shipped events' 26 trigger and 60 action
//                types among them), and for each
//                of those 86 a record per form its parameters take in
//                the shipped records: an SSN of each pool's record (one past the 128 rows a sees,
//                targeted or visited record keys, section 3a), the player's 10000, an SSN or a zone no
//                record holds, a path with stops and one without, the commands 123..127 (a Redirect's
//                third parameter an entity beside 123..125), a stop at 0, -1, inside and past the
//                path's stops, each Bool, Team and zero value the shipped records hold; every unread
//                parameter -1 and every unread sub-type 0, as every shipped record holds them
//                (mission::kUnreadParam). Its events: one of each recipe's composition
//                (mission_recipes.h), the condition flags the shipped triggers hold (0, negated, or,
//                negated or, xor), the event flags (0, ResetAfter, PreMission and the internal 0x10
//                and 0x20), waits to 505 steps and repeats, events with no trigger and with no action,
//                one holding 16 triggers and one 20 actions (the most a shipped event holds of each).
//   records.bms  the entities, paths, areas, boxes, header and loadout in each form the shipped records
//                take: a team-deathmatch header with its option bits, a full designer slot (CP19.bms),
//                bytes past the terrain's NUL (TDH_I3A.bms, TKH_I3A.bms: 6..8 of the slot), a nonzero
//                header word @260 (00TRb.bms), win slots ending at a 0 with a 255 past it; every pool's
//                AI attribute sets, teams, groups and name slots; an organic on a path, one starting at
//                or past its path's stops, on an empty path, on each SSN command with its rider an SSN,
//                one naming none and 0; items and buildings on an empty path; waypoint markers named
//                -1, 0 and N, advancing on -1 and an event, numbered sparsely along their path; location
//                markers named 0 and N; a numbered (6088), a KOTH centre (6006) and the team starts
//                (6003, 6004); SSNs past 128, negative eulers and yaws, SSNs two and three records
//                carry; paths of one stop, of 39 (CP19.bms's path 6, past its 32 slots) and of 19 with a
//                stale slot word past them (09TR.bms's path 13), the path flags 1, 2 and 3; the three
//                area flags; boxes of type 1 (-2, -1) and 5; a loadout of three- and four-string records
//                naming weapons the project has and has not; the events whose stops, waypoints and
//                objectives these name.
//   scale.bms    every table at the most records any shipped mission holds in it (291 items
//                (ASH_I1gA.bms), 1200 buildings (TKX_C1A.bms), 767 markers (CP09.bms), 230 organics
//                (CP14.bms), 37 areas (CP01.bms), 229 events holding 515 triggers and 592 actions
//                (05TR.bms), 667 stops (CP08.bms) on 83 paths (CP15.bms), 12 boxes (CP18.bms)), so it is
//                at least as large as the largest shipped (CP15.bms: 2450 rows; ASH_I1gA.bms: 2193
//                entities); its triggers and actions logic.bms's forms in turn.
//
// Default: rebuild in memory and byte-compare the committed files. `--write` (re)writes them.
#include "common/test_paths.h"

#include <formats/def/reserved_items.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/file_io.h"

namespace {

namespace bms = opennova::bms;
namespace mission = opennova::mission;
using mission::EntityKind;
using mission::ParamKind;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// --- the item ids the missions place (tests/editor's sweep authors a catalog of them) ---------------------

constexpr int kSoldier = 105311, kBuggy = 101291, kBarrel = 105002, kTower = 102001, kAlpha = 100001;
constexpr int kWaypoint = mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT;
constexpr int kLocation = mission::kItemIdOffset + opennova::def::DEF_TYPE_NAMED_LOCATION;
constexpr int kKothCentre = mission::kItemIdOffset + opennova::def::DEF_TYPE_KOTH_CENTRE;
constexpr int kNumbered = mission::kItemIdOffset + 6088; // a teleport target / effect marker by its wp_number
constexpr int kTeamStart1 = mission::kItemIdOffset + 6003, kTeamStart2 = mission::kItemIdOffset + 6004;

// --- logic.bms's records: the SSNs, zones and paths its parameters name -----------------------------------

constexpr int32_t U = mission::kUnreadParam;
constexpr int32_t kOrganic = 11, kOrganic2 = 12, kOrganicHigh = 200; // groups 1, 2, 3
constexpr int32_t kItem = 21, kItem2 = 22, kItemHigh = 300;
constexpr int32_t kBuilding = 31;
constexpr int32_t kMarker = 46; // a plain marker (the alive test never scans the markers: CP13.bms's SSN 2072)
constexpr int32_t kPlayer = mission::kPlayerSsn;
constexpr int32_t kNoEntity = 4321, kNoEntityLow = 99; // no record holds either
constexpr int32_t kZone = 20, kNoZone = 77;
constexpr int32_t kPath = 1, kEmptyPath = 2;
constexpr int32_t kStopPast = 6; // past path 1's four stops

// One record of a type: its parameters in the slots the type reads (an unread slot is written -1).
struct Form {
	bool action;
	int32_t type, sub;
	int32_t p[4];
};
constexpr bool T = false, A = true;

// The forms the shipped records' parameters take, a record per form, by type (the 26 trigger and 60
// action types the shipped events use; the classes of each slot's values over the 115 missions).
const Form kForms[] = {
	// Group triggers.
	{T, 1, 1, {1, 2}},
	{T, 1, 4, {1}},
	{T, 1, 5, {2}},
	{T, 1, 6, {1, 2}},
	{T, 1, 7, {1, kPath, 2}}, {T, 1, 7, {2, kEmptyPath, 0}}, {T, 1, 7, {1, kPath, kStopPast}},
	{T, 1, 10, {1, kZone}}, {T, 1, 10, {2, kNoZone}},
	{T, 1, 12, {1, 3}},
	{T, 1, 13, {1, 2}},
	// Single triggers: an SSN of each pool, past 128 where the relation records key it, the player, none.
	{T, 2, 1, {kOrganic, 1}}, {T, 2, 1, {kOrganicHigh, 2}},
	{T, 2, 4, {kItem}}, {T, 2, 4, {kNoEntity}}, {T, 2, 4, {kOrganic}}, {T, 2, 4, {kBuilding}}, {T, 2, 4, {kPlayer}},
	{T, 2, 4, {kMarker}},
	{T, 2, 5, {kPlayer}}, {T, 2, 5, {kNoEntity}}, {T, 2, 5, {kOrganic}}, {T, 2, 5, {kItem}}, {T, 2, 5, {kBuilding}},
	{T, 2, 7, {kOrganic, kPath, 1}}, {T, 2, 7, {kNoEntityLow, kEmptyPath, 0}}, {T, 2, 7, {kItem, kPath, kStopPast}},
	{T, 2, 7, {kItemHigh, kPath, 3}},
	{T, 2, 10, {kItem, kZone}}, {T, 2, 10, {kPlayer, kNoZone}}, {T, 2, 10, {kNoEntity, kZone}}, {T, 2, 10, {kOrganic, kZone}},
	{T, 2, 42, {kItem, kItem2}}, {T, 2, 42, {kPlayer, kItem}}, {T, 2, 42, {kOrganic, kItem}},
	{T, 2, 43, {kItem, kItem2, 25}}, {T, 2, 43, {kPlayer, kItem, 50}},
	{T, 2, 45, {kOrganic, kPlayer, 30}},
	// Event, variable and player triggers.
	{T, 3, 0, {0}}, {T, 3, 0, {1}},
	{T, 4, 1, {1, 5}}, {T, 4, 1, {2, 0}},
	{T, 4, 5, {1, 3}},
	{T, 7, 35, {1}},
	{T, 7, 36, {10}},
	{T, 7, 37, {kZone}},
	{T, 7, 38, {kItem}}, {T, 7, 39, {kItem}}, {T, 7, 40, {kItem}}, {T, 7, 41, {kItem}},
	// Actions: the group's and the entity's.
	{A, 0, 0, {}},
	{A, 1, 0, {1, kPath, 0}}, {A, 1, 0, {2, kEmptyPath, -1}}, {A, 1, 0, {1, kPath, 2}}, {A, 1, 0, {1, 125, kItem}},
	{A, 1, 0, {1, 124, kItem}}, {A, 1, 0, {3, 123, kItem}}, {A, 1, 0, {1, 127, -1}}, {A, 1, 0, {1, 126, 0}},
	{A, 1, 0, {2, 0, 0}},
	{A, 2, 0, {1}},
	{A, 3, 2, {1, 0}}, {A, 3, 5, {1}}, {A, 3, 6, {2}}, {A, 3, 8, {1, 60}}, {A, 3, 15, {1, 0}}, {A, 3, 15, {2, 1}},
	{A, 3, 21, {1, 1}}, {A, 3, 28, {1, 2}}, {A, 3, 29, {1, 20}}, {A, 3, 29, {2, 0}}, {A, 3, 30, {1, 15}},
	{A, 3, 30, {2, 0}}, {A, 3, 41, {1, 200}}, {A, 3, 42, {1, 50, 300}}, {A, 3, 43, {1, 1}}, {A, 3, 43, {2, 0}},
	{A, 3, 44, {1, kPlayer}}, {A, 3, 45, {1, 1}}, {A, 3, 45, {2, 0}},
	{A, 4, 0, {3}},
	{A, 5, 1, {1, 7}}, {A, 5, 1, {2, 0}}, {A, 5, 4, {1}},
	{A, 7, 0, {1, 0}}, {A, 7, 0, {2, 1}},
	{A, 8, 0, {}}, {A, 9, 0, {}},
	{A, 14, 0, {1}}, {A, 15, 0, {1}},
	{A, 16, 0, {1, 2}}, {A, 16, 0, {2, 1}}, {A, 16, 0, {3, 0}},
	{A, 17, 0, {1, 2}},
	{A, 18, 0, {1, 1}},
	{A, 19, 0, {kItem, kPath, 0}}, {A, 19, 0, {kOrganic, 125, kItem}}, {A, 19, 0, {kNoEntity, 127, -1}},
	{A, 19, 0, {kBuilding, 123, kBuilding}}, {A, 19, 0, {kOrganic, 124, kNoEntity}}, {A, 19, 0, {kOrganic, 125, 0}},
	{A, 19, 0, {kOrganic, 0, 2}}, {A, 19, 0, {kItem, kEmptyPath, 0}}, {A, 19, 0, {kOrganic2, kPath, -1}},
	{A, 20, 0, {kNoEntity}}, {A, 20, 0, {kItem}}, {A, 20, 0, {kBuilding}}, {A, 20, 0, {kOrganic}},
	{A, 21, 2, {kOrganic, 0}}, {A, 21, 5, {kOrganic}}, {A, 21, 6, {kOrganic2}}, {A, 21, 8, {kNoEntity, 50}},
	{A, 21, 8, {kOrganic, 70}}, {A, 21, 15, {kOrganic, 0}}, {A, 21, 15, {kNoEntity, 1}}, {A, 21, 28, {kItem, 3}},
	{A, 21, 29, {kItem, 40}}, {A, 21, 29, {kItem, 0}}, {A, 21, 30, {kItem, 30}}, {A, 21, 30, {kItem2, 0}},
	{A, 21, 41, {kNoEntity, 150}}, {A, 21, 41, {kOrganic, 100}}, {A, 21, 42, {kNoEntity, 20, 200}},
	{A, 21, 42, {kOrganic, 10, 100}}, {A, 21, 43, {kOrganic, 1}}, {A, 21, 43, {kNoEntity, 0}},
	{A, 21, 43, {kItem, 0}}, {A, 21, 43, {kBuilding, 1}}, {A, 21, 45, {kOrganic, 1}},
	{A, 22, 0, {kItem}}, {A, 22, 0, {kNoEntity}}, {A, 22, 0, {kMarker}}, {A, 22, 0, {kOrganic}}, {A, 22, 0, {kBuilding}},
	{A, 24, 0, {kItem, 2}}, {A, 24, 0, {kOrganic, 1}},
	{A, 25, 0, {kOrganic, 2}}, {A, 25, 0, {kOrganic2, 0}},
	{A, 26, 0, {kOrganic, 1}}, {A, 26, 0, {kItem, 1}},
	{A, 27, 0, {1}},
	{A, 32, 0, {1}},
	{A, 33, 0, {kItem}}, {A, 33, 0, {kOrganic}},
	{A, 34, 0, {0}}, {A, 34, 0, {1}},
	{A, 35, 0, {1, 1}}, {A, 35, 0, {2, 0}},
	{A, 36, 0, {1, 0}}, {A, 36, 0, {1, 1}},
	{A, 40, 0, {0}}, {A, 40, 0, {1}},
	{A, 42, 0, {kOrganic, kPlayer}}, {A, 42, 0, {kNoEntity, kPlayer}},
	{A, 43, 0, {kOrganic, kOrganic2}}, {A, 43, 0, {kOrganic, kBuilding}},
	{A, 45, 0, {kNoEntity, 1}}, {A, 45, 0, {kOrganic, 2}},
	{A, 46, 0, {1, kPlayer}},
	{A, 47, 0, {1, kPlayer}}, {A, 47, 0, {2, kItem}}, {A, 47, 0, {3, kBuilding}}, {A, 47, 0, {1, kNoEntity}},
	{A, 48, 0, {1, 2}}, {A, 49, 0, {1, 2}},
};

// A plain value of a kind, for a type the shipped records hold none of.
int32_t plain(ParamKind kind) {
	switch (kind) {
	case ParamKind::Group: return 1;
	case ParamKind::Entity: return kOrganic;
	case ParamKind::Zone: return kZone;
	case ParamKind::Event: return 0;
	case ParamKind::Path: return kPath;
	case ParamKind::PathNode: return 0;
	case ParamKind::MissionVar: return 1;
	case ParamKind::Dialog: return 1;
	case ParamKind::Count: return 2;
	case ParamKind::Hp: return 50;
	case ParamKind::DistanceM: return 25;
	case ParamKind::Seconds: return 10;
	case ParamKind::SpeedKph: return 20;
	case ParamKind::Bool: return 1;
	case ParamKind::Team: return 1;
	case ParamKind::SubGoal: return 1;
	case ParamKind::Bit: return 3;
	case ParamKind::HudTimer: return 2;
	case ParamKind::LightChannel: return 1;
	case ParamKind::TeleportTarget: return 1;
	case ParamKind::WpNumber: return 1;
	case ParamKind::Raw: return 5;
	case ParamKind::Unused: break;
	}
	return U;
}

bms::Trigger trigger_of(const Form &form) {
	bms::Trigger out{};
	out.main_type = static_cast<bms::TriggerMainType>(form.type);
	out.sub_type = form.sub;
	for (int slot = 0; slot < 4; ++slot) out.set_param(slot, form.p[slot]);
	for (int slot = 0; slot < 4; ++slot)
		if (mission::trigger_param_kind(out, slot) == ParamKind::Unused) out.set_param(slot, U);
	return out;
}
bms::Action action_of(const Form &form) {
	bms::Action out{};
	out.action_type = static_cast<bms::ActionType>(form.type);
	out.action_sub_type = form.sub;
	for (int slot = 0; slot < 4; ++slot) out.set_param(slot, form.p[slot]);
	for (int slot = 0; slot < 4; ++slot)
		if (mission::action_param_kind(out, slot) == ParamKind::Unused) out.set_param(slot, U);
	return out;
}

// Every type the format's tables name (the ones "Add trigger" and "Add action" offer, the editor
// test's test_types): a trigger's main type by each sub-type its evaluator reads (Event's and
// SecondTimeThrough's one, "Null"), an action's type by each sub-type that selects what it does, but
// 0 and the teammates' 3 (no arm), and a type whose sub-type selects nothing by 0. A type kForms holds
// none of takes a record of plain values.
std::vector<Form> all_forms() {
	std::vector<Form> out(std::begin(kForms), std::end(kForms));
	std::set<std::pair<int, std::pair<int32_t, int32_t>>> held;
	for (const Form &form : out) held.insert({form.action, {form.type, form.sub}});
	const auto add = [&](bool action, int32_t type, int32_t sub) {
		if (held.count({action, {type, sub}})) return;
		Form form{action, type, sub, {}};
		bms::Trigger t{};
		bms::Action a{};
		t.main_type = static_cast<bms::TriggerMainType>(type);
		t.sub_type = sub;
		a.action_type = static_cast<bms::ActionType>(type);
		a.action_sub_type = sub;
		for (int slot = 0; slot < 4; ++slot)
			form.p[slot] = plain(action ? mission::action_param_kind(a, slot) : mission::trigger_param_kind(t, slot));
		out.push_back(form);
	};
	const mission::MissionChoices mains = mission::trigger_main_types();
	for (size_t i = 0; i < mains.count; ++i) {
		const int32_t main = int32_t(mains.rows[i].value);
		const mission::MissionChoices subs = mission::trigger_sub_types(main);
		for (size_t s = 0; s < subs.count; ++s) {
			const int32_t sub = int32_t(subs.rows[s].value);
			if (sub == 0 && mission::trigger_reads_sub_type(main)) continue;
			add(T, main, sub);
		}
	}
	const mission::MissionChoices types = mission::action_types();
	for (size_t i = 0; i < types.count; ++i) {
		const int32_t type = int32_t(types.rows[i].value);
		const mission::MissionChoices subs = mission::action_sub_types(type);
		if (subs.count == 1) {
			add(A, type, 0);
			continue;
		}
		for (size_t s = 0; s < subs.count; ++s) {
			const int32_t sub = int32_t(subs.rows[s].value);
			if (sub == 0 || (type == int32_t(bms::ActionType::Teammates) && sub == 3)) continue;
			add(A, type, sub);
		}
	}
	return out;
}

// --- the parts every mission is made of -------------------------------------------------------------------

// A record of `kind` naming `item` with the SSN `ssn` at (x, y) facing `yaw`: the original editor's
// initializer's values (bms_edit's new_entity, D-MIS-10), the map symbol the properties dialog leaves
// when none is chosen. Its index in its pool.
size_t place(bms::File &doc, EntityKind kind, int item, int32_t ssn, double x, double y, int yaw = 0) {
	bms::Entity made = mission::new_entity(kind, item, ssn);
	made.x = bms::to_fixed_16_16(x);
	made.y = bms::to_fixed_16_16(y);
	made.yaw = static_cast<int16_t>(yaw);
	made.map_symbol = 255;
	std::vector<bms::Entity> &pool = *mission::entities(doc, kind);
	pool.push_back(made);
	return pool.size() - 1;
}
bms::Entity &at(bms::File &doc, EntityKind kind, size_t index) { return (*mission::entities(doc, kind))[index]; }

bms::AreaTrigger area(int32_t id, double x0, double x1, double y0, double y1, uint32_t flags, double z0 = 0,
                      double z1 = 0) {
	return bms::AreaTrigger{id,
	                        bms::to_fixed_16_16(x0),
	                        bms::to_fixed_16_16(x1),
	                        bms::to_fixed_16_16(y0),
	                        bms::to_fixed_16_16(y1),
	                        bms::to_fixed_16_16(z0),
	                        bms::to_fixed_16_16(z1),
	                        flags};
}

bms::BoundingBox box(double x0, double y0, double x1, double y1, int32_t type, int32_t ref) {
	return bms::BoundingBox{bms::to_fixed_16_16(x0), bms::to_fixed_16_16(y0), 0, bms::to_fixed_16_16(x1),
	                        bms::to_fixed_16_16(y1), bms::to_fixed_16_16(30.0), type, ref, 0};
}

bool header(bms::File &doc, const char *name, const char *designer, const char *terrain, std::string &err) {
	mission::BlankMission blank;
	blank.name = name;
	blank.designer = designer;
	blank.terrain = terrain;
	blank.environment = "synth_full";
	return mission::make_blank(doc, blank, err);
}

// A loadout: each entry a weapon, its two clip counts and, where `flags` is not null, its fourth string
// (577 of the 838 shipped records write three, bms.h).
struct Kit {
	const char *name, *primary, *secondary, *flags;
};
bool loadout(bms::File &doc, std::initializer_list<Kit> kits, std::string &err) {
	std::vector<mission::WeaponLoadoutEntry> entries;
	for (const Kit &kit : kits) {
		mission::WeaponLoadoutEntry entry;
		entry.name = kit.name;
		entry.ammo_primary = kit.primary;
		entry.ammo_secondary = kit.secondary;
		entry.has_flags = kit.flags != nullptr;
		entry.flags = kit.flags ? kit.flags : "-1";
		entries.push_back(entry);
	}
	return mission::set_weapon_loadout(doc, entries, err);
}

// The event chains joined into the file's tables (every run where its event stands, the corpus's layout).
void join(bms::File &doc, const std::vector<mission::EventChain> &chains) {
	mission::join_event_chains(chains, doc);
	mission::sync_counts(doc);
}

mission::EventChain chain(uint32_t flags = 0, int32_t delay = 0, int32_t repeat = 0) {
	mission::EventChain out{};
	out.event.flags = static_cast<bms::EventFlags>(flags);
	out.event.delay = delay;
	out.event.reset_after = repeat;
	return out;
}

// --- logic.bms ---------------------------------------------------------------------------------------------

// The places logic.bms's records stand (and scale.bms's first ones): the SSNs, zones and paths kForms names.
bool logic_context(bms::File &doc, std::string &err) {
	size_t i = place(doc, EntityKind::Organic, kSoldier, kOrganic, 30, -200, 45);
	at(doc, EntityKind::Organic, i).group_id = 1;
	at(doc, EntityKind::Organic, i).team = 2;
	i = place(doc, EntityKind::Organic, kSoldier, kOrganic2, -60, 220, 225);
	at(doc, EntityKind::Organic, i).group_id = 2;
	at(doc, EntityKind::Organic, i).team = 2;
	i = place(doc, EntityKind::Organic, kSoldier, kOrganicHigh, 90, 120, 180);
	at(doc, EntityKind::Organic, i).group_id = 3;
	at(doc, EntityKind::Organic, i).team = 1;
	i = place(doc, EntityKind::Item, kBuggy, kItem, -100, -100, 0);
	at(doc, EntityKind::Item, i).group_id = 1;
	place(doc, EntityKind::Item, kBarrel, kItem2, 0, 50, 90);
	place(doc, EntityKind::Item, kBarrel, kItemHigh, 120, -40, 180);
	place(doc, EntityKind::Building, kTower, kBuilding, -300, 200, 270);
	// Path 1's four waypoint markers round a square (a path's stops are its 6005 markers', D-MIS-6).
	const double corners[4][2] = {{-200, -200}, {200, -200}, {200, 200}, {-200, 200}};
	for (int c = 0; c < 4; ++c) place(doc, EntityKind::Marker, kWaypoint, 41 + c, corners[c][0], corners[c][1]);
	if (!mission::set_waypoint_path(doc, size_t(kPath), {0, 1, 2, 3}, 0, err)) return false;
	place(doc, EntityKind::Marker, kAlpha, kMarker, 0, 0);
	// Teleport target and effect marker 1 (a type-6088 marker's wp_number).
	i = place(doc, EntityKind::Marker, kNumbered, 47, 300, 0);
	at(doc, EntityKind::Marker, i).wp_number = 1;
	// The starts: single player's insertion point (6001), and team 1's and team 2's (6003, 6004).
	place(doc, EntityKind::Marker, mission::kItemIdOffset + 6001, 48, -450, 0);
	place(doc, EntityKind::Marker, kTeamStart1, 49, -450, 50);
	place(doc, EntityKind::Marker, kTeamStart2, 50, 450, 50);
	// Zone 20 round the middle; zone 30 the mission's boundary; zone 40 bounded in z.
	doc.area_triggers.push_back(area(kZone, -150, 150, -150, 150, 0, -20, 40));
	doc.area_triggers.push_back(area(30, -512, 512, -512, 512, bms::AreaTrigger::kFlagMissionArea));
	doc.area_triggers.push_back(area(40, 160, 260, -60, 60, bms::AreaTrigger::kFlagConstrainZ, -10, 30));
	mission::sync_counts(doc);
	return true;
}

// The chains of logic.bms: one event of each recipe's composition first, then every other trigger four to
// an event (the shipped condition flags in turn) and every other action six to one, one event holding 16
// triggers and none of the actions, one 20 actions; the event flags, waits and repeats in turn.
std::vector<mission::EventChain> logic_chains(const std::vector<Form> &forms) {
	std::vector<bms::Trigger> triggers;
	std::vector<bms::Action> actions;
	for (const Form &form : forms) {
		if (form.action) actions.push_back(action_of(form));
		else triggers.push_back(trigger_of(form));
	}
	// The recipes' triggers, each taken out of the list: the first record of the type (player_in_area's the
	// player's).
	const auto take = [&](int32_t type, int32_t sub, int32_t param1 = INT32_MIN) {
		for (size_t i = 0; i < triggers.size(); ++i)
			if (int32_t(triggers[i].main_type) == type && triggers[i].sub_type == sub &&
			    (param1 == INT32_MIN || triggers[i].param1 == param1)) {
				const bms::Trigger out = triggers[i];
				triggers.erase(triggers.begin() + std::ptrdiff_t(i));
				return out;
			}
		return bms::Trigger{};
	};
	std::vector<mission::EventChain> out;
	const auto with = [&](mission::EventChain made, std::initializer_list<bms::Trigger> held) {
		made.triggers.assign(held.begin(), held.end());
		out.push_back(made);
	};
	with(chain(), {});                                                             // right_away
	with(chain(), {take(2, 4)});                                                   // destroyed
	with(chain(), {take(1, 7)});                                                   // group_at_stop
	with(chain(), {take(1, 10)});                                                  // group_in_area
	with(chain(), {take(3, 0)});                                                   // event_fired
	with(chain(), {take(1, 6)});                                                   // group_lost
	with(chain(), {take(1, 4)});                                                   // group_wiped_out
	with(chain(uint32_t(bms::EventFlags::PreMission)), {});                        // at_start
	with(chain(), {take(4, 1)});                                                   // variable_equals
	with(chain(uint32_t(bms::EventFlags::ResetAfter), 0, 5), {take(2, 10, kPlayer)}); // player_in_area
	// The condition flags the shipped triggers hold, in turn: 0, negated, or, negated or, xor.
	const int32_t conditions[] = {0, bms::Trigger::kConditionNegated, bms::Trigger::kConditionOr,
	                              bms::Trigger::kConditionNegated | bms::Trigger::kConditionOr, bms::Trigger::kConditionXor};
	for (size_t i = 0; i < triggers.size(); ++i) triggers[i].condition_flags = conditions[i % 5];
	// One event of 16 triggers and no action.
	mission::EventChain sixteen = chain();
	for (size_t i = 0; i < 16 && !triggers.empty(); ++i) {
		sixteen.triggers.push_back(triggers.back());
		triggers.pop_back();
	}
	out.push_back(sixteen);
	// One event of 20 actions and no trigger.
	mission::EventChain twenty = chain(0, 20);
	for (size_t i = 0; i < 20 && !actions.empty(); ++i) {
		twenty.actions.push_back(actions.back());
		actions.pop_back();
	}
	out.push_back(twenty);
	// The rest, four triggers and six actions an event. The flags, waits and repeats the shipped events hold
	// in turn: 0 most, ResetAfter with a repeat, the internal 0x10 and 0x20 (kept as read, bms.h); a wait
	// of 0 to 505 steps (the most a shipped event waits).
	const uint32_t flags[] = {0, 0, uint32_t(bms::EventFlags::ResetAfter), 0, 0x10u, 0, 0x20u};
	const int32_t delays[] = {0, 5, 20, 0, 505, 1, 0};
	size_t t = 0, a = 0, n = 0;
	while (t < triggers.size() || a < actions.size()) {
		mission::EventChain made = chain(flags[n % 7], delays[n % 7], flags[n % 7] == uint32_t(bms::EventFlags::ResetAfter) ? 10 : 0);
		for (size_t i = 0; i < 4 && t < triggers.size(); ++i) made.triggers.push_back(triggers[t++]);
		for (size_t i = 0; i < 6 && a < actions.size(); ++i) made.actions.push_back(actions[a++]);
		out.push_back(made);
		++n;
	}
	// The recipe events' actions: the first ten events each kill a group or re-arm the first.
	for (size_t i = 0; i < 10; ++i) {
		bms::Action action{};
		action.action_type = i % 2 ? bms::ActionType::KillGroup : bms::ActionType::ResetEvent;
		action.param1 = i % 2 ? 2 : 0;
		action.param2 = action.param3 = action.param4 = U;
		out[i].actions.push_back(action);
	}
	return out;
}

bool build_logic(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	if (!header(doc, "Editor Logic", "OpenNova", "Tmap", err)) return false;
	// An advance-and-secure mission with the water and fog overrides and single-player respawn; no win or lose
	// slot names a directive (every one 255, make_blank's, as 56 shipped missions' first win slot holds).
	bms::set_game_mode(doc.header.attrib_flags, uint32_t(bms::AttribFlags::AdvanceAndSecure));
	doc.header.attrib_flags = static_cast<bms::AttribFlags>(uint32_t(doc.header.attrib_flags) | 0x43u);
	if (!logic_context(doc, err)) return false;
	join(doc, logic_chains(all_forms()));
	if (!loadout(doc, {{"WPN_M4AUTO", "6", "-1", "-1"}, {"WPN_M9Beretta", "-1", "-1", "-1"}, {"WPN_KNIFE", "-1", "-1", nullptr}}, err))
		return false;
	return bms::write(doc, bytes, err);
}

// --- records.bms -------------------------------------------------------------------------------------------

bool build_records(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	// The designer fills its 32 bytes with no NUL (CP19.bms, TDH_I3A.bms, TKH_I3A.bms).
	if (!header(doc, "Editor Records", "OpenNova records, thirty-two ch.", "Isle5", err)) return false;
	if (std::strlen("OpenNova records, thirty-two ch.") != sizeof(doc.header.designer)) {
		err = "the designer is not a full slot";
		return false;
	}
	bms::Header &head = doc.header;
	// Bytes past the terrain's NUL, 6..8 of its slot (TDH_I3A.bms and TKH_I3A.bms ship three there), which every
	// reader copies or prints to its first NUL (mission_field.cpp's terrain row).
	head.terrain[6] = 'q';
	head.terrain[7] = 'r';
	head.terrain[8] = 's';
	// The header word @260 the format leaves unnamed, nonzero in six shipped missions (00TRb.bms's 0x01030000).
	head.unknown5[2] = 0x03;
	head.unknown5[3] = 0x01;
	// Team deathmatch with the water and fog distance overrides; a tile set and a briefing.
	bms::set_game_mode(head.attrib_flags, uint32_t(bms::AttribFlags::TeamDeathmatch));
	head.attrib_flags = static_cast<bms::AttribFlags>(uint32_t(head.attrib_flags) | 0x3u);
	head.climate = bms::ClimateType::Jungle;
	head.weather_type = bms::WeatherType::Rainy;
	std::string message;
	if (!mission::set_header_string(doc, "terrain_tile", "Isle5", message) ||
	    !mission::set_header_string(doc, "briefing", "Hold the ridge.", message)) {
		err = message;
		return false;
	}
	// The objectives panel: slots 4, 1 and 2, ended by a 0, a 255 past it; two lose slots ended by a 0.
	const uint8_t win[8] = {4, 1, 2, 0, 0, 0, 0, 255};
	const uint8_t lose[8] = {1, 2, 0, 0, 0, 0, 0, 0};
	std::memcpy(head.win_conditions, win, 8);
	std::memcpy(head.lose_conditions, lose, 8);

	// Organics. The walker on path 1 at its first stop; one at its third; one starting at the path's count
	// (mission.path_start); one on an empty path (mission.path_empty); one on each SSN command, its rider
	// an item's SSN, one no record holds and 0; one on no path with a nonzero waypoint number; one past
	// SSN 128. Their AI attribute sets, teams, groups and name slots in turn.
	const uint32_t organic_ai[] = {0, 1, 3, 0x800000, 0x800001, 0x200000, 0x804000, 0x200001, 0x4000, 0xA00000};
	struct Walk {
		int32_t ssn;
		uint8_t path;
		int32_t number;
	};
	const Walk walks[] = {{1, 1, 0}, {2, 1, 2}, {3, 1, 9}, {4, 2, 0}, {5, 125, 101}, {6, 124, 4321}, {7, 123, 0},
	                      {8, 0, 3}, {150, 0, 0}, {9, 8, 1}};
	for (size_t w = 0; w < std::size(walks); ++w) {
		const size_t i = place(doc, EntityKind::Organic, kSoldier, walks[w].ssn, -400 + 40.0 * double(w), -300, -90 + 37 * int(w));
		bms::Entity &e = at(doc, EntityKind::Organic, i);
		e.waypoint_id = walks[w].path;
		e.wp_number = walks[w].number;
		e.bmsi_attributes = organic_ai[w % std::size(organic_ai)];
		e.team = uint8_t(w % 3);
		e.group_id = uint8_t(w % 4 == 3 ? 0 : 1 + w % 3);
		e.pitch = int16_t(w % 2 ? -5 : 4);
		e.roll = int16_t(w % 3 ? 3 : -2);
		if (w % 2) mission::set_entity_property_string(doc, EntityKind::Organic, i, "name1", "SHOOTER", message);
		if (w == 3) mission::set_entity_property_string(doc, EntityKind::Organic, i, "name2", "guard", message);
		e.ttool_index = w % 5 == 0 ? -1 : w == 7 ? 2 : 0;
		if (w == 6) e.map_symbol = 3;
	}
	// Items: a vehicle the SSN commands board (SSN 101), one on path 1, two on an empty path, one on no path with a
	// waypoint number, one past SSN 128, one carrying the walker's SSN (the lookups find the organic, an area
	// check both) and one another item's (mission.ssn_duplicate); their attribute sets (Indestructible,
	// EngineRunning, Reflective).
	const uint32_t item_ai[] = {0, 0x800000, 0x200000, 0xA00000, 1, 0x200001, 0x220000, 0x20000};
	const Walk item_walks[] = {{101, 0, 0}, {102, 1, 1}, {103, 2, 0}, {104, 2, 0}, {105, 0, 2}, {230, 0, 0}, {1, 0, 0}, {105, 0, 0}};
	for (size_t w = 0; w < std::size(item_walks); ++w) {
		const size_t i = place(doc, EntityKind::Item, w == 0 ? kBuggy : kBarrel, item_walks[w].ssn, -400 + 50.0 * double(w), -150,
		                       w % 2 ? -45 : 300);
		bms::Entity &e = at(doc, EntityKind::Item, i);
		e.waypoint_id = item_walks[w].path;
		e.wp_number = item_walks[w].number;
		e.bmsi_attributes = item_ai[w];
		e.team = uint8_t((w + 1) % 3);
		e.group_id = uint8_t(w % 3 == 0 ? 0 : 4);
		if (w % 2 == 0) mission::set_entity_property_string(doc, EntityKind::Item, i, "name1", "VEHICLE", message);
		if (w % 3 == 0) mission::set_entity_property_string(doc, EntityKind::Item, i, "name2", "drive", message);
		e.ttool_index = w % 2 ? -1 : 0;
		if (w == 5) e.map_symbol = 12;
	}
	// Buildings: on an empty path (the most shipped: 109), their attribute sets (NoShadow, Reflective,
	// Indestructible), negative yaws, the map symbol chosen and not.
	const uint32_t building_ai[] = {0, 0x800000, 0x1A00000, 0x1200000, 0x200000, 0x1800000, 0x1000000, 0xA00000};
	for (size_t w = 0; w < std::size(building_ai); ++w) {
		const size_t i = place(doc, EntityKind::Building, kTower, int32_t(201 + w), -400 + 60.0 * double(w), 300, w % 2 ? -359 + int(w) : 359 - int(w));
		bms::Entity &e = at(doc, EntityKind::Building, i);
		e.bmsi_attributes = building_ai[w];
		e.team = uint8_t(w % 3);
		if (w < 2) e.waypoint_id = 2;
		if (w % 3 == 1) e.map_symbol = 7;
		if (w % 2) mission::set_entity_property_string(doc, EntityKind::Building, i, "name1", "TOWER", message);
		if (w == 4) mission::set_entity_property_string(doc, EntityKind::Building, i, "name2", "tower", message);
		if (w >= 6) e.group_id = 5;
		e.ttool_index = w % 4 == 0 ? -1 : 0;
	}

	// Markers. Path 1's four waypoints, numbered sparsely (0, 2, 5, 9: a waypoint number at or past the count,
	// 81 shipped), named -1 ("STRWPNAME-01"), 0 and 3, the second advancing on event 2 (73 shipped), the
	// others on none (-1).
	const int32_t sparse[4] = {0, 2, 5, 9};
	for (int c = 0; c < 4; ++c) {
		const size_t i = place(doc, EntityKind::Marker, kWaypoint, 301 + c, -200 + 100.0 * c, -200);
		bms::Entity &e = at(doc, EntityKind::Marker, i);
		e.waypoint_id = 1;
		e.wp_number = sparse[c];
		e.ttool_index = c == 0 ? -1 : c == 3 ? 3 : 0;
		e.wp_adv_trigger = int16_t(c == 1 ? 1 : -1);
		e.bmsi_attributes = c == 2 ? 0x400000u : 0;
	}
	mission::lay_out_waypoint_path(doc, 1);
	// Path 3 of one stop (mission.path_one_shot: the load makes it not loop).
	size_t marker = 0;
	if (!mission::add_waypoint_marker(doc, 3, kWaypoint, {150.0f, 150.0f, 0.0f}, -1, err, &marker)) return false;
	at(doc, EntityKind::Marker, marker).id = 310;
	// Path 4 of 39 stops, past its 32 slots (CP19.bms's path 6: mission.path_count), on a line.
	for (int s = 0; s < 39; ++s) {
		if (!mission::add_waypoint_marker(doc, 4, kWaypoint, {-500.0f + 25.0f * float(s), 400.0f, 0.0f}, -1, err, &marker)) return false;
		at(doc, EntityKind::Marker, marker).id = 400 + s;
	}
	// Path 5 of 19 stops with a stale slot word past them: laid out with 20, its last taken off, the original's
	// rebuild leaving the word it held (09TR.bms's path 13; mission::lay_out_waypoint_path).
	std::vector<int> twenty;
	for (int s = 0; s < 20; ++s) {
		if (!mission::add_waypoint_marker(doc, 5, kWaypoint, {-500.0f + 25.0f * float(s), 450.0f, 0.0f}, -1, err, &marker)) return false;
		at(doc, EntityKind::Marker, marker).id = 450 + s;
		twenty.push_back(int(marker));
	}
	twenty.pop_back();
	if (!mission::set_waypoint_path(doc, 5, twenty, 0, err)) return false;
	// Paths 6, 7 and 8: the player's route (2), a route that does not loop (3), a patrol that does not loop (1).
	const uint32_t path_flags[3] = {2, 3, 1};
	for (int p = 0; p < 3; ++p) {
		std::vector<int> stops;
		for (int s = 0; s < 3; ++s) {
			const size_t i = place(doc, EntityKind::Marker, kWaypoint, 500 + 10 * p + s, 300 + 30.0 * s, -300 - 40.0 * p);
			at(doc, EntityKind::Marker, i).ttool_index = p + s;
			stops.push_back(int(i));
		}
		if (!mission::set_waypoint_path(doc, size_t(6 + p), stops, int(path_flags[p]), err)) return false;
	}
	// Location markers named 0 (LOCATION000) and 2 (LOCATION002 and STRNAME002); a numbered marker (6088, wp
	// number 4); a KOTH centre named 1; the two team starts of team deathmatch; plain markers, one's second
	// name slot set (its group 6, its yaw negative, its general string empty), one advancing on 0 (no waypoint:
	// read on a type-6005 alone), one past SSN 128.
	place(doc, EntityKind::Marker, kLocation, 600, 0, 0);
	size_t i = 0;
	i = place(doc, EntityKind::Marker, kLocation, 601, 50, 0);
	at(doc, EntityKind::Marker, i).name_index = 2;
	i = place(doc, EntityKind::Marker, kNumbered, 602, 100, 0);
	at(doc, EntityKind::Marker, i).wp_number = 4;
	i = place(doc, EntityKind::Marker, kKothCentre, 603, 150, 0);
	at(doc, EntityKind::Marker, i).ttool_index = 1;
	place(doc, EntityKind::Marker, kTeamStart1, 604, -450, -450);
	place(doc, EntityKind::Marker, kTeamStart2, 605, 450, 450);
	i = place(doc, EntityKind::Marker, kAlpha, 606, 200, 0, -120);
	at(doc, EntityKind::Marker, i).group_id = 6;
	std::memset(at(doc, EntityKind::Marker, i).gen_string, 0, sizeof(bms::Entity::gen_string));
	mission::set_entity_property_string(doc, EntityKind::Marker, i, "name2", "flag", message);
	at(doc, EntityKind::Marker, i).team = 1;
	i = place(doc, EntityKind::Marker, kAlpha, 607, 250, 0);
	at(doc, EntityKind::Marker, i).wp_adv_trigger = 0;
	at(doc, EntityKind::Marker, i).team = 2;
	at(doc, EntityKind::Marker, i).bmsi_attributes = 0x800000;
	place(doc, EntityKind::Marker, kAlpha, 140, 300, 0);
	// Two markers carrying the first building's SSN, three records of it (mission.ssn_duplicate twice: the
	// lookups find the building, pools in order).
	place(doc, EntityKind::Marker, kAlpha, 201, 350, 0);
	place(doc, EntityKind::Marker, kAlpha, 201, 400, 0);

	// The areas: zone 1 bounded in z by nothing, zone 2 the mission's boundary, zone 3 constrained in z, zone 99
	// flat in z.
	doc.area_triggers.push_back(area(1, -100, 100, -100, 100, 0, -16, 16));
	doc.area_triggers.push_back(area(2, -1000, 1000, -1000, 1000, bms::AreaTrigger::kFlagMissionArea, -16, 16));
	doc.area_triggers.push_back(area(3, 200, 300, 200, 300, bms::AreaTrigger::kFlagConstrainZ, 0, 25));
	doc.area_triggers.push_back(area(99, -300, -200, 200, 300, 0));
	// The boxes: health drains (type 1, -2 and -1) and locations (type 5).
	doc.bounding_boxes.push_back(box(-50, -50, 50, 50, 1, -2));
	doc.bounding_boxes.push_back(box(60, -50, 160, 50, 1, -1));
	doc.bounding_boxes.push_back(box(-50, 60, 50, 160, 5, 3));
	doc.bounding_boxes.push_back(box(60, 60, 160, 160, 5, 95));
	mission::sync_counts(doc);

	// The events: a group at path 1's third stop sends it on from its fourth; the walker at path 4's 34th
	// (past its slots) sent to its 36th; the objectives shown, won and lost (the directives' and the chat lines'
	// keys); the event waypoint 2 advances on.
	const auto trigger = [](int32_t main, int32_t sub, int32_t p1, int32_t p2 = U, int32_t p3 = U) {
		bms::Trigger t{};
		t.main_type = static_cast<bms::TriggerMainType>(main);
		t.sub_type = sub;
		t.param1 = p1;
		t.param2 = p2;
		t.param3 = p3;
		t.param4 = U;
		return t;
	};
	const auto action = [](int32_t type, int32_t p1, int32_t p2 = U, int32_t p3 = U) {
		bms::Action a{};
		a.action_type = static_cast<bms::ActionType>(type);
		a.param1 = p1;
		a.param2 = p2;
		a.param3 = p3;
		a.param4 = U;
		return a;
	};
	std::vector<mission::EventChain> chains(3, chain());
	chains[0].triggers = {trigger(1, 7, 1, 1, 2)};
	chains[0].actions = {action(1, 1, 1, 3)};
	chains[1].triggers = {trigger(2, 7, 1, 4, 33)};
	chains[1].actions = {action(19, 1, 4, 35), action(35, 4, 1), action(14, 2), action(15, 1), action(36, 1, 1)};
	chains[2].triggers = {trigger(2, 4, 201)};
	chains[2].actions = {action(34, 0)};
	join(doc, chains);
	if (!loadout(doc, {{"WPN_M4AUTO", "6", "-1", "-1"},
	                   {"WPN_CAR15AUTO", "-1", "-1", nullptr},
	                   {"WPN_M9Berreta", "-1", "-1", "1"},
	                   {"WPN_GRENADEHE", "2", "-1", nullptr},
	                   {"WPN_KNIFE", "-1", "-1", "-1"}},
	             err))
		return false;
	return bms::write(doc, bytes, err);
}

// --- scale.bms ---------------------------------------------------------------------------------------------

// The most records a shipped mission holds in each table (the module comment's missions).
constexpr size_t kMostItems = 291, kMostBuildings = 1200, kMostMarkers = 767, kMostOrganics = 230, kMostAreas = 37,
                 kMostEvents = 229, kMostTriggers = 515, kMostActions = 592, kMostStops = 667, kMostPaths = 83,
                 kMostBoxes = 12;

bool build_scale(std::vector<uint8_t> &bytes, std::string &err) {
	bms::File doc;
	// No designer (five shipped missions name none).
	if (!header(doc, "Editor Scale", "", "Tmap", err)) return false;
	// Team king of the hill with night vision and the weather, fog and water overrides; all eight win rows,
	// two lose rows ended by a 0.
	bms::set_game_mode(doc.header.attrib_flags, uint32_t(bms::AttribFlags::TeamKingOfTheHill));
	doc.header.attrib_flags = static_cast<bms::AttribFlags>(uint32_t(doc.header.attrib_flags) | 0x10000Bu);
	const uint8_t win[8] = {1, 2, 3, 4, 5, 6, 7, 8}, lose[8] = {2, 3, 0, 0, 0, 0, 0, 0};
	std::memcpy(doc.header.win_conditions, win, 8);
	std::memcpy(doc.header.lose_conditions, lose, 8);
	// logic.bms's places first (its forms name them), then each pool to its most, on a grid 3 km across.
	if (!logic_context(doc, err)) return false;
	int32_t ssn = 1000;
	const auto grid = [](size_t n, double spacing, double &x, double &y) {
		x = -1500.0 + spacing * double(n % 60);
		y = -1500.0 + spacing * double(n / 60);
	};
	double x = 0, y = 0;
	for (size_t n = doc.organics.size(); n < kMostOrganics; ++n) {
		grid(n, 50, x, y);
		const size_t i = place(doc, EntityKind::Organic, kSoldier, ssn++, x, y + 20, int(n * 7 % 360));
		at(doc, EntityKind::Organic, i).group_id = uint8_t(1 + n % 40);
		at(doc, EntityKind::Organic, i).team = uint8_t(1 + n % 2);
	}
	for (size_t n = doc.items.size(); n < kMostItems; ++n) {
		grid(n, 50, x, y);
		place(doc, EntityKind::Item, n % 5 ? kBarrel : kBuggy, ssn++, x + 10, y, int(n * 11 % 360));
	}
	for (size_t n = doc.buildings.size(); n < kMostBuildings; ++n) {
		grid(n, 50, x, y);
		place(doc, EntityKind::Building, kTower, ssn++, x + 25, y + 25, int(n * 13 % 360) - 180);
	}
	// The paths: path 1 logic.bms's; then paths 3 on, each of eight waypoints (the last of seven), to the most
	// stops and paths.
	size_t stops = mission::waypoint_path_markers(doc, size_t(kPath)).size(), paths = 1;
	for (size_t path = 3; paths < kMostPaths && stops < kMostStops; ++path, ++paths) {
		const size_t take = std::min<size_t>(paths + 1 == kMostPaths ? kMostStops - stops : 8, kMostStops - stops);
		for (size_t s = 0; s < take; ++s) {
			size_t marker = 0;
			grid(stops, 45, x, y);
			if (!mission::add_waypoint_marker(doc, path, kWaypoint, {float(x), float(y - 30), 0.0f}, -1, err, &marker)) return false;
			at(doc, EntityKind::Marker, marker).id = ssn++;
			++stops;
		}
	}
	for (size_t n = doc.markers.size(); n < kMostMarkers; ++n) {
		grid(n, 47, x, y);
		place(doc, EntityKind::Marker, kAlpha, ssn++, x - 20, y, 0);
	}
	for (size_t n = doc.area_triggers.size(); n < kMostAreas; ++n) {
		grid(n * 97, 50, x, y);
		doc.area_triggers.push_back(area(int32_t(41 + n), x, x + 80, y, y + 80, n % 9 == 0 ? bms::AreaTrigger::kFlagConstrainZ : 0, -10, 30));
	}
	for (size_t n = 0; n < kMostBoxes; ++n) {
		grid(n * 131, 50, x, y);
		doc.bounding_boxes.push_back(box(x, y, x + 60, y + 60, n % 2 ? 5 : 1, n % 2 ? int32_t(n) : -1));
	}
	mission::sync_counts(doc);
	// The events: logic.bms's forms in turn, to the most triggers and actions, two or three an event, every
	// group of the organics in turn where a form names group 1.
	const std::vector<Form> forms = all_forms();
	std::vector<bms::Trigger> triggers;
	std::vector<bms::Action> actions;
	for (size_t n = 0; triggers.size() < kMostTriggers || actions.size() < kMostActions; ++n) {
		const Form &form = forms[n % forms.size()];
		if (form.action && actions.size() < kMostActions) actions.push_back(action_of(form));
		if (!form.action && triggers.size() < kMostTriggers) triggers.push_back(trigger_of(form));
	}
	std::vector<mission::EventChain> chains;
	size_t t = 0, a = 0;
	for (size_t e = 0; e < kMostEvents; ++e) {
		mission::EventChain made = chain(e % 9 == 4 ? uint32_t(bms::EventFlags::ResetAfter) : 0, int32_t(e % 6) * 3,
		                                 e % 9 == 4 ? 4 : 0);
		const size_t left = kMostEvents - e;
		const size_t take_t = (kMostTriggers - t + left - 1) / left, take_a = (kMostActions - a + left - 1) / left;
		for (size_t i = 0; i < take_t && t < triggers.size(); ++i) made.triggers.push_back(triggers[t++]);
		for (size_t i = 0; i < take_a && a < actions.size(); ++i) made.actions.push_back(actions[a++]);
		chains.push_back(made);
	}
	join(doc, chains);
	if (!loadout(doc, {{"WPN_M4AUTO", "6", "-1", "-1"}, {"WPN_M9Beretta", "-1", "-1", nullptr}}, err)) return false;
	const bool shape = doc.items.size() == kMostItems && doc.buildings.size() == kMostBuildings &&
	                   doc.markers.size() == kMostMarkers && doc.organics.size() == kMostOrganics &&
	                   doc.area_triggers.size() == kMostAreas && doc.events.size() == kMostEvents &&
	                   doc.triggers.size() == kMostTriggers && doc.actions.size() == kMostActions &&
	                   doc.bounding_boxes.size() == kMostBoxes;
	size_t laid = 0, held = 0;
	for (size_t path = 1; path < doc.waypoint_records.size(); ++path) {
		laid += doc.waypoint_records[path].marker_count;
		held += doc.waypoint_records[path].marker_count != 0;
	}
	if (!shape || laid != kMostStops || held != kMostPaths) {
		err = "the mission is not at the most records of each table";
		return false;
	}
	return bms::write(doc, bytes, err);
}

// The bytes parse back to the same document and write again identically.
bool reparses(const std::vector<uint8_t> &bytes, std::string &err) {
	bms::File back;
	if (!bms::parse(bytes.data(), bytes.size(), back, err)) return false;
	std::vector<uint8_t> again;
	if (!bms::write(back, again, err)) return false;
	if (again != bytes) {
		err = "write(parse(write(file))) differs from write(file)";
		return false;
	}
	return true;
}

int emit(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return 1;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: %s byte-reproducible (%zu bytes)\n", path.c_str(), bytes.size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string root = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/editor/missions/";
	const struct {
		const char *name;
		bool (*build)(std::vector<uint8_t> &, std::string &);
	} missions[] = {{"logic.bms", build_logic}, {"records.bms", build_records}, {"scale.bms", build_scale}};
	int failures = 0;
	for (const auto &m : missions) {
		std::vector<uint8_t> bytes;
		std::string err;
		if (!expect(m.build(bytes, err) && reparses(bytes, err), (std::string(m.name) + ": " + err).c_str())) return 1;
		failures += emit(root + m.name, bytes, write_mode);
	}
	return failures ? 1 : 0;
}
