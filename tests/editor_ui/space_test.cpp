// A mission's logic in its views over a real session (ADR 0046 S24, In the views). S0: Pick in view raises the
// set_workspace whose pick the session holds (a trigger's parameter slot), and ends it; an event's outline line's
// right-click menu offers the logic's items after the jumps (the mission's row menu), Show on board one
// set_workspace that shows the Logic pane.
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/record_drag.h>
#include <editor/ui/reference_picker.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

std::string right_click(Ui &ui, ImVec2 at) {
	ui.mouse(at.x, at.y);
	ui.button(true, ImGuiMouseButton_Right);
	ui.button(false, ImGuiMouseButton_Right);
	return logged_frame(ui);
}

void test_pick_and_row_menu() {
	MissionRun run("opennova_editor_ui_space_pick");
	CHECK(run.open(), "the mission open, its 3D view drawn");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const SessionView &v = run.session.view();
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const std::vector<Document::Collection> lists = mission->collections_of({event->id, event->kind, 0});
	CHECK(!lists.empty() && !lists[0].ids.empty(), "the first event's trigger");
	if (lists.empty() || lists[0].ids.empty()) return;
	const NodeAddress trigger{event->id, node_kind(MissionKind::Trigger), lists[0].ids[0]};

	// Pick in view: one set_workspace, the pick held; ended by a record of row 0.
	window_requests::pick_in_view(run.ui.windows, run.path, trigger, 1);
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	CHECK(raised.size() == 1 && workspace_sets(raised, "document", "pick").size() == 1, "Pick in view: one set_workspace");
	const WorkspaceView::Pick &pick = v.workspace.document(run.path).pick;
	CHECK(pick.active() && pick.record == trigger && pick.slot == 1, "the session holds the pick");
	window_requests::pick_in_view(run.ui.windows, run.path, NodeAddress{}, -1);
	run.settle();
	run.take();
	CHECK(!v.workspace.document(run.path).pick.active(), "the pick ended");

	// The outline: the first event's line, its menu's logic items after the jumps.
	const ImGuiWindow *outline = MissionRun::drawn_window("/outline_column_");
	CHECK(outline != nullptr, "the outline column");
	if (!outline) return;
	std::string text;
	bool opened = false;
	for (float y = outline->InnerRect.Min.y + 2.0f; y < outline->InnerRect.Max.y && !opened; y += 6.0f) {
		run.ui.mouse(outline->InnerRect.Min.x + 40.0f, y);
		if (GImGui->HoveredId == 0) continue;
		text = right_click(run.ui, ImVec2(outline->InnerRect.Min.x + 40.0f, y));
		opened = text.find("Show on board") != std::string::npos && GImGui->OpenPopupStack.Size == 1;
		if (!opened) {
			ImGui::ClosePopupsExceptModals();
			run.ui.frames(1);
		}
	}
	CHECK(opened && in_order(text, {"Go to definition", "Find usages", "Show on board"}),
	      "an event's line: its jumps, then the logic's Show on board");
	if (!opened) return;
	run.settle();
	run.take();
	const ImGuiWindow *popup = GImGui->OpenPopupStack.Size ? GImGui->OpenPopupStack.back().Window : nullptr;
	CHECK(popup != nullptr, "the menu open");
	if (!popup) return;
	run.ui.activate(item_id(popup->ID, {"Show on board"}));
	run.settle();
	raised = run.take();
	CHECK(workspace_sets(raised, "document", "logic").size() == 1 && v.workspace.document(run.path).logic,
	      "Show on board: the Logic pane shown");
}

