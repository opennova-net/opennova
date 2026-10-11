// A mission's logic in its views (ADR 0046 S24, In the views), over the minted synth_logic mission.
//
// documents/mission_links: what an event, a trigger or an action names in the mission's space, as the game finds
// it (an SSN's first holder in pool order, a zone's first area, a path's stops, a group's members, a stop by its
// number, the numbered 6088 markers), the distances the near, line-of-sight and sees triggers test, and how many
// events name each record, counted once a state of the document (and the events a badge's hover lists); what a
// pick takes from a mark (a record naming another refused with the record the game finds first, a path picked at a
// stop setting the stop too) and the group New group hands out; over the minted mission with a third event, a
// second zone 20, a second holder of the walker's SSN and two numbered markers authored on top.
//
// S0: the pick a board chip asks of the views (the workspace's document `pick`, session/workspace_parts): a
// trigger's parameter slot set, reported by the record's identity and its slot, refused for a record that is no
// trigger or action and for a slot past 3 or left out, and ended by {}.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_links.h>
#include <editor/documents/mission_table.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/mission_link_shapes.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/workspace_parts.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
namespace bms = opennova::bms;
namespace mission = opennova::mission;

namespace {

// The mission's entry in the workspace's documents, as the view reports it.
JsonValue document_entry(const SessionView &view, const std::string &path) {
	const JsonValue workspace = workspace_to_json(view);
	if (const JsonValue *documents = workspace.get("documents"))
		for (const JsonValue &entry : documents->array)
			if (entry.get_string("path", "") == path) return entry;
	return JsonValue();
}

int test_pick() {
	editor_test::TempProjectDir dir("opennova_mission_links_pick");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Links"));
	editor_test::create_missing_files(session);
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/missions/logic.bms",
	                                     test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const std::string path = mission->path();
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const std::vector<Document::Collection> lists = mission->collections_of({event->id, event->kind, 0});
	TEST_EXPECT(!lists.empty() && !lists[0].ids.empty());
	const NodeId trigger = lists[0].ids[0];
	const Node *organic = mission->rows_of(MissionKind::Organic)[0];
	const auto set = [&](const std::string &pick) {
		const std::string change = "{\"document\": {\"path\": \"" + path + "\", \"pick\": " + pick + "}}";
		std::vector<WorkspaceRefusal> refusals;
		SessionView &view = const_cast<SessionView &>(session.view());
		apply_workspace_change(view, change, refusals);
		return refusals;
	};
	// No pick at first.
	JsonValue entry = document_entry(session.view(), path);
	TEST_EXPECT(entry.get("pick") && entry.get("pick")->is_null());
	// The trigger's param2 asked for.
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + ", \"slot\": 1}").empty());
	const WorkspaceView::Pick &held = session.view().workspace.document(path).pick;
	TEST_EXPECT(held.active() && held.record.child == trigger && held.slot == 1);
	entry = document_entry(session.view(), path);
	const JsonValue *pick = entry.get("pick");
	TEST_EXPECT(pick && pick->get_number("record", 0) == double(trigger) && pick->get_number("slot", -1) == 1);
	// Refused, the pick kept: an entity, a slot past 3.
	TEST_EXPECT(set("{\"record\": " + std::to_string(organic->id) + ", \"slot\": 0}").size() == 1);
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + ", \"slot\": 4}").size() == 1);
	TEST_EXPECT(set("{\"record\": " + std::to_string(trigger) + "}").size() == 1);
	TEST_EXPECT(session.view().workspace.document(path).pick.slot == 1);
	// Ended.
	TEST_EXPECT(set("{}").empty());
	TEST_EXPECT(!session.view().workspace.document(path).pick.active());
	entry = document_entry(session.view(), path);
	TEST_EXPECT(entry.get("pick") && entry.get("pick")->is_null());
	// S24: a stop removed that a record names by its place: the record keeps that number, and the status line counts
	// it (the organic walking path 1 from its stop 1, that stop taken out).
	{
		const NodeAddress walker{organic->id, organic->kind, 0};
		Edit walks;
		walks.address = walker;
		walks.field = "waypoint_id";
		walks.value = int64_t(1);
		Edit from = walks;
		from.field = "wp_number";
		from.value = int64_t(1);
		session.handle(request::edit_record(path, {walks, from}));
		TEST_EXPECT(session.outcome().done());
		const Node *route = mission->rows_of(MissionKind::WaypointPath)[1];
		const std::vector<Document::Collection> stops = mission->collections_of({route->id, route->kind, 0});
		TEST_EXPECT(!stops.empty() && stops[0].ids.size() > 2);
		Edit taken;
		taken.operation = EditOperation::Remove;
		taken.address = {route->id, node_kind(MissionKind::Stop), stops[0].ids[1]};
		session.handle(request::edit_record(path, {taken}));
		TEST_EXPECT(session.outcome().done());
		const std::string &status = session.view().activity.status;
		TEST_EXPECT(status.find("1 record named a removed stop by its place and keeps that number") != std::string::npos);
		Value start;
		TEST_EXPECT(mission->get(walker, "wp_number", start) && std::get<int64_t>(start) == 1);
	}
	return 0;
}

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes) {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, "synth_links.bms", AssetKind::Mission, "jo", error)) {
		std::fprintf(stderr, "the mission does not load: %s\n", error.message.c_str());
		return nullptr;
	}
	return document;
}

