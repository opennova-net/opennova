// Adding and editing a mission's logic without its format (editor/documents/mission_logic.h,
// mission_uses.h, ADR 0046 S15, Events and scripts), over the minted synth_logic mission: a trigger's
// and an action's form (its type by name, the parameters its type reads with their plain labels and
// their values in words, the words and the event's sentence), an event's form (its parts, flags,
// steps and the limits); an add by type with its kinds' defaults, one batch the sentence then reads;
// the 20 an event holds refused before the edit, in words; a retype keeping what still applies; a
// move within an event and to another (one undo step); a negation and a join; what names a record
// (an entity, an area, an event, a group, a path; a second holder of an SSN said inert); and the
// `mission_logic` and `mission_uses` queries over a session. The defaults a new record's parameters take, the
// Common types and the recipes' measures, each the value the install's missions were measured to hold once; and
// every trigger and action of the sweep (mission_sweep.h) given a form and retyped to its own type planning
// nothing, what every record holds where its type reads nothing (-1 in a parameter, 0 in a sub-type), every
// recipe's composition matched.
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_recipes.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_uses.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_params.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/mission_sweep.h"
#include "editor/mission_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;
using opennova::io::JsonValue;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

using mission_test::same;
using mission_test::fixture_bytes;
using mission_test::open;

const MissionDocument &as_mission(const Document &document) { return dynamic_cast<const MissionDocument &>(document); }

// The minted mission's records: its two events, event 1's trigger and action, the walker.
struct Fixture {
	std::unique_ptr<Document> document;
	NodeAddress first, second, trigger, action, walker, zone20, zone30;
	int32_t ssn = 0;
	bool load() {
		document = open(fixture_bytes());
		if (!document) return false;
		const MissionDocument &m = as_mission(*document);
		refresh();
		const std::vector<const Node *> organics = m.rows_of(MissionKind::Organic), areas = m.rows_of(MissionKind::Area);
		walker = {organics[0]->id, organics[0]->kind, 0};
		ssn = static_cast<const EntityRow &>(*organics[0]).native.id;
		zone20 = {areas[0]->id, areas[0]->kind, 0};
		zone30 = {areas[1]->id, areas[1]->kind, 0};
		return true;
	}
	void refresh() {
		const MissionDocument &m = as_mission(*document);
		const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
		first = {events[0]->id, events[0]->kind, 0};
		second = {events[1]->id, events[1]->kind, 0};
		const std::vector<Document::Collection> lists = m.collections_of(first);
		trigger = lists[0].ids.empty() ? NodeAddress() : NodeAddress{first.row, k(MissionKind::Trigger), lists[0].ids[0]};
		action = lists[1].ids.empty() ? NodeAddress() : NodeAddress{first.row, k(MissionKind::Action), lists[1].ids[0]};
	}
	bool apply(const std::vector<Edit> &edits) {
		Diagnostic error;
		const bool ok = document->apply(edits, error);
		if (!ok) std::printf("  refused: %s\n", error.message.c_str());
		refresh();
		return ok;
	}
	size_t count(const NodeAddress &event, MissionKind kind) const {
		for (const Document::Collection &list : document->collections_of(event))
			if (list.spec.kind == k(kind)) return list.ids.size();
		return 0;
	}
};

