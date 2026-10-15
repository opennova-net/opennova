// The Event board's portable half (ADR 0046 S24, The board; editor/documents/mission_board.h), over the minted
// synth_logic mission: its events as cards (title, sentence, flags, wait and repeat), each trigger and action a
// line of runs with a chip per parameter its type reads; the notes, never gating (an event with no action, one
// with no trigger, a pick left unset or naming nothing, an area the game drops, a wait past 512 steps, a value
// its type does not read); the filter, Needs attention and folds; the running game's report on its card. The
// mission_logic query answers by its op's row (session/mission_queries.h): every op its catalog offers is one of
// the table's tokens, each answered by its own row; `board` the cards as the Logic pane shows them, its filter
// and folds the workspace's unless the args name them, with the Common types and the recipes. And (a SKIP-LEG
// without OPENNOVA_JO_DIR) every shipped mission's board built, every event a card, each line's runs its words,
// its notes counted by code, timed.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <chrono>
#include <map>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_board.h>
#include <editor/documents/mission_live.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_recipes.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_variables.h>
#include <editor/documents/strings_document.h>
#include <editor/session/editor_queries.h>
#include <editor/session/mission_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/blank/blank_factory.h>
#include <editor/session/request_factories.h>
#include <formats/def/reserved_items.h>
#include <formats/pff/pff.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/mission_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
namespace bms = opennova::bms;
namespace mission = opennova::mission;