const MissionDocument &as_mission(const Document &document) { return dynamic_cast<const MissionDocument &>(document); }

NodeAddress row_at(const MissionDocument &document, MissionKind kind, size_t index) {
	const std::vector<const Node *> rows = document.rows_of(kind);
	return index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
}

NodeAddress child_at(const MissionDocument &document, const NodeAddress &event, MissionKind kind, size_t index) {
	for (const Document::Collection &list : document.collections_of(event))
		if (list.spec.kind == k(kind) && index < list.ids.size()) return {event.row, k(kind), list.ids[index]};
	return NodeAddress();
}

mission::MissionTriggerRecord trigger(int main, int sub, int p1, int p2, int p3 = -1) {
	mission::MissionTriggerRecord made;
	made.main_type = main;
	made.sub_type = sub;
	made.param1 = p1;
	made.param2 = p2;
	made.param3 = p3;
	made.param4 = -1;
	return made;
}

mission::MissionActionRecord action(bms::ActionType type, int p1, int p2 = -1, int p3 = -1) {
	mission::MissionActionRecord made;
	made.action_type = int(type);
	made.param1 = p1;
	made.param2 = p2;
	made.param3 = p3;
	made.param4 = -1;
	return made;
}

// The minted mission with, on top: a 6088 marker numbered 7 twice (an effect plays at both, a teleport
// finds the first), a second organic carrying the walker's SSN in the items' pool (the lookups find the
// organic first), a second area of zone 20 (the resolver takes the first), and event 3: the walker near
// the guard within 25 m, the walker sees the guard within 40 m, the player (SSN 10000, no record) in zone
// 30; then group 1 sent to path 1's stop 2, the walker teleported to marker 7, an effect at marker 7.
struct Fixture {
	std::unique_ptr<Document> document;
	int32_t walker = 0, guard = 0;
	bool load() {
		const std::vector<uint8_t> bytes =
		        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
		bms::File file;
		std::string error;
		if (!bms::parse(bytes.data(), bytes.size(), file, error)) return false;
		walker = file.organics[0].id;
		guard = file.organics[1].id;
		mission::EntityTransform at;
		at.x = 400.0f;
		at.y = 400.0f;
		for (int i = 0; i < 2; ++i) {
			const size_t made = mission::add_entity(file, mission::EntityKind::Marker, mission::kItemIdOffset + 6088, at);
			file.markers[made].wp_number = 7;
			at.x += 20.0f;
		}
		const size_t twin = mission::add_entity(file, mission::EntityKind::Item, 106100, at);
		file.items[twin].id = walker;
		file.area_triggers.push_back(
		        bms::AreaTrigger{20, bms::to_fixed_16_16(300.0), bms::to_fixed_16_16(350.0), bms::to_fixed_16_16(300.0),
		                         bms::to_fixed_16_16(350.0), 0, 0, 0});
		mission::sync_counts(file);
		const size_t event = mission::add_event(file, mission::MissionEventRecord());
		const int single = int(bms::TriggerMainType::Single);
		if (!mission::insert_event_trigger(file, event, 0, trigger(single, 43, walker, guard, 25), error) ||
		    !mission::insert_event_trigger(file, event, 1, trigger(single, 45, walker, guard, 40), error) ||
		    !mission::insert_event_trigger(file, event, 2, trigger(single, 10, mission::kPlayerSsn, 30), error) ||
		    !mission::insert_event_action(file, event, 0, action(bms::ActionType::RedirectGroupTo, 1, 1, 2), error) ||
		    !mission::insert_event_action(file, event, 1, action(bms::ActionType::SingleTeleportAction, walker, 7), error) ||
		    !mission::insert_event_action(file, event, 2, action(bms::ActionType::ParticleEffectAction, 7), error)) {
			std::fprintf(stderr, "event 3: %s\n", error.c_str());
			return false;
		}
		std::vector<uint8_t> out;
		if (!bms::write(file, out, error)) return false;
		document = open(out);
		return document != nullptr;
	}
};

