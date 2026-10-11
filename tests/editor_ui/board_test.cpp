// The Event board in the mission's tab over a real session (ADR 0046 S24, The board). The pane: the 3D view's Logic
// tool raises one set_workspace that shows the mission's Logic pane, which then shares the viewport's column with
// the canvas (both drawn, nothing past its room); Ctrl+L hides it again, as the Windows menu's Logic tick does.
// The cards (ui/mission_card): each event a card, its title, its triggers and actions as lines of words and chips;
// a card's title clicked selects its event; Down the next; "+ action" opens the type picker, Common first, then
// the groups, and a type chosen there is one edit_record that keeps the selection; Show text's chip then opens on
// New text, whose words typed make the mission's text table's next key (set_string_text's create) and the line
// reads them; the wait chip takes seconds (5 typed is 5 steps); the filter as the workspace holds it, and a card
// folded to its sentence.
#include <algorithm>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_card.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

// The cards drawn last frame, top to bottom.
std::vector<const ImGuiWindow *> card_windows() {
	std::vector<const ImGuiWindow *> out;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "/card_")) out.push_back(window);
	std::sort(out.begin(), out.end(), [](const ImGuiWindow *a, const ImGuiWindow *b) { return a->Pos.y < b->Pos.y; });
	return out;
}

// A popup's child, as ImGui names it: the popup's window, then "<child>_<its id>".
ImGuiID popup_child(ImGuiID popup, const char *child) {
	char popup_name[32];
	std::snprintf(popup_name, sizeof(popup_name), "##Popup_%08x", popup);
	const ImGuiID popup_window = ImHashStr(popup_name);
	char name[96];
	std::snprintf(name, sizeof(name), "%s/%s_%08X", popup_name, child, ImHashStr(child, 0, popup_window));
	return ImHashStr(name);
}

void show_logic(MissionRun &run) {
	window_requests::show_logic(run.ui.windows, run.path, true);
	run.settle();
	run.take();
}

void test_logic_pane() {
	MissionRun run("opennova_editor_ui_board_pane");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	const ImGuiWindow *column = MissionRun::drawn_window("/viewport_column_");
	CHECK(column != nullptr && !MissionRun::drawn_window("/logic_pane_"), "the 3D view's column, no pane yet");
	if (!column) return;

	// The toolbar's Logic: one set_workspace showing the pane.
	run.ui.activate(item_id(column->ID, {"Logic"}));
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	const std::vector<opennova::io::JsonValue> shown = workspace_sets(raised, "document", "logic");
	CHECK(raised.size() == 1 && shown.size() == 1 && shown[0].is_bool() && shown[0].boolean, "Logic: one set_workspace, logic on");
	CHECK(v.workspace.document(run.path).logic, "the session holds the pane shown");
	run.ui.away();
	const std::string text = logged_frame(run.ui);
	CHECK(MissionRun::drawn_window("/logic_pane_") && MissionRun::drawn_window("/canvas_column_"),
	      "the column split: the canvas and the pane both drawn");
	CHECK(run.devices.held(run.path, ViewportKind::Mission)->draws > 0, "the canvas still drawn");
	CHECK(in_order(text, {"New event", "2 of 2 events", "Event 1", "When", "is ", "in ", "Then", "re-arm ", "Event 2", "When",
	                      "has ", "fired"}),
	      "the events as cards, their lines' words and chips");
	CHECK(overflowing().empty(), "nothing runs past its room");
	run.take();

	// A card's title clicked selects its event.
	const std::vector<const ImGuiWindow *> cards = card_windows();
	CHECK(cards.size() == 2, "two cards drawn");
	if (cards.size() == 2) {
		run.ui.activate(item_id(cards[0]->ID, {"Event 1"}));
		run.settle();
		const Node *first = mission->rows_of(MissionKind::Event)[0];
		const std::vector<EditorRequest> clicked = run.take();
		const EditorRequest *select = only(clicked, EditorRequestKind::SelectRecord);
		CHECK(select && select->address.row == first->id && !select->address.child, "a card's title: one SelectRecord of its event");
		CHECK(v.documents.selection.primary.row == first->id, "the session holds it");
	}

	// Ctrl+L hides the pane.
	run.ui.away();
	raised = run.ui.chord({ImGuiMod_Ctrl, ImGuiKey_L});
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	const std::vector<opennova::io::JsonValue> hidden = workspace_sets(raised, "document", "logic");
	CHECK(hidden.size() == 1 && hidden[0].is_bool() && !hidden[0].boolean && !v.workspace.document(run.path).logic,
	      "Ctrl+L: the pane hidden");
	run.ui.frames(2);
	CHECK(!MissionRun::drawn_window("/logic_pane_"), "no pane drawn");

	// The Windows menu's tick shows it again.
	raised = choose(run.ui, "Windows", {"Logic"});
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	CHECK(workspace_sets(raised, "document", "logic").size() == 1 && v.workspace.document(run.path).logic,
	      "the Windows menu's Logic: the pane shown");
}