namespace {

using mission_test::same;
using mission_test::open;
using mission_test::fixture_bytes;

const BoardNote *note_of(const BoardCard &card, const char *code) {
	for (const BoardNote &note : card.notes)
		if (std::string(note.code) == code) return &note;
	return nullptr;
}

using mission_test::apply_edits;

// The cards over the minted mission: two events, the first's trigger and action as lines of runs and chips.
int test_cards() {
	std::unique_ptr<Document> document = open(fixture_bytes(), "synth_logic.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const auto &m = dynamic_cast<const MissionDocument &>(*document);
	const DocumentMissionNames names(m);
	MissionBoard board = mission_board(m, names, BoardQuery());
	TEST_EXPECT(board.events == 2 && board.cards.size() == 2 && board.noted == 0);
	const BoardCard &first = board.cards[0];
	const Node *organic = m.rows_of(MissionKind::Organic)[0];
	const std::string walker = "Organic #" + std::to_string(static_cast<const EntityRow &>(*organic).native.id);
	TEST_EXPECT(same(first.title, "Event 1") && first.index == 0 && !first.folded && !first.live);
	TEST_EXPECT(same(first.words.sentence, "When " + walker + " is in Zone 20, then re-arm event 2."));
	TEST_EXPECT(first.triggers.size() == 1 && first.actions.size() == 1 && first.notes.empty());
	const BoardLine &trigger = first.triggers[0];
	TEST_EXPECT(!trigger.action && trigger.type && same(trigger.type_words, "Entity is in an area") && trigger.last);
	TEST_EXPECT(trigger.runs.size() == 3 && trigger.runs[0].slot == 0 && same(trigger.runs[0].text, walker) &&
	            trigger.runs[1].slot == kLogicTypeSlot && same(trigger.runs[1].text, " is in ") && trigger.runs[2].slot == 1 &&
	            same(trigger.runs[2].text, "Zone 20"));
	TEST_EXPECT(trigger.chips.size() == 2 && trigger.chips[0].kind == mission::ParamKind::Entity && trigger.chips[0].picks &&
	            !trigger.chips[0].unset && !trigger.chips[0].missing && same(trigger.chips[1].words, "Zone 20"));
	const BoardCard &second = board.cards[1];
	TEST_EXPECT(same(second.title, "Event 2") && second.delay_steps == 5 && same(second.words.delay, "after 5.1 s"));

	// An event of no trigger and no action whose firing nothing reads: it fires right away, and is noted for that and
	// for its no action; a trigger whose entity is unset; a wait past 512 steps; a value its type does not read.
	std::vector<Edit> edits;
	std::string error;
	TEST_EXPECT(recipe_event_edits(m, *mission_recipe("right_away"), NodeAddress(), edits, error) && apply_edits(*document, edits));
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	const NodeId second_event = m.rows_of(MissionKind::Event)[1]->id;
	TEST_EXPECT(destroyed && logic_add_edits(m, second_event, *destroyed, SIZE_MAX, edits, error) && apply_edits(*document, edits));
	Edit wait;
	wait.address = {second_event, node_kind(MissionKind::Event), 0};
	wait.field = "delay";
	wait.value = int64_t(600);
	TEST_EXPECT(apply_edits(*document, {wait}));
	board = mission_board(m, names, BoardQuery());
	TEST_EXPECT(board.events == 3 && board.cards.size() == 3 && board.noted == 2);
	const BoardCard &noted = board.cards[1];
	TEST_EXPECT(noted.triggers.size() == 2 && noted.triggers[1].chips.size() == 1 && noted.triggers[1].chips[0].unset);
	TEST_EXPECT(note_of(noted, kBoardUnset) && same(note_of(noted, kBoardUnset)->words, "Trigger 2: its entity is not set."));
	// The wrap said as the sentence says it: the wait ends about 1.0 s after its triggers hold.
	TEST_EXPECT(note_of(noted, kBoardWaitWraps) &&
	            same(note_of(noted, kBoardWaitWraps)->words,
	                 "Its wait of 600 steps is past 512, where the game's countdown wraps: it acts about 1.0 s after its triggers hold."));
	TEST_EXPECT(noted.words.delay.find("after about 1.0 s") == 0);
	const BoardCard &empty = board.cards[2];
	TEST_EXPECT(note_of(empty, kBoardNoActions) && note_of(empty, kBoardRightAway) &&
	            std::string(empty.notes[0].code) == kBoardRightAway);
	TEST_EXPECT(same(note_of(empty, kBoardRightAway)->words, "It has no trigger: it fires right away, as the mission runs."));
	TEST_EXPECT(same(note_of(empty, kBoardNoActions)->words,
	                 "It has no action, and nothing reads that it fired: no trigger names it, no waypoint marker advances on it, "
	                 "no line of the mission's script reads its event()."));
	const NodeId third_event = m.rows_of(MissionKind::Event)[2]->id;
	{
		// What reads its firing (its latch, set as it fires with no action [bms-event-runtime-re.md 1.2, D-EVT-9]): the
		// script's event(2), a trigger naming it, a waypoint marker advancing on it; each takes its no-action note away.
		BoardQuery read;
		read.script_events = {2};
		BoardCard card;
		TEST_EXPECT(mission_board_card(m, third_event, names, card, nullptr, read.script_events) && !note_of(card, kBoardNoActions));
		TEST_EXPECT(mission_board(m, names, read).noted == 2);
		const LogicType *fired = logic_type(false, int32_t(bms::TriggerMainType::Event), 0);
		const NodeId first_event = m.rows_of(MissionKind::Event)[0]->id;
		TEST_EXPECT(fired && logic_add_edits(m, first_event, *fired, SIZE_MAX, edits, error, {{0, 2}}) && apply_edits(*document, edits));
		TEST_EXPECT(mission_board_card(m, third_event, names, card) && !note_of(card, kBoardNoActions) && note_of(card, kBoardRightAway));
		TEST_EXPECT(document->can_undo());
		document->undo();
		TEST_EXPECT(mission_board_card(m, third_event, names, card) && note_of(card, kBoardNoActions));
		const Node *marker = nullptr;
		for (const Node *row : m.rows_of(MissionKind::Marker))
			if (static_cast<const EntityRow &>(*row).native.type_id == opennova::def::DEF_TYPE_WAYPOINT) marker = row;
		TEST_EXPECT(marker != nullptr);
		if (marker) {
			Edit advance;
			advance.address = {marker->id, marker->kind, 0};
			advance.field = "wp_adv_trigger";
			advance.value = int64_t(2);
			TEST_EXPECT(apply_edits(*document, {advance}));
			TEST_EXPECT(mission_board_card(m, third_event, names, card) && !note_of(card, kBoardNoActions));
			TEST_EXPECT(document->can_undo());
			document->undo();
		}
		// A wait: no longer "right away", but after it; a repeat, again each time it ends.
		Edit waits;
		waits.address = {third_event, node_kind(MissionKind::Event), 0};
		waits.field = "delay";
		waits.value = int64_t(5);
		TEST_EXPECT(apply_edits(*document, {waits}));
		TEST_EXPECT(mission_board_card(m, third_event, names, card) &&
		            same(note_of(card, kBoardRightAway)->words,
		                 "It has no trigger: it holds on its first check, as the mission runs, and acts after its wait (5.1 s)."));
		// A repeat past 512 steps: its note says when it is checked again as its sentence does (the pass after it ends).
		Edit repeats = waits;
		repeats.field = "flags";
		repeats.value = int64_t(bms::EventFlags::ResetAfter);
		Edit cooldown = waits;
		cooldown.field = "reset_after";
		cooldown.value = int64_t(600);
		TEST_EXPECT(apply_edits(*document, {repeats, cooldown}));
		TEST_EXPECT(mission_board_card(m, third_event, names, card));
		const BoardNote *wrapped = nullptr;
		for (const BoardNote &note : card.notes)
			if (note.code == std::string(kBoardWaitWraps)) wrapped = &note;
		TEST_EXPECT(wrapped && same(wrapped->words, "Its repeat of 600 steps is past 512, where the game's countdown wraps: it is "
		                                            "checked again about 2.0 s after its triggers held."));
		TEST_EXPECT(card.words.repeat.find("Checked again about 2.0 s after its triggers held") == 0);
		TEST_EXPECT(same(note_of(card, kBoardRightAway)->words,
		                 "It has no trigger: it holds on its first check, as the mission runs, and acts after its wait (5.1 s), and "
		                 "again each time its repeat ends."));
		TEST_EXPECT(document->can_undo());
		document->undo();
		document->undo();
	}

	// Needs attention, the filter, the folds.
	BoardQuery query;
	query.attention = true;
	TEST_EXPECT(mission_board(m, names, query).cards.size() == 2);
	query = BoardQuery();
	query.filter = "RE-ARM";
	board = mission_board(m, names, query);
	TEST_EXPECT(board.cards.size() == 1 && board.cards[0].index == 0 && board.events == 3);
	query = BoardQuery();
	query.folded = {second_event};
	board = mission_board(m, names, query);
	TEST_EXPECT(!board.cards[0].folded && board.cards[1].folded);

	// A running game's report: its event's state on the card that is the played event at its place (S24 Live).
	LiveLogic live;
	live.mission = m.path();
	for (const Node *row : m.rows_of(MissionKind::Event)) live.played.push_back({row->id, {}});
	LiveEvent fired;
	fired.index = 1;
	fired.fired = true;
	live.events.push_back(fired);
	board = mission_board(m, names, BoardQuery(), &live);
	TEST_EXPECT(!board.cards[0].live && board.cards[1].live && board.cards[1].live->fired);
	// Another row at that place than the played one (moved since Play started): none of the game's state.
	std::swap(live.played[1], live.played[2]);
	board = mission_board(m, names, BoardQuery(), &live);
	TEST_EXPECT(!board.cards[1].live);
	return 0;
}

// The variables the events use (S24 A7, the board's Variables fold): by number with their uses in the events'
// order, named as the mission's names name them; a new one the lowest number no event uses.
int test_variables() {
	std::unique_ptr<Document> document = open(fixture_bytes(), "synth_logic.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const auto &m = dynamic_cast<const MissionDocument &>(*document);
	const DocumentMissionNames names(m);
	TEST_EXPECT(mission_variables(m, names).empty() && mission_new_variable(m) == 0);
	const NodeId first = m.rows_of(MissionKind::Event)[0]->id, second = m.rows_of(MissionKind::Event)[1]->id;
	const LogicType *set = logic_type(true, int32_t(bms::ActionType::MisvarChange), 1);
	const LogicType *equals = logic_type(false, int32_t(bms::TriggerMainType::MissionVariable), 1);
	std::vector<Edit> edits;
	std::string error;
	TEST_EXPECT(set && equals);
	TEST_EXPECT(logic_add_edits(m, first, *set, SIZE_MAX, edits, error, {{0, 0}, {1, 1}}) && apply_edits(*document, edits));
	TEST_EXPECT(logic_add_edits(m, second, *equals, SIZE_MAX, edits, error, {{0, 0}, {1, 1}}) && apply_edits(*document, edits));
	TEST_EXPECT(logic_add_edits(m, second, *set, SIZE_MAX, edits, error, {{0, 2}, {1, 5}}) && apply_edits(*document, edits));
	const std::vector<MissionVariable> variables = mission_variables(m, names);
	TEST_EXPECT(variables.size() == 2 && variables[0].index == 0 && same(variables[0].name, "V0") && variables[0].uses.size() == 2);
	TEST_EXPECT(variables[0].uses[0].event_index == 0 && variables[0].uses[0].action && same(variables[0].uses[0].words, "set V0 to 1"));
	TEST_EXPECT(variables[0].uses[1].event_index == 1 && !variables[0].uses[1].action && same(variables[0].uses[1].words, "V0 equals 1"));
	TEST_EXPECT(variables[1].index == 2 && variables[1].uses.size() == 1);
	TEST_EXPECT(mission_new_variable(m) == 1);
	// The script's V1 taken too (its caller's): the next free one.
	TEST_EXPECT(mission_new_variable(m, {1}) == 3);
	return 0;
}

// The board op over a session: the cards as the Logic pane shows them (its filter the workspace's), the args'
// own filter, the Common types and the recipes.
int test_board_op() {
	test_temp::TempDir dir("opennova_mission_board_op");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto ask = [&](const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query("mission_logic", parsed, error);
		if (!error.empty()) std::printf("  refused: %s\n", error.c_str());
		return answer;
	};
	JsonValue board = ask("{\"op\": \"board\"}");
	const JsonValue *cards = board.get("cards");
	TEST_EXPECT(cards && cards->array.size() == 2 && board.get_number("events", 0) == 2 && board.get_number("count", 0) == 2);
	const JsonValue &first = cards->array[0];
	TEST_EXPECT(same(first.get_string("title", ""), "Event 1") && first.get("triggers") && first.get("triggers")->array.size() == 1);
	const JsonValue &line = first.get("triggers")->array[0];
	TEST_EXPECT(line.get("runs") && line.get("runs")->array.size() == 3 && line.get("chips") && line.get("chips")->array.size() == 2);
	TEST_EXPECT(line.get_string("words", "").find(" is in Zone 20") != std::string::npos);
	TEST_EXPECT(board.get("recipes") && board.get("recipes")->array.size() == mission_recipes().size());
	TEST_EXPECT(board.get("common_triggers") && board.get("common_triggers")->array.size() == 8);
	// The Logic pane's filter read, the args' own taking its place.
	session.handle(request::set_workspace(R"({"document": {"path": "missions/logic.bms", "logic_filter": "has fired"}})"));
	board = ask("{\"op\": \"board\"}");
	TEST_EXPECT(board.get("cards") && board.get("cards")->array.size() == 1 && same(board.get_string("filter", ""), "has fired"));
	board = ask("{\"op\": \"board\", \"filter\": \"\"}");
	TEST_EXPECT(board.get("cards") && board.get("cards")->array.size() == 2);
	board = ask("{\"op\": \"board\", \"filter\": \"\", \"limit\": 1, \"offset\": 1}");
	TEST_EXPECT(board.get("cards") && board.get("cards")->array.size() == 1 &&
	            board.get("cards")->array[0].get_number("index", -1) == 1);
	// A folded event removed leaves the folds (the workspace tidied after the request).
	const auto *m = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(m);
	if (!m) return 1;
	std::vector<Edit> made;
	std::string why;
	TEST_EXPECT(recipe_event_edits(*m, *mission_recipe("right_away"), NodeAddress(), made, why));
	editor_test::handle_to_end(session, request::edit_record("missions/logic.bms", made));
	const NodeId fresh = m->rows_of(MissionKind::Event).back()->id;
	session.handle(request::set_workspace(R"({"document": {"path": "missions/logic.bms", "logic_folded": [)" + std::to_string(fresh) + "]}}"));
	TEST_EXPECT(session.view().workspace.document("missions/logic.bms").logic_folded == std::vector<NodeId>{fresh});
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = {fresh, node_kind(MissionKind::Event), 0};
	editor_test::handle_to_end(session, request::edit_record("missions/logic.bms", remove));
	TEST_EXPECT(m->rows_of(MissionKind::Event).size() == 2 && session.view().workspace.document("missions/logic.bms").logic_folded.empty());
	return 0;
}

// The chips' requests (S24 A4): an edit_record with keep_selection adds without selecting what it made; Show
// text's number is a key of the mission's Triggered Text, picked by its words; set_string_text's create makes the
// next free key in the mission's own table (made from the mission text blank where the project has none, never
// MEDMSSN.BIN), its text, and sets the field to its number, again the next.
int test_text_and_selection() {
	test_temp::TempDir dir("opennova_mission_board_text");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *m = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(m);
	if (!m) return 1;
	const std::string path = m->path();
	const SessionView &v = session.view();
	const Node *first = m->rows_of(MissionKind::Event)[0];
	session.handle(request::select_record(path, {first->id, first->kind, 0}));
	// Show text added with keep_selection: the event stays selected.
	const LogicType *show = logic_type(true, int32_t(bms::ActionType::OutputText), 0);
	std::vector<Edit> edits;
	std::string error;
	TEST_EXPECT(show && logic_add_edits(*m, first->id, *show, SIZE_MAX, edits, error));
	EditorRequest add = request::edit_record(path, edits);
	add.keep_selection = true;
	editor_test::handle_to_end(session, add);
	TEST_EXPECT(v.documents.selection.primary == NodeAddress({first->id, first->kind, 0}) && v.documents.selection.records.size() == 1);
	const std::vector<Document::Collection> lists = m->collections_of({first->id, first->kind, 0});
	TEST_EXPECT(lists.size() == 2 && lists[1].ids.size() == 2);
	const NodeAddress action{first->id, node_kind(MissionKind::Action), lists[1].ids[1]};
	// Its number a key of the mission's Triggered Text.
	const FieldSchema *param1 = nullptr;
	for (const FieldSchema &field : m->fields(action.kind))
		if (field.id == "param1") param1 = &field;
	TEST_EXPECT(param1);
	if (!param1) return 1;
	const FieldUse use = m->field_on(action, *param1);
	TEST_EXPECT(use.picks == ReferenceKind::TextId && use.key_prefix && std::string(use.key_prefix) == "ID" &&
	            same(use.scope, "LOGIC.BIN/Triggered Text") && same(use.scope_alternate, "MEDMSSN.BIN"));
	// The project has no LOGIC.BIN: New text makes it, the key ID001 in a new Triggered Text section, the field 1.
	TEST_EXPECT(!v.project.scan->find("logic.bin"));
	EditorRequest create = request::set_string_text(path, action, "param1", "Get to the bridge");
	create.create = true;
	editor_test::handle_to_end(session, create);
	const AssetEntry *table = v.project.scan->find("logic.bin");
	TEST_EXPECT(table && !v.project.scan->find("medmssn.bin"));
	if (!table) return 1;
	const auto *strings = dynamic_cast<const StringsDocument *>(session.document_for(table->relative_path));
	TEST_EXPECT(strings);
	if (!strings) return 1;
	TEST_EXPECT(same(strings->table().get_in_section("Triggered Text", "ID001"), "Get to the bridge"));
	Value value;
	TEST_EXPECT(m->get(action, "param1", value) && value == Value(int64_t(1)));
	TEST_EXPECT(v.documents.active == path && v.documents.selection.primary == NodeAddress({first->id, first->kind, 0}));
	// Again: the next free key.
	create.values = {{"text", "Hold the line"}};
	editor_test::handle_to_end(session, create);
	TEST_EXPECT(same(strings->table().get_in_section("Triggered Text", "ID002"), "Hold the line"));
	TEST_EXPECT(m->get(action, "param1", value) && value == Value(int64_t(2)));
	// A field that forms no key: refused.
	EditorRequest wrong = request::set_string_text(path, {first->id, first->kind, 0}, "delay", "x");
	wrong.create = true;
	const ActionOutcome refused = editor_test::handle_to_end(session, wrong);
	TEST_EXPECT(m->get(action, "param1", value) && value == Value(int64_t(2)));
	(void)refused;
	return 0;
}

// S24 A8, the acceptance over the wire, as an editor MCP client sends it (handle_json, query): the viewport command
// new_event over area X (the player's recipe), mission_logic add of Show text to the new event (kept selected),
// set_string_text with create, edit_record of the wait, then the board's card reading it.
int test_wire_acceptance() {
	test_temp::TempDir dir("opennova_mission_board_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *m = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(m);
	if (!m) return 1;
	const auto send = [&](const std::string &text) {
		JsonValue json;
		std::string error;
		opennova::io::json_parse(text, json, error);
		const JsonValue answer = session.handle_json(json);
		session.run_operations();
		if (!answer.get_bool("ok", false)) std::printf("  refused: %s\n%s\n", text.c_str(), opennova::io::json_write(answer).c_str());
		return answer.get_bool("ok", false);
	};
	const auto ask = [&](const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query("mission_logic", parsed, error);
		if (!error.empty()) std::printf("  refused: %s\n", error.c_str());
		return answer;
	};
	const Node *area = m->rows_of(MissionKind::Area)[0];
	// 1. New event over area X.
	TEST_EXPECT(send(R"({"kind": "edit_in_viewport", "path": "missions/logic.bms", "command": {"kind": "mission", "name": "new_event", "ids": [)" +
	                 std::to_string(area->id) + R"(], "value": "player_in_area"}})"));
	const std::vector<const Node *> events = m->rows_of(MissionKind::Event);
	TEST_EXPECT(events.size() == 3 && session.view().workspace.document(m->path()).logic);
	if (events.size() != 3) return 1;
	const NodeId made = events.back()->id;
	// 2. Show text added to it, the selection kept.
	const JsonValue planned = ask(R"({"path": "missions/logic.bms", "op": "add", "id": )" + std::to_string(made) +
	                              R"(, "list": "action", "type": 6})");
	const JsonValue *edits = planned.get("edits");
	TEST_EXPECT(edits && !edits->array.empty());
	if (!edits) return 1;
	TEST_EXPECT(send(R"({"kind": "edit_record", "path": "missions/logic.bms", "keep_selection": true, "edits": )" +
	                 opennova::io::json_write(*edits) + "}"));
	const std::vector<Document::Collection> lists = m->collections_of({made, node_kind(MissionKind::Event), 0});
	TEST_EXPECT(lists.size() == 2 && lists[1].ids.size() == 1);
	if (lists.size() != 2 || lists[1].ids.size() != 1) return 1;
	// 3. Its text made.
	TEST_EXPECT(send(R"({"kind": "set_string_text", "path": "missions/logic.bms", "address": {"row": )" + std::to_string(made) +
	                 R"(, "kind": )" + std::to_string(node_kind(MissionKind::Action)) + R"(, "child": )" + std::to_string(lists[1].ids[0]) +
	                 R"(}, "field": "param1", "values": {"text": "Get to the bridge"}, "create": true})"));
	// Two files changed, its status says so: the table made (logic.bin, opened) and the mission, both unsaved until Save All.
	const AssetEntry *made_table = session.view().project.scan ? session.view().project.scan->find("logic.bin") : nullptr;
	TEST_EXPECT(made_table);
	if (!made_table) return 1;
	const std::string table_path = made_table->relative_path;
	const DocumentBase *table = session.document_for(table_path);
	TEST_EXPECT(table && table->dirty() && m->dirty());
	const std::string table_name = table_path.substr(table_path.find_last_of('/') + 1);
	TEST_EXPECT(same(session.view().activity.status,
	                 "Made ID001 in " + table_name + "'s Triggered Text ('Get to the bridge') and set logic.bms's param1 to it: both "
	                 "files changed, unsaved; Save All writes both."));
	// 4. The wait: 5 s, 5 steps.
	TEST_EXPECT(send(R"({"kind": "edit_record", "path": "missions/logic.bms", "keep_selection": true, "edits": [{"id": )" +
	                 std::to_string(made) + R"(, "op": "set", "field": "delay", "value": 5}]})"));
	// 5. The board's card reads it, once the project's names have read the table.
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(!m->dirty() && session.document_for(table_path) && !session.document_for(table_path)->dirty());
	const JsonValue board = ask(R"({"path": "missions/logic.bms", "op": "board", "filter": ""})");
	std::string sentence;
	if (const JsonValue *cards = board.get("cards"))
		for (const JsonValue &card : cards->array)
			if (card.get_number("event", 0) == double(made)) sentence = card.get_string("sentence", "");
	TEST_EXPECT(same(sentence, "When the player is in Zone 20, then, after 5.1 s, show text 1: 'Get to the bridge'."));
	return 0;
}

// New text where the project has no table of the mission's name but the game install has one (a mod of a shipped
// mission): refused, its words saying to import the install's (it holds the mission's title and its other texts),
// and no table made that would hide it.
int test_new_text_install() {
	test_temp::TempDir dir("opennova_mission_board_install");
	const std::string retail = dir.file("game");
	TEST_EXPECT(test_io::write_text(retail + "/readme.txt", "game"));
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "logic.bin";
	blank.project_title = "Retail";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const opennova::pff::PffWriteEntry entries[] = {{"logic.bin", table.data(), uint32_t(table.size()), 0, 0, 0}};
	TEST_EXPECT(opennova::pff::pff_write_archive((retail + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	editor_test::set_game_install(session, retail);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *m = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(m && install_file_name(session.view(), "LOGIC.BIN") == "logic.bin");
	if (!m) return 1;
	const Node *first = m->rows_of(MissionKind::Event)[0];
	const LogicType *show = logic_type(true, int32_t(bms::ActionType::OutputText), 0);
	std::vector<Edit> edits;
	std::string reason;
	TEST_EXPECT(show && logic_add_edits(*m, first->id, *show, SIZE_MAX, edits, reason));
	editor_test::handle_to_end(session, request::edit_record(m->path(), edits));
	const std::vector<Document::Collection> lists = m->collections_of({first->id, first->kind, 0});
	TEST_EXPECT(lists.size() == 2 && lists[1].ids.size() == 2);
	if (lists.size() != 2 || lists[1].ids.size() != 2) return 1;
	EditorRequest create = request::set_string_text(m->path(), {first->id, node_kind(MissionKind::Action), lists[1].ids[1]}, "param1",
	                                                "Get to the bridge");
	create.create = true;
	editor_test::handle_to_end(session, create);
	const std::string &status = session.view().activity.status;
	TEST_EXPECT(status.find("the game install has logic.bin") != std::string::npos && status.find("import it") != std::string::npos);
	TEST_EXPECT(!session.view().project.scan->find("logic.bin"));
	return 0;
}

// The op table (S24 S0): the query's op param takes exactly the table's tokens, each answered by its own row.
int test_op_table() {
	const EditorQueryRow &row = editor_query_row(EditorQueryKind::MissionLogic);
	std::vector<std::string> offered;
	for (size_t c = 0; c < row.choice_count; ++c)
		if (std::string(row.choices[c].param) == "op")
			for (size_t i = 0; const char *token = row.choices[c].token(i); ++i) offered.push_back(token);
	std::vector<std::string> table;
	for (size_t i = 0; const char *token = mission_logic_op_choice(i); ++i) table.push_back(token);
	TEST_EXPECT(!table.empty() && offered == table);
	for (const char *op : {"form", "types", "add", "retype", "move", "negate", "join", "board"})
		TEST_EXPECT(std::find(table.begin(), table.end(), op) != table.end());

	test_temp::TempDir dir("opennova_mission_board_ops");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Board"));
	editor_test::create_missing_files(session);
	const std::string repo = test_paths_repo_root(__FILE__);
	TEST_EXPECT(test_io::write_file(session.view().project.root + "/missions/logic.bms",
	                                     test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const Node *event = mission->rows_of(MissionKind::Event)[1];
	const auto ask = [&](const std::string &args, std::string &error) {
		JsonValue parsed;
		opennova::io::json_parse(args, parsed, error);
		return session.query("mission_logic", parsed, error);
	};
	// Each op reaches its row: what it refuses is its own need, never an unknown op.
	for (const std::string &op : table) {
		std::string error;
		ask("{\"path\": \"missions/logic.bms\", \"op\": \"" + op + "\"}", error);
		TEST_EXPECT(error.find("no op") == std::string::npos);
	}
	std::string error;
	const JsonValue form = ask("{\"path\": \"missions/logic.bms\", \"op\": \"form\", \"id\": " + std::to_string(event->id) + "}", error);
	TEST_EXPECT(error.empty() && same(form.get_string("sentence", ""), "When event 1 has fired, then, after 5.1 s, kill group 2."));
	// An op the table has none of is refused by the param's choices, naming the ops.
	const JsonValue unknown = ask("{\"path\": \"missions/logic.bms\", \"op\": \"fold\"}", error);
	TEST_EXPECT(unknown.is_null() && error.find("form") != std::string::npos);
	return 0;
}

// Every shipped mission's board: every event a card, each line's runs its words, the notes counted by code.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission's board)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, cards = 0, lines = 0, largest = 0;
	std::map<std::string, size_t> notes;
	double took_ms = 0, slowest_ms = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const auto &m = dynamic_cast<const MissionDocument &>(*document);
			const DocumentMissionNames names(m);
			const bms::Header *header = m.mission_row() ? &m.mission_row()->native.header : nullptr;
			const auto started = std::chrono::steady_clock::now();
			const MissionBoard board = mission_board(m, names, BoardQuery());
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			took_ms += ms;
			if (ms > slowest_ms) slowest_ms = ms;
			TEST_EXPECT(board.cards.size() == board.events && board.events == m.rows_of(MissionKind::Event).size());
			largest = std::max(largest, board.cards.size());
			for (const BoardCard &card : board.cards) {
				++cards;
				const auto &event = static_cast<const EventRow &>(*m.row(card.event.row));
				TEST_EXPECT(card.triggers.size() == event.native.triggers.size() && card.actions.size() == event.native.actions.size());
				for (const BoardLine &line : card.triggers) {
					++lines;
					TEST_EXPECT(logic_runs_text(line.runs) == trigger_words(event.native.triggers[line.index], names));
				}
				for (const BoardLine &line : card.actions) {
					++lines;
					TEST_EXPECT(logic_runs_text(line.runs) == action_words(event.native.actions[line.index], names, header));
				}
				for (const BoardNote &note : card.notes) ++notes[note.code];
			}
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions' boards, %zu cards (at most %zu), %zu lines, in %.1f ms (the slowest %.1f ms); notes:",
	            missions, cards, largest, lines, took_ms, slowest_ms);
	for (const auto &[code, count] : notes) std::printf(" %s %zu", code.c_str(), count);
	std::printf("\n");
	TEST_EXPECT(missions == 115 && cards == 3073 && lines == 3858 + 4971);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_cards() != 0) return 1;
	if (test_variables() != 0) return 1;
	if (test_board_op() != 0) return 1;
	if (test_text_and_selection() != 0) return 1;
	if (test_wire_acceptance() != 0) return 1;
	if (test_new_text_install() != 0) return 1;
	if (test_op_table() != 0) return 1;
	return test_retail();
}