int test_forms() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const DocumentMissionNames names(m);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	LogicForm form;
	TEST_EXPECT(logic_form(m, f.trigger, names, form));
	TEST_EXPECT(!form.action && form.type && same(form.type_words, "Entity is in an area") && same(form.type->group, "Areas and waypoints"));
	TEST_EXPECT(!form.negated && form.join == LogicJoin::And && form.last && form.index == 0 && form.count == 1);
	// The parameters the type reads, labelled and named; none it does not read holds a value.
	TEST_EXPECT(form.params.size() == 2 && form.unread.empty());
	TEST_EXPECT(form.params[0].field == "param1" && form.params[0].kind == mission::ParamKind::Entity &&
	            same(form.params[0].label, "Entity") && same(form.params[0].words, walker) && form.params[0].picks);
	TEST_EXPECT(form.params[1].kind == mission::ParamKind::Zone && same(form.params[1].label, "Zone") &&
	            same(form.params[1].words, "Zone 20"));
	TEST_EXPECT(same(form.words, walker + " is in Zone 20"));
	TEST_EXPECT(same(form.sentence, "When " + walker + " is in Zone 20, then re-arm event 2."));
	TEST_EXPECT(logic_form(m, f.action, names, form));
	TEST_EXPECT(form.action && same(form.type_words, "Re-arm event") && form.params.size() == 1 &&
	            same(form.params[0].words, "event 2"));
	// A type the dispatcher knows whose sub-type selects nothing is no unknown type (S15).
	Fixture g;
	TEST_EXPECT(g.load());
	Edit no_change;
	no_change.address = g.action;
	no_change.field = "action_type";
	no_change.value = int64_t(bms::ActionType::ChangeGroupAI);
	Edit sub_zero = no_change;
	sub_zero.field = "action_sub_type";
	sub_zero.value = int64_t(0);
	TEST_EXPECT(g.apply({no_change, sub_zero}));
	LogicForm unchanged;
	const DocumentMissionNames g_names(as_mission(*g.document));
	TEST_EXPECT(logic_form(as_mission(*g.document), g.action, g_names, unchanged) && !unchanged.type &&
	            same(unchanged.type_words, "Change group AI: no change (sub-type 0 does nothing)"));
	// Neither an event nor an entity has a record form.
	TEST_EXPECT(!logic_form(m, f.first, names, form) && !logic_form(m, f.walker, names, form));
	// The event's form: its parts, its flags, its steps and what it holds of the most.
	LogicEventForm event;
	TEST_EXPECT(logic_event_form(m, f.second.row, names, event));
	TEST_EXPECT(event.index == 1 && same(event.words.when, "When event 1 has fired") && same(event.words.delay, "after 5.1 s"));
	TEST_EXPECT(!event.repeats && !event.at_start && !event.at_end && event.delay == 5 && event.repeat == 0);
	TEST_EXPECT(event.most_steps == 1023 && event.triggers == 1 && event.actions == 1 && event.most == 20);
	TEST_EXPECT(event.trigger_refusal.empty() && event.action_refusal.empty());
	return 0;
}

int test_add_and_limit() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	// "Entity is destroyed" added to event 2: an Add with its type, its sub-type and the entity it reads
	// at its kind's default (the record born holding -1 in each parameter, the three it does not read
	// kept so, as every shipped record holds them); one batch, one step.
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	TEST_EXPECT(destroyed);
	std::vector<Edit> edits;
	std::string refusal;
	TEST_EXPECT(logic_add_edits(m, f.second.row, *destroyed, SIZE_MAX, edits, refusal));
	TEST_EXPECT(edits.size() == 3 && edits[0].operation == EditOperation::Add && edits[0].field == "main_type" &&
	            edits[1].address.child == batch_made(0) && edits[1].field == "sub_type" && edits[2].field == "param1" &&
	            edits[2].value == Value(int64_t(0)));
	TEST_EXPECT(f.apply(edits));
	{
		const EventRow &event = static_cast<const EventRow &>(*m.row(f.second.row));
		const bms::Trigger &made = event.native.triggers.back();
		TEST_EXPECT(made.param1 == 0 && made.param2 == mission::kUnreadParam && made.param3 == mission::kUnreadParam &&
		            made.param4 == mission::kUnreadParam);
		LogicForm form;
		TEST_EXPECT(logic_form(m, {f.second.row, k(MissionKind::Trigger), event.ids.lists[0].back().id},
		                       DocumentMissionNames(m), form) &&
		            form.unread.empty());
	}
	TEST_EXPECT(f.count(f.second, MissionKind::Trigger) == 2);
	TEST_EXPECT(same(m.record_title(f.second),
	                 "Event 2: When event 1 has fired and SSN 0 (no entity has it) is destroyed, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(f.document->can_undo());
	// A sub-goal's action takes its kinds' defaults: the first sub-goal.
	const LogicType *won = logic_type(true, int32_t(bms::ActionType::SubGoalWon), 0);
	TEST_EXPECT(won && logic_add_edits(m, f.second.row, *won, 0, edits, refusal));
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.second),
	                 "Event 2: When event 1 has fired and SSN 0 (no entity has it) is destroyed, then, after 5.1 s, win sub-goal 1; kill "
	                 "group 2."));
	// Twenty triggers: the event's form and the add say so before the edit.
	const LogicType *alive = logic_type(false, int32_t(bms::TriggerMainType::Single), 5);
	for (size_t i = f.count(f.first, MissionKind::Trigger); i < 20; ++i) {
		TEST_EXPECT(logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal));
		TEST_EXPECT(f.apply(edits));
	}
	TEST_EXPECT(f.count(f.first, MissionKind::Trigger) == 20);
	const std::string full = logic_add_refusal(m, f.first.row, false);
	TEST_EXPECT(full.find("holds 20 triggers, the most an event holds") != std::string::npos);
	TEST_EXPECT(logic_add_refusal(m, f.first.row, true).empty());
	TEST_EXPECT(!logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal) && same(refusal, full));
	LogicEventForm event;
	TEST_EXPECT(logic_event_form(m, f.first.row, DocumentMissionNames(m), event) && same(event.trigger_refusal, full));
	return 0;
}