void test_cards() {
	MissionRun run("opennova_editor_ui_board_cards");
	CHECK(run.open(), "the mission open");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	show_logic(run);
	const Node *first = mission->rows_of(MissionKind::Event)[0];
	const Node *second = mission->rows_of(MissionKind::Event)[1];
	std::vector<const ImGuiWindow *> cards = card_windows();
	CHECK(cards.size() == 2, "two cards");
	if (cards.size() != 2) return;
	run.ui.activate(item_id(cards[0]->ID, {"Event 1"}));
	run.settle();
	run.take();

	// Down: the next card.
	ImGui::FocusWindow(const_cast<ImGuiWindow *>(cards[0]));
	run.ui.frames(1);
	std::vector<EditorRequest> raised = run.ui.chord({ImGuiKey_DownArrow});
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	CHECK(v.documents.selection.primary.row == second->id, "Down: the next card selected");
	run.take();

	// + action on the first card: the type picker, Common first; Show text chosen is one edit_record kept.
	cards = card_windows();
	run.ui.activate(item_id(cards[0]->ID, {"Event 1"}));
	run.settle();
	run.take();
	cards = card_windows();
	run.ui.activate(item_id(cards[0]->ID, {"+ action"}));
	const ImGuiID types = ImHashStr("types", 0, cards[0]->ID);
	CHECK(ImGui::IsPopupOpen(types, ImGuiPopupFlags_None), "+ action opens the type picker");
	std::string text = logged_frame(run.ui);
	CHECK(in_order(text, {"Common", "Send group along a path", "Show text"}), "Common first, then the groups");
	const int show = int(opennova::bms::ActionType::OutputText);
	run.ui.activate(ImHashStr("Show text", 0, pushed(pushed(ImHashStr("all", 0, popup_child(types, "types")), show), 0)));
	run.settle();
	raised = run.take();
	const EditorRequest *add = only(raised, EditorRequestKind::EditRecord);
	CHECK(add && add->keep_selection && !add->edits.empty() && add->edits[0].operation == EditOperation::Add &&
	              add->edits[0].value == Value(int64_t(show)),
	      "Show text: one edit_record of the add, the selection kept");
	CHECK(v.documents.selection.primary == NodeAddress({first->id, first->kind, 0}), "the card stays selected");
	const std::vector<Document::Collection> lists = mission->collections_of({first->id, first->kind, 0});
	CHECK(lists.size() == 2 && lists[1].ids.size() == 2, "the action added last");
	if (lists.size() != 2 || lists[1].ids.size() != 2) return;
	// Its chip opened on New text: the words typed and Enter make the next key of LOGIC.BIN.
	run.ui.frames(2);
	CHECK(GImGui->OpenPopupStack.Size == 1, "the New text popup open");
	ImGui::GetIO().AddInputCharactersUTF8("Get to the bridge");
	run.ui.frames(1);
	raised = run.ui.chord({ImGuiKey_Enter});
	const EditorRequest *create = only(raised, EditorRequestKind::SetStringText);
	CHECK(create && create->create && create->field == "param1" && create->address.child == lists[1].ids[1] &&
	              create->values.size() == 1 && create->values[0].second == "Get to the bridge",
	      "Enter: one set_string_text that makes the string");
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	CHECK(v.project.scan->find("logic.bin") != nullptr, "the mission's text table made");
	Value value;
	CHECK(mission->get({first->id, node_kind(MissionKind::Action), lists[1].ids[1]}, "param1", value) && value == Value(int64_t(1)),
	      "Show text's number the new key's");
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(text.find("show ") != std::string::npos && text.find("text ") != std::string::npos, "the line reads Show text");

	// The wait chip: 5 typed is 5 steps, the selection kept.
	cards = card_windows();
	run.ui.activate(item_id(cards[0]->ID, {"##delay"}));
	ImGui::GetIO().AddInputCharactersUTF8("5");
	run.ui.frames(1);
	raised = run.ui.chord({ImGuiKey_Enter});
	const EditorRequest *wait = only(raised, EditorRequestKind::EditRecord);
	CHECK(wait && wait->keep_selection && wait->edits.size() == 1 && wait->edits[0].field == "delay" &&
	              wait->edits[0].value == Value(int64_t(5)),
	      "the wait chip: 5 s is 5 steps");
	for (const EditorRequest &request : raised) run.session.handle(request);
	run.settle();
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(text.find("(5 steps)") != std::string::npos, "the card reads its wait in steps");

	// The filter as the workspace holds it; a card folded to its sentence.
	run.session.handle(request::set_workspace(R"({"document": {"path": "missions/logic.bms", "logic_filter": "has fired"}})"));
	run.settle();
	CHECK(card_windows().size() == 1, "the filter: one card");
	run.session.handle(request::set_workspace(R"({"document": {"path": "missions/logic.bms", "logic_filter": ""}})"));
	run.settle();
	cards = card_windows();
	CHECK(cards.size() == 2, "both cards again");
	if (cards.size() != 2) return;
	run.ui.activate(item_id(cards[1]->ID, {"fold"}));
	run.settle();
	CHECK(v.workspace.document(run.path).logic_folded == std::vector<NodeId>{second->id}, "the second card folded");
	run.ui.away();
	text = logged_frame(run.ui);
	CHECK(text.find("When event 1 has fired, then, after 5.1 s, kill group 2.") != std::string::npos, "folded: its sentence");
	CHECK(overflowing().empty(), "nothing runs past its room");
}