// What an event, a trigger and an action name.
int test_links() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const NodeAddress walker = row_at(m, MissionKind::Organic, 0), guard = row_at(m, MissionKind::Organic, 1);
	const NodeAddress zone20 = row_at(m, MissionKind::Area, 0), zone30 = row_at(m, MissionKind::Area, 1);
	const NodeAddress first = row_at(m, MissionKind::Event, 0), second = row_at(m, MissionKind::Event, 1);
	const NodeAddress third = row_at(m, MissionKind::Event, 2);
	TEST_EXPECT(walker.row && guard.row && third.row && row_at(m, MissionKind::Area, 2).row);
	// Only an event, a trigger or an action is a logic record.
	TEST_EXPECT(mission_logic_record(first) && mission_logic_record(child_at(m, first, MissionKind::Trigger, 0)));
	TEST_EXPECT(!mission_logic_record(walker) && !mission_logic_record(NodeAddress()));
	TEST_EXPECT(!mission_links(m, walker).any() && !mission_links(m, walker).event.row);

	// Event 1: the walker (by its SSN: the organic, the first holder in the lookups' pool order, not the
	// item carrying it too) inside zone 20 (the first area of the id); then event 2 re-armed.
	MissionLinks one = mission_links(m, first);
	TEST_EXPECT(one.event == first && one.links.size() == 3 && one.reaches.empty());
	TEST_EXPECT(!one.links[0].action && one.links[0].kind == mission::ParamKind::Entity && one.links[0].record == walker &&
	            one.links[0].rows == std::vector<NodeId>{walker.row} && one.links[0].value == f.walker);
	TEST_EXPECT(one.links[1].kind == mission::ParamKind::Zone && one.links[1].record == zone20 && one.links[1].slot == 1);
	TEST_EXPECT(one.links[2].action && one.links[2].kind == mission::ParamKind::Event && one.links[2].record == second &&
	            one.links[2].rows.empty());
	TEST_EXPECT(one.links[0].from == child_at(m, first, MissionKind::Trigger, 0) &&
	            one.links[2].from == child_at(m, first, MissionKind::Action, 0));

	// Event 2: event 1 fired; then group 2 killed: the group's record on the mission row and its member,
	// the guard.
	MissionLinks two = mission_links(m, second);
	TEST_EXPECT(two.links.size() == 2 && two.links[0].record == first);
	const MissionLink &group = two.links[1];
	TEST_EXPECT(group.kind == mission::ParamKind::Group && group.value == 2 && group.record.row == m.mission_row()->id &&
	            group.record.kind == k(MissionKind::Group) && group.record.child != 0);
	TEST_EXPECT(group.rows == std::vector<NodeId>{guard.row});

	// Event 3: three triggers (two distances, the player), three actions.
	MissionLinks three = mission_links(m, third);
	TEST_EXPECT(three.reaches.size() == 2);
	const MissionReach &near = three.reaches[0], &sees = three.reaches[1];
	TEST_EXPECT(near.subject == walker.row && near.other == guard.row && near.metres == 25 && !near.sight &&
	            !near.facing && !near.centres && near.from == child_at(m, third, MissionKind::Trigger, 0));
	TEST_EXPECT(sees.subject == walker.row && sees.metres == 40 && sees.sight && sees.facing && sees.centres);
	TEST_EXPECT(!near.player && !sees.player);
	// The metres the game compares: param3's low 16 bits, signed (the call's << 16): 32778 is negative (no distance
	// within it), 65546 wraps to 10.
	{
		Fixture g;
		TEST_EXPECT(g.load());
		const MissionDocument &n = as_mission(*g.document);
		const NodeAddress reach = child_at(n, row_at(n, MissionKind::Event, 2), MissionKind::Trigger, 0);
		Diagnostic error;
		for (const auto &[authored, compared] : {std::pair<int64_t, int64_t>{32778, -32758}, {65546, 10}}) {
			Edit edit;
			edit.address = reach;
			edit.field = "param3";
			edit.value = authored;
			TEST_EXPECT(g.document->apply({edit}, error));
			TEST_EXPECT(mission_links(n, reach).reaches.size() == 1 && mission_links(n, reach).reaches[0].metres == compared);
		}
	}
	// The player's SSN names no record; zone 30 its area.
	const MissionLink *player = nullptr, *zone = nullptr;
	for (const MissionLink &link : three.links) {
		if (link.from == child_at(m, third, MissionKind::Trigger, 2) && link.slot == 0) player = &link;
		if (link.kind == mission::ParamKind::Zone) zone = &link;
	}
	TEST_EXPECT(player && player->kind == mission::ParamKind::Entity && player->value == mission::kPlayerSsn &&
	            !player->record.row && player->rows.empty());
	TEST_EXPECT(zone && zone->record == zone30);
	// The redirect: group 1 (the walker), path 1 (its four stops, in order) and stop 2 (the third marker).
	std::vector<const MissionLink *> redirect;
	for (const MissionLink &link : three.links)
		if (link.from == child_at(m, third, MissionKind::Action, 0)) redirect.push_back(&link);
	TEST_EXPECT(redirect.size() == 3);
	TEST_EXPECT(redirect[0]->kind == mission::ParamKind::Group && redirect[0]->rows == std::vector<NodeId>{walker.row});
	TEST_EXPECT(redirect[1]->kind == mission::ParamKind::Path && redirect[1]->record == row_at(m, MissionKind::WaypointPath, 1));
	std::vector<NodeId> stops;
	for (size_t i = 0; i < 4; ++i) stops.push_back(row_at(m, MissionKind::Marker, i).row);
	TEST_EXPECT(redirect[1]->rows == stops);
	TEST_EXPECT(redirect[2]->kind == mission::ParamKind::PathNode && redirect[2]->record == row_at(m, MissionKind::Marker, 2) &&
	            redirect[2]->rows == std::vector<NodeId>{stops[2]});
	// The teleport finds the first marker numbered 7; the effect plays at both.
	const NodeAddress seven = row_at(m, MissionKind::Marker, 5), again = row_at(m, MissionKind::Marker, 6);
	const MissionLink *teleport = nullptr, *effect = nullptr;
	for (const MissionLink &link : three.links) {
		if (link.kind == mission::ParamKind::TeleportTarget) teleport = &link;
		if (link.kind == mission::ParamKind::WpNumber) effect = &link;
	}
	TEST_EXPECT(teleport && teleport->record == seven && teleport->rows == std::vector<NodeId>{seven.row});
	TEST_EXPECT(effect && effect->record == seven && (effect->rows == std::vector<NodeId>{seven.row, again.row}));

	// A trigger alone: its own links and reach, its event said.
	MissionLinks alone = mission_links(m, child_at(m, third, MissionKind::Trigger, 1));
	TEST_EXPECT(alone.event == third && alone.reaches.size() == 1 && alone.reaches[0].facing && alone.links.size() == 2);
	MissionLinks act = mission_links(m, child_at(m, third, MissionKind::Action, 2));
	TEST_EXPECT(act.event == third && act.reaches.empty() && act.links.size() == 1 && act.links[0].action);
	return 0;
}