int test_retype_move_negate() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	std::vector<Edit> edits;
	std::string refusal;
	// Entity is in an area -> Entity is destroyed: the entity kept, the zone (unread now) -1, as every
	// shipped record holds a parameter its type does not read.
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *destroyed, edits, refusal));
	TEST_EXPECT(edits.size() == 2 && edits[0].field == "sub_type" && edits[1].field == "param2" &&
	            edits[1].value == Value(int64_t(mission::kUnreadParam)));
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is destroyed"));
	// To its own type: nothing.
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *destroyed, edits, refusal) && edits.empty());
	// -> Group is wiped out: the entity is no group, so the first parameter takes the group's default.
	const LogicType *wiped = logic_type(false, int32_t(bms::TriggerMainType::Group), 4);
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *wiped, edits, refusal));
	TEST_EXPECT(f.apply(edits) && same(m.record_title(f.trigger), "no group has no unit left alive"));
	TEST_EXPECT(f.document->can_undo());
	f.document->undo();
	f.document->undo();
	f.refresh();
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is in Zone 20"));
	// Negated, then joined by or to a second trigger.
	Edit edit;
	TEST_EXPECT(logic_negate_edit(m, f.trigger, true, edit) && f.apply({edit}));
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is not in Zone 20"));
	const LogicType *alive = logic_type(false, int32_t(bms::TriggerMainType::Single), 5);
	TEST_EXPECT(logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal) && f.apply(edits));
	TEST_EXPECT(logic_join_edit(m, f.trigger, LogicJoin::Or, edit) && f.apply({edit}));
	TEST_EXPECT(same(m.record_title(f.first), "Event 1: When " + walker + " is not in Zone 20 or SSN 0 (no entity has it) is alive, then re-arm event 2."));
	// The action moved to event 2 (added there with its fields, removed here: one step), then back.
	const std::string before = f.document->serialize().text;
	TEST_EXPECT(logic_move_edits(m, f.action, f.second.row, SIZE_MAX, edits, refusal));
	TEST_EXPECT(edits.back().operation == EditOperation::Remove);
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(f.count(f.first, MissionKind::Action) == 0 && f.count(f.second, MissionKind::Action) == 2);
	TEST_EXPECT(same(m.record_title(f.second), "Event 2: When event 1 has fired, then, after 5.1 s, kill group 2; re-arm event 2."));
	f.document->undo();
	f.refresh();
	TEST_EXPECT(f.document->serialize().text == before);
	// Within its event: a Move.
	TEST_EXPECT(logic_add_edits(m, f.first.row, *logic_type(true, int32_t(bms::ActionType::BlueWin), 0), SIZE_MAX, edits, refusal) &&
	            f.apply(edits));
	TEST_EXPECT(logic_move_edits(m, f.action, f.first.row, 1, edits, refusal) && edits.size() == 1 &&
	            edits[0].operation == EditOperation::Move && f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.first).substr(m.record_title(f.first).find(", then")),
	                 ", then end the round: the blue team (team 1) wins; re-arm event 2."));
	return 0;
}

