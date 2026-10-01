#include <editor/ui/main_viewport_view.h>

#include <algorithm>

#include <editor/model/document_base.h>
#include <editor/ui/outline_view.h>
#include <editor/ui/viewport_view.h>
#include <editor/ui/viewport_views.h>

#include <imgui.h>

namespace opennova::editor {

MainViewportView::MainViewportView(const OutlineSpec &outline, ViewportKind kind) :
		outline_(std::make_unique<OutlineView>(outline)), viewport_(make_viewport_view(kind)), kind_(kind) {}

MainViewportView::~MainViewportView() = default;

void MainViewportView::draw(Workspace &workspace, const DocumentBase &document) {
	// The RevealRecord events its document was sent are the outline's, which shows the selection.
	for (const ViewEvent &event : take_events()) outline_->receive(event);
	// The outline in a column a third of the tab wide (wide enough to read), the viewport beside it.
	const float column = std::max(ImGui::GetFontSize() * 12.0f, ImGui::GetContentRegionAvail().x * 0.3f);
	if (ImGui::BeginChild("outline", ImVec2(column, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders))
		outline_->draw(workspace, document);
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("viewport", ImVec2(0.0f, 0.0f))) main_viewport(workspace, document);
	ImGui::EndChild();
}

void MainViewportView::rebind(const DocumentBase &document) {
	outline_->rebind(document);
}

bool MainViewportView::main_viewport(Workspace &workspace, const DocumentBase &document) {
	if (!viewport_) return false;
	viewport_->draw(workspace, document.path());
	return true;
}

OutlineModel *MainViewportView::outline() {
	return outline_->outline();
}

void MainViewportView::end_frame(Workspace &workspace) {
	if (viewport_) viewport_->end_frame(workspace);
}

} // namespace opennova::editor
