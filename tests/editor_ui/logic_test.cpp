// A mission's logic in the Inspector over a real session (ui/mission_logic_view, ADR 0046 S15, Events and scripts;
// S24 A7: the board's card and line): an event selected shows its sentence and its card (its flags, its wait in
// seconds with the steps, its triggers and actions as lines of words and chips, + trigger and + action), and none
// of the format's raw fields; + action opens the type picker, and the type chosen is one EditRecord of the planned
// add, which the sentence then reads; a trigger selected shows its type by name, its negation and its line; an
// entity selected lists the events that name it, in words.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_table.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

constexpr NodeKind kind_of(MissionKind kind) { return node_kind(kind); }

// The session pumped as the Shell pumps it: frames drawn, what the windows raise handled, the
// operations and the validation run.
struct LogicRun {
	ProjectSession &session;
	Ui &ui;
	std::vector<EditorRequest> raised;
	void settle(int rounds = 4) {
		for (int i = 0; i < rounds; ++i) {
			ui.frames();
			EditorRequest request;
			while (ui.windows.take_request(request)) {
				raised.push_back(request);
				session.handle(request);
			}
			session.run_operations();
		}
	}
	std::vector<EditorRequest> take() {
		std::vector<EditorRequest> out;
		out.swap(raised);
		return out;
	}
};

// A popup's child window, as ImGui names it: the popup's window, then "<child>_<its id>".
ImGuiID popup_child(ImGuiID popup, const char *child) {
	char popup_name[32];
	std::snprintf(popup_name, sizeof(popup_name), "##Popup_%08x", popup);
	const ImGuiID popup_window = ImHashStr(popup_name);
	char name[96];
	std::snprintf(name, sizeof(name), "%s/%s_%08X", popup_name, child, ImHashStr(child, 0, popup_window));
	return ImHashStr(name);
}

void test_event_inspector() {
	editor_test::TempProjectDir dir("opennova_editor_ui_logic");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Logic"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/missions/logic.bms", test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")),
	      "the mission written");
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("missions/logic.bms"));
	session.run_operations();
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	CHECK(mission != nullptr, "the mission open");
	if (!mission) return;
	const std::string path = mission->path();
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const NodeAddress first{event->id, event->kind, 0};
	const Node *organic = mission->rows_of(MissionKind::Organic)[0];
	const std::string walker = "Organic #" + std::to_string(static_cast<const EntityRow &>(*organic).native.id);
	Ui ui;
	ui.windows.set_view(&v);
	LogicRun run{session, ui, {}};

	// The event: its sentence and its form in words; the raw flag word and steps are not shown.
	session.handle(request::select_record(path, first));
	ui.focus("Inspector");
	run.settle();
	run.take();
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("When " + walker + " is in Zone 20, then re-arm event 2.") != std::string::npos, "the event's sentence");
	CHECK(in_order(text, {"Event 1", "Repeats", "At start", "At end", "Wait", "When", "is ", "in ", "Zone 20", "+ trigger", "Then",
	                      "re-arm ", "+ action"}),
	      "its card: flags, wait, its lines by their words and chips");
	CHECK(text.find("Reset after") == std::string::npos && text.find("ResetAfter") == std::string::npos &&
	              text.find("Bits...") == std::string::npos,
	      "none of the format's raw fields");

	// + action: the types by name; Blue team wins chosen is one EditRecord, the sentence reads it.
	const ImGuiWindow *card = nullptr;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "Inspector") && std::strstr(window->Name, "/card_"))
			card = window;
	CHECK(card != nullptr, "the Inspector's card");
	if (!card) return;
	ui.activate(item_id(card->ID, {"+ action"}));
	const ImGuiID popup = ImHashStr("types", 0, card->ID);
	CHECK(ImGui::IsPopupOpen(popup, ImGuiPopupFlags_None), "+ action opens the types");
	text = logged_frame(ui);
	CHECK(in_order(text, {"Common", "By group", "Objectives and the end"}) && text.find("Blue team wins") == std::string::npos,
	      "the types by name, Common first, then their groups folded");
	ui.activate(ImHashStr("Objectives and the end", 0, popup_child(popup, "types")));
	text = logged_frame(ui);
	CHECK(in_order(text, {"Objectives and the end", "Win a sub-goal", "Blue team wins"}), "a group opened: its types");
	const int blue = int(opennova::bms::ActionType::BlueWin);
	ui.activate(ImHashStr("Blue team wins", 0, pushed(pushed(ImHashStr("all", 0, popup_child(popup, "types")), blue), 0)));
	run.settle();
	const std::vector<EditorRequest> raised = run.take();
	const EditorRequest *add = only(raised, EditorRequestKind::EditRecord);
	CHECK(add && add->keep_selection && add->edits.size() == 1 && add->edits[0].operation == EditOperation::Add &&
	              add->edits[0].field == "action_type" && add->edits[0].value == Value(int64_t(blue)),
	      "one EditRecord: the action added by its type, the event kept selected");
	CHECK(mission->record_title(first) == "Event 1: When " + walker + " is in Zone 20, then re-arm event 2; end the round: the blue team (team 1) wins.",
	      "the sentence reads the new action");

	// The trigger: its type by name, its negation, its line; its event's sentence above.
	const std::vector<Document::Collection> lists = mission->collections_of(first);
	session.handle(request::select_record(path, {first.row, kind_of(MissionKind::Trigger), lists[0].ids[0]}));
	run.settle();
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Event 1", "/", "Trigger 1", "Event 1: When ", "Go to its event", "Type", "Entity is in an area",
	                      "Not: it holds when this is not so", "When", "is ", "in ", "Zone 20"}),
	      "the breadcrumb by name, then the trigger's form and its line");
	CHECK(text.find("Condition") == std::string::npos && text.find("Sub-type") == std::string::npos &&
	              text.find("Parameter 1") == std::string::npos,
	      "its raw condition, its sub-type and its read parameters left to the line");

	// The walker: the events that name it.
	session.handle(request::select_record(path, {organic->id, organic->kind, 0}));
	run.settle();
	ui.away();
	text = logged_frame(ui);
	const std::string use = "Event 1: " + walker + " is in Zone 20";
	CHECK(in_order(text, {"Used by 1 trigger or action of the events", use.c_str()}), "the walker's uses, in words");
	CHECK(text.find("Referenced by") == std::string::npos, "its own document's uses not listed twice");
	CHECK(overflowing().empty(), "nothing runs past its window");
}