// What names a record: the walker by the event's trigger, zone 20 likewise, event 2 by the action,
// group 2 by event 2's kill; the second holder of an SSN named by none.
int test_uses() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const DocumentMissionNames names(m);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	MissionUses uses = mission_uses(m, f.walker, names);
	TEST_EXPECT(same(uses.what, walker) && uses.inert.empty() && uses.uses.size() == 1);
	TEST_EXPECT(uses.uses[0].record == f.trigger && uses.uses[0].field == "param1" && uses.uses[0].event_index == 0 &&
	            same(uses.uses[0].words, walker + " is in Zone 20") && uses.uses[0].sentence.find("When ") == 0);
	uses = mission_uses(m, f.zone20, names);
	TEST_EXPECT(same(uses.what, "Zone 20") && uses.uses.size() == 1 && uses.uses[0].field == "param2");
	TEST_EXPECT(mission_uses(m, f.zone30, names).uses.empty());
	uses = mission_uses(m, f.second, names);
	TEST_EXPECT(same(uses.what, "event 2") && uses.uses.size() == 1 && uses.uses[0].record == f.action);
	uses = mission_uses(m, f.first, names);
	TEST_EXPECT(uses.uses.size() == 1 && uses.uses[0].event_index == 1); // event 2's trigger
	// Group 2, a record of the mission row's Groups.
	const MissionRow *mission_row = m.mission_row();
	NodeAddress group;
	for (const Document::Collection &list : m.collections_of({mission_row->id, mission_row->kind, 0}))
		if (list.spec.kind == k(MissionKind::Group)) group = {mission_row->id, list.spec.kind, list.ids[2]};
	uses = mission_uses(m, group, names);
	TEST_EXPECT(same(uses.what, "group 2") && uses.uses.size() == 1 && same(uses.uses[0].words, "kill group 2"));
	// A building is no event's business by itself only through its SSN: none names it.
	const Node *building = m.rows_of(MissionKind::Building)[0];
	TEST_EXPECT(mission_uses(m, {building->id, building->kind, 0}, names).uses.empty());
	// A second holder of the walker's SSN: inert, said why.
	Edit edit;
	edit.address = {building->id, building->kind, 0};
	edit.field = "id";
	edit.value = int64_t(f.ssn);
	TEST_EXPECT(f.apply({edit}));
	uses = mission_uses(m, {building->id, building->kind, 0}, DocumentMissionNames(m));
	TEST_EXPECT(!uses.inert.empty() && uses.inert.find("find the first in pool order") != std::string::npos);
	return 0;
}

// S24's helpers: a wait in seconds as the steps the game counts, the Common types, a new event of a recipe (one
// batch: the event at the end, its flags and its trigger with its presets and the subject's value), a recipe
// over a record of another kind refused, an add with presets, and what a record is to a parameter.
int test_recipes_and_steps() {
	// Steps of 1.024 s, rounded; ten bits at most; past 512 the countdown wraps (the words say so).
	TEST_EXPECT(logic_seconds_steps(5.0) == 5 && logic_seconds_steps(5.1) == 5 && logic_seconds_steps(1.5) == 1);
	TEST_EXPECT(logic_seconds_steps(0.0) == 0 && logic_seconds_steps(-3.0) == 0 && logic_seconds_steps(0.6) == 1);
	TEST_EXPECT(logic_seconds_steps(512 * 1.024) == 512 && logic_seconds_steps(5000.0) == 1023);
	TEST_EXPECT(logic_steps_words(logic_seconds_steps(600.0)).find("wraps") != std::string::npos);
	TEST_EXPECT(logic_common_types(false).size() == 8 && logic_common_types(true).size() == 8);
	TEST_EXPECT(same(logic_common_types(false)[0]->title, "Entity is destroyed") &&
	            same(logic_common_types(true)[0]->title, "Send group along a path"));
	TEST_EXPECT(mission_recipe("player_in_area") && !mission_recipe("player_enters"));
	TEST_EXPECT(mission_recipes_for(mission::ParamKind::Zone).size() == 2);

	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	mission::ParamKind kind = mission::ParamKind::Unused;
	int64_t value = 0;
	TEST_EXPECT(mission_record_value(m, f.zone20, kind, value) && kind == mission::ParamKind::Zone && value == 20);
	TEST_EXPECT(mission_record_value(m, f.walker, kind, value) && kind == mission::ParamKind::Entity && value == f.ssn);
	TEST_EXPECT(mission_record_value(m, f.second, kind, value) && kind == mission::ParamKind::Event && value == 1);
	TEST_EXPECT(!mission_record_value(m, f.trigger, kind, value));

	// The player inside zone 30: one batch, the event last, its sentence reads it; the others keep their places.
	const std::string before_first = m.record_title(f.first), before_second = m.record_title(f.second);
	std::vector<Edit> edits;
	std::string error;
	TEST_EXPECT(recipe_event_edits(m, *mission_recipe("player_in_area"), f.zone30, edits, error));
	TEST_EXPECT(edits.size() >= 4 && edits[0].operation == EditOperation::Add && edits[1].operation == EditOperation::Add &&
	            edits[1].address.row == batch_made(0));
	TEST_EXPECT(f.apply(edits));
	std::vector<const Node *> events = m.rows_of(MissionKind::Event);
	TEST_EXPECT(events.size() == 3 && same(m.record_title(f.first), before_first) && same(m.record_title(f.second), before_second));
	const NodeAddress made{events[2]->id, events[2]->kind, 0};
	TEST_EXPECT(same(m.record_title(made), "Event 3: When the player is in Zone 30 (mission area), then nothing."));
	// At the mission's start: no trigger, the start flag.
	TEST_EXPECT(recipe_event_edits(m, *mission_recipe("at_start"), NodeAddress(), edits, error) && f.apply(edits));
	events = m.rows_of(MissionKind::Event);
	TEST_EXPECT(events.size() == 4 && same(m.record_title({events[3]->id, events[3]->kind, 0}), "Event 4: At the mission's start, then nothing."));
	// A recipe over an entity given an area: refused, in words.
	TEST_EXPECT(!recipe_event_edits(m, *mission_recipe("destroyed"), f.zone20, edits, error) && !error.empty());
	TEST_EXPECT(recipe_event_edits(m, *mission_recipe("destroyed"), f.walker, edits, error) && f.apply(edits));
	events = m.rows_of(MissionKind::Event);
	TEST_EXPECT(same(m.record_title({events[4]->id, events[4]->kind, 0}),
	                 "Event 5: When Organic #" + std::to_string(f.ssn) + " is destroyed, then nothing."));
	// An add with presets: the walker's SSN where the kind's default would be none.
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	TEST_EXPECT(destroyed && logic_add_edits(m, f.second.row, *destroyed, SIZE_MAX, edits, error, {{0, f.ssn}}) && f.apply(edits));
	TEST_EXPECT(m.record_title(f.second).find("and Organic #" + std::to_string(f.ssn) + " is destroyed") != std::string::npos);
	return 0;
}