// How many events name each record, counted once a state of the document.
int test_counts() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	MissionLinkCounts counts;
	counts.follow(m);
	TEST_EXPECT(counts.made() == 1);
	const NodeAddress walker = row_at(m, MissionKind::Organic, 0), guard = row_at(m, MissionKind::Organic, 1);
	// The walker: events 1 and 3 (event 3 names it in four parameters: once). The guard: event 3 (by its SSN;
	// event 2's group names the group, not its member).
	TEST_EXPECT(counts.events(walker.row) == 2 && counts.events(guard.row) == 1);
	TEST_EXPECT(counts.events(row_at(m, MissionKind::Area, 0).row) == 1 && counts.events(row_at(m, MissionKind::Area, 1).row) == 1);
	// The second zone 20 and the item carrying the walker's SSN: no event's (the lookups find the first).
	TEST_EXPECT(counts.events(row_at(m, MissionKind::Area, 2).row) == 0 && counts.events(row_at(m, MissionKind::Item, 3).row) == 0);
	// Event 2 named by event 1's re-arm, event 1 by event 2's trigger; the path, its stop, both numbered markers.
	TEST_EXPECT(counts.events(row_at(m, MissionKind::Event, 1).row) == 1 && counts.events(row_at(m, MissionKind::Event, 0).row) == 1);
	TEST_EXPECT(counts.events(row_at(m, MissionKind::WaypointPath, 1).row) == 1 && counts.events(row_at(m, MissionKind::Marker, 2).row) == 1);
	TEST_EXPECT(counts.events(row_at(m, MissionKind::Marker, 5).row) == 1 && counts.events(row_at(m, MissionKind::Marker, 6).row) == 1);
	TEST_EXPECT(counts.events(row_at(m, MissionKind::Marker, 0).row) == 0);
	// The events a badge counts are the ones its hover lists (S24): each once, in their order.
	TEST_EXPECT(counts.namers(walker.row).size() == counts.events(walker.row) &&
	            counts.namers(row_at(m, MissionKind::Marker, 6).row) == std::vector<NodeId>{row_at(m, MissionKind::Event, 2).row} &&
	            counts.namers(row_at(m, MissionKind::Marker, 0).row).empty());
	TEST_EXPECT(counts.group_events(1) == 1 && counts.group_events(2) == 1 && counts.group_events(3) == 0 && counts.group_events(0) == 0);
	// The same state counts nothing again; an edit counts anew.
	counts.follow(m);
	TEST_EXPECT(counts.made() == 1);
	Edit edit;
	edit.address = child_at(m, row_at(m, MissionKind::Event, 0), MissionKind::Trigger, 0);
	edit.field = "param1";
	edit.value = int64_t(f.guard);
	Diagnostic error;
	TEST_EXPECT(f.document->apply({edit}, error));
	counts.follow(m);
	TEST_EXPECT(counts.made() == 2 && counts.events(walker.row) == 1 && counts.events(guard.row) == 2);
	return 0;
}

