// A mission's logic in the Inspector over a real session (ui/mission_logic_view, ADR 0046 S15, Events
// and scripts): an event selected shows its sentence and its form in words (repeats, the start and
// end passes, the wait in steps and seconds), its triggers and actions by their words with Add trigger
// and Add action, and none of the format's raw fields; Add action opens the types by name, and the
// one chosen is one EditRecord of the planned add, which the sentence then reads; a trigger selected
// shows its type by name, its negation and its words; an entity selected lists the events that name
// it, in words.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
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
	const std::string first_trigger = "1. " + walker + " is in Zone 20";
	CHECK(in_order(text, {"Once, at the mission's start", "Once, at the mission's end", "Repeats", "Wait before acting",
	                      "Triggers (1 of 20)", "Add trigger...", first_trigger.c_str()}) &&
	              in_order(text, {"Actions (1 of 20)", "Add action...", "1. re-arm event 2"}),
	      "its form in words, its lists by their words");
	CHECK(text.find("Reset after") == std::string::npos && text.find("ResetAfter") == std::string::npos &&
	              text.find("Bits...") == std::string::npos,
	      "none of the format's raw fields");

	// Add action: the types by name; Blue team wins chosen is one EditRecord, the sentence reads it.
	const ImGuiID inspector = Ui::window_id("Inspector");
	ui.activate(item_id(inspector, {"mission_event", "actions", "Add action..."}));
	const ImGuiID popup = item_id(inspector, {"mission_event", "actions", "add"});
	CHECK(ImGui::IsPopupOpen(popup, ImGuiPopupFlags_None), "Add action opens the types");
	text = logged_frame(ui);
	CHECK(in_order(text, {"Objectives and the end", "Win a sub-goal", "Blue team wins"}) && text.find("Group AI: Red alert") != std::string::npos,
	      "the types by name, in their groups");
	const int blue = int(opennova::bms::ActionType::BlueWin);
	ui.activate(ImHashStr("Blue team wins", 0, pushed(pushed(popup_child(popup, "types"), blue), 0)));
	run.settle();
	const std::vector<EditorRequest> raised = run.take();
	const EditorRequest *add = only(raised, EditorRequestKind::EditRecord);
	CHECK(add && add->edits.size() == 1 && add->edits[0].operation == EditOperation::Add && add->edits[0].field == "action_type" &&
	              add->edits[0].value == Value(int64_t(blue)),
	      "one EditRecord: the action added by its type");
	CHECK(mission->record_title(first) == "When " + walker + " is in Zone 20, then re-arm event 2; end the round: the blue team wins.",
	      "the sentence reads the new action");

	// The trigger: its type by name, its negation, its words; its event's sentence above.
	const std::vector<Document::Collection> lists = mission->collections_of(first);
	session.handle(request::select_record(path, {first.row, kind_of(MissionKind::Trigger), lists[0].ids[0]}));
	run.settle();
	ui.away();
	text = logged_frame(ui);
	const std::string reads = "Reads: " + walker + " is in Zone 20";
	CHECK(in_order(text, {"Event 1", "/", "Trigger 1", "Event 1: When ", "Go to its event", "Type", "Entity is in an area",
	                      "Not: it holds when this is not so", reads.c_str()}),
	      "the breadcrumb by name, then the trigger's form in words");
	CHECK(text.find("Condition") == std::string::npos && text.find("Sub-type") == std::string::npos,
	      "its raw condition and sub-type left to the format");

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

} // namespace

void run_logic_tests() { test_event_inspector(); }

} // namespace editor_ui_test