// S24 A6: a drop's preview reads the event as the move leaves it (a trigger read with the join its new place
// gives it); a whole event moved (the board's Move event...) renumbers the Event parameters that name the events.
int test_move_preview_and_event_move() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const DocumentMissionNames names(m);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	// The walker's trigger moved to the second event, first: its sentence reads it and the join it then has.
	TEST_EXPECT(same(logic_move_sentence(m, f.trigger, f.second.row, 0, names),
	                 "When " + walker + " is in Zone 20 and event 1 has fired, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(same(logic_move_sentence(m, f.trigger, f.second.row, SIZE_MAX, names),
	                 "When event 1 has fired and " + walker + " is in Zone 20, then, after 5.1 s, kill group 2."));
	// Within its own event: the event as it reads (one trigger, nowhere else to go).
	TEST_EXPECT(same(logic_move_sentence(m, f.trigger, f.first.row, 0, names), "When " + walker + " is in Zone 20, then re-arm event 2."));
	// The event it leaves: its only trigger gone, it holds on its first check as the mission runs (said by the flag);
	// none for a move within one event.
	bool untriggered = false;
	TEST_EXPECT(same(logic_move_left_sentence(m, f.trigger, f.second.row, 0, names, &untriggered), "Right away, then re-arm event 2.") &&
	            untriggered);
	TEST_EXPECT(logic_move_left_sentence(m, f.trigger, f.first.row, 0, names, &untriggered).empty() && !untriggered);
	// The second event moved first: the first's re-arm names it still (now event 1), its trigger the other (now event 2).
	Edit move;
	move.operation = EditOperation::Move;
	move.address = f.second;
	move.position = 0;
	TEST_EXPECT(f.apply({move}));
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
	TEST_EXPECT(events.size() == 2 && events[0]->id == f.first.row);
	TEST_EXPECT(same(m.record_title({events[0]->id, events[0]->kind, 0}), "Event 1: When event 2 has fired, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(same(m.record_title({events[1]->id, events[1]->kind, 0}), "Event 2: When " + walker + " is in Zone 20, then re-arm event 1."));
	return 0;
}

// The two queries over a session: the event's form, the types, an add planned and applied through
// edit_record, a refusal answered, a record's uses.
int test_wire() {
	test_temp::TempDir dir("opennova_mission_logic_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Logic"));
	editor_test::create_missing_files(session);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const Document *document = session.document_for("missions/logic.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const MissionDocument &m = as_mission(*document);
	const Node *event = m.rows_of(MissionKind::Event)[1];
	const auto ask = [&](const char *name, const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query(name, parsed, error);
		if (!error.empty()) std::printf("  %s refused: %s\n", name, error.c_str());
		return answer;
	};
	const std::string id = std::to_string(event->id);
	JsonValue form = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"form\", \"id\": " + id + "}");
	TEST_EXPECT(same(form.get_string("sentence", ""), "When event 1 has fired, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(form.get_number("delay_steps", -1) == 5 && form.get_number("most", 0) == 20);
	JsonValue types = ask("mission_logic", "{\"op\": \"types\", \"list\": \"action\", \"limit\": 200}");
	TEST_EXPECT(types.get("types") && types.get_number("count", 0) == double(logic_types(true).size()));
	JsonValue planned = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"add\", \"id\": " + id +
	                                                 ", \"list\": \"action\", \"type\": 6}");
	const JsonValue *edits = planned.get("edits");
	// The Add, then the text id it reads at its kind's default over the -1 a new record holds.
	TEST_EXPECT(edits && edits->array.size() == 2 && !planned.get("refusal") &&
	            edits->array[1].get_string("field", "") == "param1" && edits->array[1].get_number("value", -1) == 0);
	// The planned edits passed back as they are.
	JsonValue request = JsonValue::make_object();
	request.set("kind", opennova::io::json_string("edit_record"));
	request.set("path", opennova::io::json_string("missions/logic.bms"));
	request.set("edits", *edits);
	const JsonValue answer = session.handle_json(request);
	session.run_operations();
	TEST_EXPECT(answer.get_bool("ok", false));
	TEST_EXPECT(same(m.record_title({event->id, event->kind, 0}), "Event 2: When event 1 has fired, then, after 5.1 s, kill group 2; show text 0."));
	const JsonValue unknown = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"add\", \"id\": " + id +
	                                                   ", \"list\": \"action\", \"type\": 29}");
	TEST_EXPECT(unknown.is_null());
	const Node *organic = m.rows_of(MissionKind::Organic)[0];
	JsonValue uses = ask("mission_uses", "{\"path\": \"missions/logic.bms\", \"id\": " + std::to_string(organic->id) + "}");
	TEST_EXPECT(uses.get_number("count", 0) == 1 && uses.get("uses") && uses.get("uses")->array[0].get_string("field", "") == "param1");
	return 0;
}