// S24 A7: an event's outline line's right-click menu offers Add trigger and Add action (their types by name, Common
// first) and Show on board; a type chosen there is one edit_record that keeps the event selected.
void test_outline_logic_items() {
	MissionRun run("opennova_editor_ui_logic_row_menu");
	CHECK(run.open(), "the mission open");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	const Node *first = mission->rows_of(MissionKind::Event)[0];
	const ImGuiWindow *outline = MissionRun::drawn_window("/outline_column_");
	CHECK(outline != nullptr, "the outline column");
	if (!outline) return;
	std::string text;
	bool opened = false;
	for (float y = outline->InnerRect.Min.y + 2.0f; y < outline->InnerRect.Max.y && !opened; y += 6.0f) {
		run.ui.mouse(outline->InnerRect.Min.x + 40.0f, y);
		if (GImGui->HoveredId == 0) continue;
		run.ui.button(true, ImGuiMouseButton_Right);
		run.ui.button(false, ImGuiMouseButton_Right);
		text = logged_frame(run.ui);
		opened = text.find("Add trigger") != std::string::npos && GImGui->OpenPopupStack.Size == 1;
		if (!opened) {
			ImGui::ClosePopupsExceptModals();
			run.ui.frames(1);
		}
	}
	CHECK(opened && in_order(text, {"Go to definition", "Find usages", "Add trigger", "Add action", "Show on board"}),
	      "an event's line: its jumps, then Add trigger, Add action and Show on board");
	if (!opened) return;
	run.settle();
	run.take();
	CHECK(v.documents.selection.primary == NodeAddress({first->id, first->kind, 0}), "the right click selected the event");
	const ImGuiWindow *popup = GImGui->OpenPopupStack.Size ? GImGui->OpenPopupStack.back().Window : nullptr;
	if (!popup) return;
	run.ui.activate(item_id(popup->ID, {"Add trigger"}));
	text = logged_frame(run.ui);
	CHECK(in_order(text, {"Common", "Entity is destroyed"}), "Add trigger: the Common types first");
	const LogicType *destroyed = logic_common_types(false)[0];
	const ImGuiWindow *submenu = GImGui->OpenPopupStack.Size ? GImGui->OpenPopupStack.back().Window : nullptr;
	CHECK(submenu != nullptr && submenu != popup, "its submenu open");
	if (!submenu || submenu == popup) return;
	run.ui.activate(ImHashStr(destroyed->title, 0, ImHashData(&destroyed, sizeof(destroyed), submenu->ID)));
	run.settle();
	const std::vector<EditorRequest> raised = run.take();
	const EditorRequest *add = only(raised, EditorRequestKind::EditRecord);
	CHECK(add && add->keep_selection && !add->edits.empty() && add->edits[0].operation == EditOperation::Add &&
	              add->edits[0].field == "main_type",
	      "a type chosen: one edit_record of the add, the selection kept");
	const std::vector<Document::Collection> lists = mission->collections_of({first->id, first->kind, 0});
	CHECK(lists.size() == 2 && lists[0].ids.size() == 2, "the trigger added");
	CHECK(v.documents.selection.primary == NodeAddress({first->id, first->kind, 0}), "the event still selected");
}

} // namespace

void run_logic_tests() {
	test_event_inspector();
	test_outline_logic_items();
}

} // namespace editor_ui_test