// The groups' members: by group, by the records that carry or are a group, each once in the rows' order.
int test_groups() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const NodeAddress walker = row_at(m, MissionKind::Organic, 0), guard = row_at(m, MissionKind::Organic, 1);
	TEST_EXPECT(mission_group_rows(m, 1) == std::vector<NodeId>{walker.row} && mission_group_rows(m, 2) == std::vector<NodeId>{guard.row});
	TEST_EXPECT(mission_group_rows(m, 0).empty() && mission_group_rows(m, 9).empty());
	TEST_EXPECT(mission_group_of(m, walker) == 1 && mission_group_of(m, row_at(m, MissionKind::Area, 0)) == -1);
	// The group record event 2 kills (group 2) is the guard's group.
	const MissionLinks two = mission_links(m, row_at(m, MissionKind::Event, 1));
	const NodeAddress group2 = two.links[1].record;
	TEST_EXPECT(mission_group_of(m, group2) == 2);
	TEST_EXPECT(mission_group_members(m, {group2}) == std::vector<NodeAddress>{guard});
	TEST_EXPECT((mission_group_members(m, {guard, walker, group2}) == std::vector<NodeAddress>{walker, guard}));
	// An item in no group carries none.
	TEST_EXPECT(mission_group_members(m, {row_at(m, MissionKind::Item, 0)}).empty());
	return 0;
}