// What the shipped records hold, measured once over the install's 115 missions (3,858 triggers and 4,971 actions;
// this test's leg over them, 2026-10-19) and pinned here as the constants the editor's tables state.
//
// The kinds whose value is a quantity or a choice (a count, a distance, seconds, a speed, on or off, a team, a
// sub-goal): a new record's default (logic_param_default) is the value the shipped records of the kind hold most
// often: a count 1 (179 of 290 records), a distance 200 m (27 of 202), 10 s (8 of 17), 20 km/h (87 of 626), off
// (781 of 1,047), team 2 (16 of 23), sub-goal 1 (131 of 548). An index (a waypoint, a variable, a dialog, a
// marker number, a HUD item, a light channel, an input bit) and a record named (picked) start at 0.
int test_measured_defaults() {
	using K = mission::ParamKind;
	TEST_EXPECT(logic_param_default(K::Count) == 1 && logic_param_default(K::DistanceM) == 200 &&
	            logic_param_default(K::Seconds) == 10 && logic_param_default(K::SpeedKph) == 20 &&
	            logic_param_default(K::Bool) == 0 && logic_param_default(K::Team) == 2 && logic_param_default(K::SubGoal) == 1);
	// Hit points: no shipped record holds the kind, 0.
	for (const K kind : {K::Group, K::Entity, K::Zone, K::Event, K::Path, K::PathNode, K::MissionVar, K::Dialog, K::Bit,
	                     K::HudTimer, K::LightChannel, K::TeleportTarget, K::WpNumber, K::Hp})
		TEST_EXPECT(logic_param_default(kind) == 0);
	// Every parameter slot a shipped record's type does not read held -1 (19,596 of 19,596), every sub-type its
	// type does not read 0 (501 of 501): what a new record's hold (mission::kUnreadParam; the Add sets a sub-type
	// only where its type has one).
	TEST_EXPECT(mission::kUnreadParam == -1);
	return 0;
}

