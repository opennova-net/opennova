#include <editor/ui/mission_board_view.h>

#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_board_live.h>

namespace opennova::editor {

namespace {

class MissionBoardPane final : public DocumentPane {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &) override { revealing_ = false; }
	void reveal(const ViewEvent &event) override {
		revealed_ = event.address;
		revealing_ = true;
	}

private:
	NodeAddress revealed_;
	bool revealing_ = false;
};

void MissionBoardPane::draw(Workspace &workspace, const DocumentBase &document) {
	const auto *mission = dynamic_cast<const MissionDocument *>(records_of(document));
	if (!mission) return;
	const SessionView &view = workspace.view();
	const std::vector<const Node *> events = mission->rows_of(MissionKind::Event);
	ImGui::TextDisabled("Events (%zu)", events.size());
	ImGui::Separator();
	// The running game's report of this mission, drawn on the cards (lane C).
	const LiveLogic &live = view.activity.live_logic;
	const bool live_here = !live.empty() && live.mission == mission->path();
	const bool active = view.documents.active == mission->path();
	if (ImGui::BeginChild("cards")) {
		for (size_t i = 0; i < events.size(); ++i) {
			const NodeAddress event{events[i]->id, events[i]->kind, 0};
			ImGui::PushID(static_cast<int>(events[i]->id));
			const std::string title = mission->record_title(event);
			const float width = ImGui::GetContentRegionAvail().x;
			const ImVec2 size = ImGui::CalcTextSize(title.c_str(), nullptr, false, width);
			const ImVec2 at = ImGui::GetCursorPos();
			const bool selected = active && view.documents.selection.primary.row == event.row;
			if (ImGui::Selectable("##card", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, size.y)))
				window_requests::select(workspace, *mission, event);
			if (revealing_ && revealed_.row == event.row) {
				ImGui::SetScrollHereY(0.0f);
				revealing_ = false;
			}
			ImGui::SetCursorPos(at);
			ImGui::PushTextWrapPos(at.x + width);
			ImGui::TextUnformatted(title.c_str());
			ImGui::PopTextWrapPos();
			if (live_here) draw_card_live(workspace, mission->path(), live, CardLiveSpot{i, -1});
			ImGui::PopID();
		}
		// A record no card holds (its event gone since) reveals nothing.
		revealing_ = false;
	}
	ImGui::EndChild();
}

} // namespace

std::unique_ptr<DocumentPane> make_mission_board_pane() { return std::make_unique<MissionBoardPane>(); }

} // namespace opennova::editor