// S24 pick mode's model: what a slot takes from the views, and what a mark's record gives it.
int test_pick_values() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const NodeAddress first = row_at(m, MissionKind::Event, 0), second = row_at(m, MissionKind::Event, 1),
	                  third = row_at(m, MissionKind::Event, 2);
	const NodeAddress walker = row_at(m, MissionKind::Organic, 0), zone20 = row_at(m, MissionKind::Area, 0);
	MissionPickSlot slot;
	int64_t value = 0;
	// Event 1's trigger: param1 an entity, param2 a zone.
	TEST_EXPECT(mission_pick_slot(m, child_at(m, first, MissionKind::Trigger, 0), 0, slot) &&
	            slot.kind == mission::ParamKind::Entity && slot.marks && slot.what == "an entity" && slot.label == "Entity");
	TEST_EXPECT(mission_pick_value(m, slot, walker, value) && value == f.walker && !mission_pick_value(m, slot, zone20, value));
	TEST_EXPECT(mission_pick_slot(m, child_at(m, first, MissionKind::Trigger, 0), 1, slot) && slot.kind == mission::ParamKind::Zone &&
	            slot.what == "an area" && mission_pick_value(m, slot, zone20, value) && value == 20 &&
	            !mission_pick_value(m, slot, walker, value));
	// Event 1's action re-arms an event: no mark gives one.
	TEST_EXPECT(mission_pick_slot(m, child_at(m, first, MissionKind::Action, 0), 0, slot) && slot.kind == mission::ParamKind::Event &&
	            !slot.marks && !mission_pick_value(m, slot, walker, value));
	// Event 2's KillGroup: an entity in a group gives its group; one in none, nothing.
	TEST_EXPECT(mission_pick_slot(m, child_at(m, second, MissionKind::Action, 0), 0, slot) && slot.kind == mission::ParamKind::Group &&
	            mission_pick_value(m, slot, walker, value) && value == 1 &&
	            !mission_pick_value(m, slot, row_at(m, MissionKind::Item, 0), value));
	// A record whose value names another (S24): the item carrying the walker's SSN, the second area of zone 20, each
	// refused with the record the game finds first.
	std::string refusal;
	TEST_EXPECT(mission_pick_slot(m, child_at(m, first, MissionKind::Trigger, 0), 0, slot) &&
	            !mission_pick_value(m, slot, row_at(m, MissionKind::Item, 3), value, &refusal) &&
	            refusal == "SSN " + std::to_string(f.walker) + " names " + m.record_title(walker) + ", which the game finds first.");
	refusal.clear();
	TEST_EXPECT(mission_pick_slot(m, child_at(m, first, MissionKind::Trigger, 0), 1, slot) &&
	            !mission_pick_value(m, slot, row_at(m, MissionKind::Area, 2), value, &refusal) &&
	            refusal == "Zone id 20 names " + m.record_title(zone20) + ", which the game finds first.");
	// Event 3's redirect: the path a stop's marker is on, with the stop's place for its stop slot (param3), and the
	// stop's place on the record's path (path 1).
	const NodeAddress redirect = child_at(m, third, MissionKind::Action, 0);
	TEST_EXPECT(mission_pick_slot(m, redirect, 1, slot) && slot.kind == mission::ParamKind::Path && slot.node_slot == 2 &&
	            mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 3), value) && value == 1 &&
	            !mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 4), value));
	int64_t place = -1;
	TEST_EXPECT(mission_pick_node(m, slot, row_at(m, MissionKind::Marker, 3), place) && place == 3 &&
	            !mission_pick_node(m, slot, row_at(m, MissionKind::Marker, 4), place));
	// The pick's frame over two marks: the path picked at marker 3's stop sets its stop slot to that stop in the same
	// batch (one request, two Sets), then the pick ended; marker 4 (on no path) fills nothing.
	{
		struct Collect : CanvasRequests {
			std::vector<EditorRequest> got;
			void request(EditorRequest request) override { got.push_back(std::move(request)); }
		} collect;
		const MissionPickFrame pick = mission_pick_frame(m, redirect, 1, {row_at(m, MissionKind::Marker, 3), row_at(m, MissionKind::Marker, 4)});
		TEST_EXPECT(pick.fills_mark(0) && !pick.fills_mark(1) && pick.nodes.size() == 2 && pick.nodes[0] == 3);
		mission_pick_requests("missions/logic.bms", pick, 0, collect);
		TEST_EXPECT(collect.got.size() == 2 && collect.got[0].edits.size() == 2 && collect.got[0].edits[0].field == "param2" &&
		            collect.got[0].edits[1].field == "param3" && std::get<int64_t>(collect.got[0].edits[1].value) == 3 &&
		            collect.got[1].kind == EditorRequestKind::SetWorkspace);
		// Over a mark that names another record, the hint says why.
		const MissionPickFrame teleport =
		        mission_pick_frame(m, child_at(m, third, MissionKind::Action, 1), 1, {row_at(m, MissionKind::Marker, 6)});
		TEST_EXPECT(!teleport.fills_mark(0) &&
		            mission_pick_hint(teleport, 0).find("which the game finds first. Click another (Esc cancels).") != std::string::npos &&
		            mission_pick_hint(teleport, -1) == "Click a numbered marker (record type 6088) for Teleport target (Esc cancels).");
	}
	TEST_EXPECT(mission_pick_slot(m, redirect, 2, slot) && slot.kind == mission::ParamKind::PathNode && slot.path == 1 &&
	            slot.node_slot == -1 && slot.what == "a stop of path 1" &&
	            mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 2), value) && value == 2);
	// The teleport's target: the first numbered marker of its number (the teleport finds it); the second refused,
	// the effect's taking either (it plays at each); a waypoint marker gives none.
	refusal.clear();
	TEST_EXPECT(mission_pick_slot(m, child_at(m, third, MissionKind::Action, 1), 1, slot) && !slot.every_marker &&
	            slot.kind == mission::ParamKind::TeleportTarget && mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 5), value) &&
	            value == 7 && !mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 6), value, &refusal) &&
	            !mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 0), value));
	TEST_EXPECT(refusal == "Marker number 7 names " + m.record_title(row_at(m, MissionKind::Marker, 5)) +
	                               ", which the game finds first.");
	TEST_EXPECT(mission_pick_slot(m, child_at(m, third, MissionKind::Action, 2), 0, slot) && slot.every_marker &&
	            mission_pick_value(m, slot, row_at(m, MissionKind::Marker, 6), value) && value == 7);
	// Refused: a record that is no trigger or action, a slot past 3.
	TEST_EXPECT(!mission_pick_slot(m, walker, 0, slot) && !mission_pick_slot(m, redirect, 4, slot));
	TEST_EXPECT(std::string(mission_param_field(2)) == "param3" && std::string(mission_param_field(4)).empty());
	// B3: what a dropped record gives a reference field: an entity its SSN, an area its zone, a path its number,
	// an event its index, a group record its index; another kind nothing.
	TEST_EXPECT(mission_record_value(m, walker, ReferenceKind::MissionEntity, value) && value == f.walker);
	refusal.clear();
	TEST_EXPECT(!mission_record_value(m, row_at(m, MissionKind::Item, 3), ReferenceKind::MissionEntity, value, &refusal) &&
	            refusal.find("which the game finds first") != std::string::npos);
	refusal.clear();
	TEST_EXPECT(!mission_record_value(m, row_at(m, MissionKind::Area, 2), ReferenceKind::MissionZone, value, &refusal) &&
	            refusal.find("Zone id 20 names") == 0);
	TEST_EXPECT(!mission_record_value(m, walker, ReferenceKind::MissionZone, value) &&
	            !mission_record_value(m, walker, ReferenceKind::MissionGroup, value));
	TEST_EXPECT(mission_record_value(m, zone20, ReferenceKind::MissionZone, value) && value == 20);
	TEST_EXPECT(mission_record_value(m, row_at(m, MissionKind::WaypointPath, 1), ReferenceKind::MissionPath, value) && value == 1);
	TEST_EXPECT(mission_record_value(m, third, ReferenceKind::MissionEvent, value) && value == 2);
	// B4: what a mark offers a new event or the selected one: the walker and its group 1; an area its zone alone.
	const std::vector<MissionSubject> subjects = mission_subjects(m, walker);
	TEST_EXPECT(subjects.size() == 2 && subjects[0].kind == mission::ParamKind::Entity && subjects[0].value == f.walker &&
	            subjects[1].kind == mission::ParamKind::Group && subjects[1].value == 1 &&
	            subjects[1].record == mission_group_address(m, 1));
	TEST_EXPECT(mission_subjects(m, zone20).size() == 1 && mission_subjects(m, row_at(m, MissionKind::Item, 0)).size() == 1);
	TEST_EXPECT(mission_subjects(m, first).size() == 1 && mission_subjects(m, first)[0].kind == mission::ParamKind::Event);
	// The types taking a zone: Single and Group in an area among the triggers, each with its zone's slot (1).
	bool single_in_area = false;
	for (const MissionTypeSlot &use : mission_types_taking(mission::ParamKind::Zone, false))
		single_in_area = single_in_area || (use.type->type == 2 && use.type->sub == 10 && use.slot == 1);
	TEST_EXPECT(single_in_area && !mission_types_taking(mission::ParamKind::Entity, true).empty());
	// B7: what a revealed record frames: an entity itself, a path its stops' markers, a group its members, an event
	// (or its trigger) every row it names.
	TEST_EXPECT(mission_reveal_rows(m, walker) == std::vector<NodeId>{walker.row});
	TEST_EXPECT(mission_reveal_rows(m, row_at(m, MissionKind::WaypointPath, 1)).size() == 4);
	TEST_EXPECT((mission_reveal_rows(m, first) == std::vector<NodeId>{walker.row, zone20.row}));
	TEST_EXPECT(mission_reveal_rows(m, child_at(m, first, MissionKind::Action, 0)).empty());
	TEST_EXPECT(mission_reveal_rows(m, mission_group_address(m, 1)) == std::vector<NodeId>{walker.row});
	const NodeAddress group2 = mission_links(m, second).links[1].record;
	TEST_EXPECT(mission_record_value(m, group2, ReferenceKind::MissionGroup, value) && value == 2 &&
	            !mission_record_value(m, group2, ReferenceKind::MissionEntity, value));
	// New group: the lowest group no entity carries (1 and 2 carried), no event names and no name is given; one an
	// event names is skipped, as a named one.
	TEST_EXPECT(mission_fresh_group(m) == 3);
	Edit names_three;
	names_three.address = child_at(m, second, MissionKind::Action, 0);
	names_three.field = "param1";
	names_three.value = int64_t(3);
	Diagnostic error;
	TEST_EXPECT(f.document->apply({names_three}, error) && mission_fresh_group(m) == 4);
	{
		const std::vector<uint8_t> bytes =
		        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
		bms::File file;
		std::string why;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, why) && file.group_records.size() > 3 &&
		            bms::set_group_name(file.group_records[3], "Reserve", why) && bms::write(file, out, why));
		const std::unique_ptr<Document> named = open(out);
		TEST_EXPECT(named && mission_fresh_group(as_mission(*named)) == 4);
	}
	// The player's starts: none in this mission (no marker of the start types).
	TEST_EXPECT(mission_player_starts(m).empty());
	return 0;
}

} // namespace

int main() {
	if (test_pick() != 0) return 1;
	if (test_links() != 0) return 1;
	if (test_groups() != 0) return 1;
	if (test_pick_values() != 0) return 1;
	if (test_counts() != 0) return 1;
	std::printf("editor_mission_links: all tests passed\n");
	return 0;
}