// The types the shipped missions use most (logic_common_types, the type picker's Common): the eight trigger and
// eight action types holding the most of the install's records, in order (the measure above: Entity is destroyed
// 1,076, Group is in an area 606, Event has fired 501, Group has passed a waypoint 484, Group has lost units 289,
// Entity is in an area 267, Group is wiped out 166, Variable equals 126; Send group along a path 659, Send entity
// along a path 491, Kill entity 343, Re-arm event 340, Group AI: Patrol speed 334, Remove entity 307, Show or hide a
// win objective 275, Play dialog 238). The recipes' measures (mission_recipes: the events of the 115 holding
// exactly a recipe's trigger, or none, with its flags, and the missions holding them, the first three named), as
// the table states them; and every recipe's composition matched by an event of the sweep (logic.bms holds one of
// each), no event matching two. (No shipped event shows a Triggered Text line and none of the 135 shipped string
// tables holds that section, which is why New text numbers from 1, set_string_text's create: a fact of the install
// alone, not pinned.)
int test_common_and_recipes() {
	const char *const triggers[] = {"Entity is destroyed",         "Group is in an area",  "Event has fired",
	                                "Group has passed a waypoint", "Group has lost units", "Entity is in an area",
	                                "Group is wiped out",          "Variable equals"};
	const char *const actions[] = {"Send group along a path",      "Send entity along a path", "Kill entity",
	                               "Re-arm event",                 "Group AI: Patrol speed",   "Remove entity",
	                               "Show or hide a win objective", "Play dialog"};
	for (const bool action : {false, true}) {
		const std::vector<const LogicType *> &common = logic_common_types(action);
		TEST_EXPECT(common.size() == 8);
		for (size_t i = 0; i < common.size(); ++i) TEST_EXPECT(same(common[i]->title, action ? actions[i] : triggers[i]));
	}
	struct Measure {
		const char *token;
		size_t events, missions;
		const char *seen_in;
	};
	const Measure measures[] = {
	        {"right_away", 707, 97, "00tra.bms, 00trb.bms, 00trc.bms"},
	        {"destroyed", 463, 36, "00trb.bms, 00trc.bms, 00trd.bms"},
	        {"group_at_stop", 364, 33, "00tra.bms, 00trb.bms, 00trc.bms"},
	        {"group_in_area", 242, 33, "00tra.bms, 00trb.bms, 00trd.bms"},
	        {"event_fired", 159, 28, "00tre.bms, 00tri.bms, 01tr.bms"},
	        {"group_lost", 139, 22, "00trc.bms, 00tre.bms, 00trf.bms"},
	        {"group_wiped_out", 57, 19, "00trb.bms, 00trg.bms, 00trh.bms"},
	        {"at_start", 35, 31, "00trb.bms, 00tre.bms, 00trf.bms"},
	        {"variable_equals", 26, 5, "00tri.bms, 03tr.bms, 05tr.bms"},
	        {"player_in_area", 12, 2, "01tr.bms, 04tr.bms"},
	};
	TEST_EXPECT(mission_recipes().size() == std::size(measures));
	for (size_t r = 0; r < std::size(measures); ++r) {
		const MissionRecipe &recipe = mission_recipes()[r];
		TEST_EXPECT(same(recipe.token, measures[r].token) && recipe.events == measures[r].events &&
		            recipe.missions == measures[r].missions && same(recipe.seen_in, measures[r].seen_in));
	}
	std::vector<std::string> missing; // test_sweep requires every one
	std::vector<size_t> events(mission_recipes().size());
	for (const mission_sweep::Mission &file : mission_sweep::load(missing)) {
		std::unique_ptr<Document> document = open(file.bytes, file.name.c_str());
		TEST_EXPECT(document);
		if (!document) continue;
		for (const Node *row : as_mission(*document).rows_of(MissionKind::Event)) {
			const EventRow &event = static_cast<const EventRow &>(*row);
			size_t matched = 0;
			for (size_t r = 0; r < mission_recipes().size(); ++r)
				if (recipe_matches(mission_recipes()[r], event.native.event, event.native.triggers)) {
					++events[r];
					++matched;
				}
			TEST_EXPECT(matched <= 1);
		}
	}
	for (size_t r = 0; r < events.size(); ++r) {
		std::printf("  recipe %s: %zu events of the sweep\n", mission_recipes()[r].token, events[r]);
		TEST_EXPECT(events[r] > 0);
	}
	return 0;
}