// B3: a record dragged onto a value that names records of the mission (ReferencePicker::draw_field, the Inspector's
// and the board's chips): an area onto a trigger's zone gives its zone id; an entity onto it, or a record of
// another document, is never accepted.
struct DropProbe {
	Ui &ui;
	ReferencePicker picker;
	ImVec2 centre;
	explicit DropProbe(Ui &u) : ui(u) {}
	// One frame of the field drawn alone in a window of its own (its frame's centre kept), a record's drag held
	// by an outside source when `drag` names one; true when the field took a value, `picked` it.
	bool frame(const MissionDocument &mission, const NodeAddress &trigger, const FieldUse &field, const Value &value,
	           const RecordDrag *drag, std::string &picked) {
		ImGui::NewFrame();
		if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
			ImGui::SetDragDropPayload(kRecordDragPayload, drag, sizeof(*drag));
			ImGui::EndDragDropSource();
		}
		ImGui::SetNextWindowPos(ImVec2(1400.0f, 900.0f));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 100.0f));
		ImGui::Begin("record drop probe", nullptr, ImGuiWindowFlags_NoDecoration);
		DisplayName words;
		const bool took = picker.draw_field(ui.windows, mission, trigger, field, value, words, false, picked);
		centre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
		                (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
		ImGui::End();
		ImGui::Render();
		return took;
	}
	// The record dragged from outside and let go over the field: what the field took ("" nothing).
	std::string drop(const MissionDocument &mission, const NodeAddress &trigger, const FieldUse &field, const Value &value,
	                 const RecordDrag &drag) {
		std::string picked;
		frame(mission, trigger, field, value, nullptr, picked);
		ImGui::GetIO().AddMousePosEvent(centre.x, centre.y);
		frame(mission, trigger, field, value, nullptr, picked);
		ImGui::GetIO().AddMouseButtonEvent(0, true);
		for (int i = 0; i < 3; ++i) frame(mission, trigger, field, value, &drag, picked);
		ImGui::GetIO().AddMouseButtonEvent(0, false);
		bool took = frame(mission, trigger, field, value, &drag, picked);
		took = frame(mission, trigger, field, value, nullptr, picked) || took;
		return took ? picked : std::string();
	}
};

void test_record_drops() {
	MissionRun run("opennova_editor_ui_space_drops");
	CHECK(run.open(), "the mission open");
	const MissionDocument *mission = run.mission();
	if (!mission) return;
	const Node *event = mission->rows_of(MissionKind::Event)[0];
	const std::vector<Document::Collection> lists = mission->collections_of({event->id, event->kind, 0});
	if (lists.empty() || lists[0].ids.empty()) return;
	const NodeAddress trigger{event->id, node_kind(MissionKind::Trigger), lists[0].ids[0]};
	const FieldSchema *schema = nullptr;
	for (const FieldSchema &each : mission->fields(trigger.kind))
		if (each.id == "param2") schema = &each;
	CHECK(schema != nullptr, "the trigger's param2");
	if (!schema) return;
	const FieldUse zone = mission->field_on(trigger, *schema);
	CHECK(zone.reference == ReferenceKind::MissionZone, "event 1's trigger's param2 names a zone");
	Value value;
	mission->get(trigger, "param2", value);
	const Node *area = mission->rows_of(MissionKind::Area)[1]; // zone 30
	const Node *walker = mission->rows_of(MissionKind::Organic)[0];
	DropProbe probe(run.ui);
	RecordDrag drag;
	drag.document = mission->identity();
	drag.record = NodeAddress{area->id, area->kind, 0};
	CHECK(probe.drop(*mission, trigger, zone, value, drag) == "30", "an area dropped on a zone gives its zone id");
	drag.record = NodeAddress{walker->id, walker->kind, 0};
	CHECK(probe.drop(*mission, trigger, zone, value, drag).empty(), "an entity dropped on a zone is not taken");
	drag.record = NodeAddress{area->id, area->kind, 0};
	drag.document = mission->identity() + 1;
	CHECK(probe.drop(*mission, trigger, zone, value, drag).empty(), "a record of another document is not taken");
	// The entity slot takes the walker's SSN.
	for (const FieldSchema &each : mission->fields(trigger.kind))
		if (each.id == "param1") schema = &each;
	const FieldUse entity = mission->field_on(trigger, *schema);
	mission->get(trigger, "param1", value);
	drag.document = mission->identity();
	drag.record = NodeAddress{walker->id, walker->kind, 0};
	CHECK(probe.drop(*mission, trigger, entity, value, drag) ==
	          std::to_string(static_cast<const EntityRow &>(*walker).native.id),
	      "an entity dropped on an entity gives its SSN");
}

} // namespace

void run_space_tests() {
	test_pick_and_row_menu();
	test_record_drops();
}

} // namespace editor_ui_test