// A6: a trigger's line dragged by its grip onto the other card's "+ trigger" moves it there: one edit_record (added
// there, removed here, one undo step), the selection kept; a card's title is a record drag of its event.
void test_line_drag() {
	MissionRun run("opennova_editor_ui_board_drag");
	CHECK(run.open(), "the mission open");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	show_logic(run);
	run.ui.away();
	std::vector<const ImGuiWindow *> cards = card_windows();
	CHECK(cards.size() == 2, "two cards");
	if (cards.size() != 2) return;
	const Node *first = mission->rows_of(MissionKind::Event)[0];
	const Node *second = mission->rows_of(MissionKind::Event)[1];
	// Where the second card's + trigger is: the point the mouse hovers it at.
	const ImGuiID plus = item_id(cards[1]->ID, {"+ trigger"});
	ImVec2 target(-1.0f, -1.0f);
	for (float y = cards[1]->Pos.y; y < cards[1]->Pos.y + cards[1]->Size.y && target.x < 0.0f; y += 2.0f)
		for (float x = cards[1]->Pos.x + 4.0f; x < cards[1]->Pos.x + 120.0f && target.x < 0.0f; x += 4.0f) {
			run.ui.mouse(x, y);
			if (GImGui->HoveredId == plus) target = ImVec2(x, y);
		}
	CHECK(target.x >= 0.0f, "the second card's + trigger found");
	// The first card's trigger grip: pressed and dragged, it carries the line.
	const float grip_x = cards[0]->Pos.x + ImGui::GetStyle().WindowPadding.x + ImGui::GetFrameHeight() +
	                     ImGui::GetStyle().ItemSpacing.x + 3.0f;
	bool dragging = false;
	for (float y = cards[0]->Pos.y + 4.0f; y < cards[0]->Pos.y + cards[0]->Size.y && !dragging; y += 3.0f) {
		run.ui.mouse(grip_x, y);
		run.ui.button(true);
		run.ui.mouse(grip_x + 12.0f, y + 12.0f);
		run.ui.mouse(grip_x + 20.0f, y + 20.0f);
		const ImGuiPayload *payload = ImGui::GetDragDropPayload();
		dragging = payload && payload->IsDataType(kLogicLineDragPayload);
		if (!dragging) {
			run.ui.button(false);
			run.take();
		}
	}
	CHECK(dragging, "the trigger's grip drags its line");
	if (!dragging || target.x < 0.0f) return;
	run.ui.mouse(target.x, target.y);
	run.ui.mouse(target.x + 1.0f, target.y);
	run.ui.button(false);
	run.settle();
	const std::vector<EditorRequest> raised = run.take();
	const EditorRequest *move = only(raised, EditorRequestKind::EditRecord);
	CHECK(move && move->keep_selection && move->edits.size() >= 2 && move->edits.front().operation == EditOperation::Add &&
	              move->edits.back().operation == EditOperation::Remove,
	      "dropped: one edit_record, added there and removed here");
	const std::vector<Document::Collection> one = mission->collections_of({first->id, first->kind, 0});
	const std::vector<Document::Collection> two = mission->collections_of({second->id, second->kind, 0});
	CHECK(one.size() == 2 && one[0].ids.empty() && two.size() == 2 && two[0].ids.size() == 2, "the trigger moved to the second event");
}

} // namespace

void run_board_tests() {
	test_logic_pane();
	test_cards();
	test_line_drag();
}

} // namespace editor_ui_test