// Every trigger and action of every mission of the sweep (mission_sweep.h), in place of every one the install
// ships, given a form of its type and retyped to its own type planning nothing (logic.bms holds every type with
// each form of value the shipped records hold); what each record holds where its type reads nothing, -1 in a
// parameter and 0 in a sub-type, as every shipped record holds it; and what names each organic.
int test_sweep() {
	std::vector<std::string> missing;
	const std::vector<mission_sweep::Mission> sweep = mission_sweep::load(missing);
	size_t missions = 0, records = 0, uses = 0;
	std::map<int64_t, size_t> unused, unread_subs;
	for (const mission_sweep::Mission &file : sweep) {
		std::unique_ptr<Document> document = open(file.bytes, file.name.c_str());
		TEST_EXPECT(document && !document->blocked());
		if (!document) continue;
		++missions;
		const MissionDocument &m = as_mission(*document);
		const DocumentMissionNames names(m);
		for (const Node *row : m.rows_of(MissionKind::Event)) {
			for (const Document::Collection &list : m.collections_of({row->id, row->kind, 0}))
				for (const NodeId id : list.ids) {
					const NodeAddress record{row->id, list.spec.kind, id};
					LogicForm form;
					TEST_EXPECT(logic_form(m, record, names, form) && form.type);
					if (!form.type) continue;
					++records;
					std::vector<Edit> edits;
					std::string refusal;
					TEST_EXPECT(logic_retype_edits(m, record, *form.type, edits, refusal) && edits.empty());
				}
		}
		// What the records hold where their type reads nothing: each parameter slot no row of the type names, a
		// trigger's sub-type under a main type that reads none. The rule is the shipped records' (and what the editor
		// writes): synth_dense.bms (tests/fixtures/minimal_bms_gen.cpp) is minted with 0 in some unread slots, a
		// form no shipped mission holds, so its records are left out of the tally.
		const bool shipped_form = file.name != "synth_dense.bms";
		for (const Node *row : shipped_form ? m.rows_of(MissionKind::Event) : std::vector<const Node *>()) {
			const EventRow &event = static_cast<const EventRow &>(*row);
			for (const bms::Trigger &trigger : event.native.triggers) {
				const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
				for (int slot = 0; slot < 4; ++slot)
					if (mission::trigger_param_kind(trigger, slot) == mission::ParamKind::Unused) ++unused[params[slot]];
				if (!mission::trigger_reads_sub_type(int32_t(trigger.main_type))) ++unread_subs[trigger.sub_type];
			}
			for (const bms::Action &action : event.native.actions) {
				const int32_t params[4] = {action.param1, action.param2, action.param3, action.param4};
				for (int slot = 0; slot < 4; ++slot)
					if (mission::action_param_kind(action, slot) == mission::ParamKind::Unused) ++unused[params[slot]];
			}
		}
		for (const Node *row : m.rows_of(MissionKind::Organic)) uses += mission_uses(m, {row->id, row->kind, 0}, names).uses.size();
	}
	std::printf("sweep: %zu missions, %zu triggers and actions given a form and retyped to themselves, %zu uses of the "
	            "organics; unused parameters:",
	            missions, records, uses);
	for (const auto &[value, count] : unused) std::printf(" [%lld: %zu]", (long long)value, count);
	std::printf("; unread sub-types:");
	for (const auto &[value, count] : unread_subs) std::printf(" [%lld: %zu]", (long long)value, count);
	std::printf("\n");
	TEST_EXPECT(records > 0 && uses > 0);
	TEST_EXPECT(unused.size() == 1 && unused.begin()->first == mission::kUnreadParam);
	TEST_EXPECT(unread_subs.size() == 1 && unread_subs.begin()->first == 0);
	TEST_EXPECT(mission_sweep::none_missing(missing));
	return 0;
}

} // namespace

int main() {
	if (test_forms() != 0) return 1;
	if (test_add_and_limit() != 0) return 1;
	if (test_retype_move_negate() != 0) return 1;
	if (test_uses() != 0) return 1;
	if (test_recipes_and_steps() != 0) return 1;
	if (test_move_preview_and_event_move() != 0) return 1;
	if (test_wire() != 0) return 1;
	if (test_measured_defaults() != 0) return 1;
	if (test_common_and_recipes() != 0) return 1;
	return test_sweep();
}
