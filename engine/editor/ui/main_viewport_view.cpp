#include <editor/ui/main_viewport_view.h>

#include <algorithm>

#include <editor/model/document_base.h>
#include <editor/session/view/session_view.h>
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
	// The outline's lines select and its tools edit: held back while an operation holds the documents
	// (S13 A3), as the Document window holds back a view of records; the canvas beside it is not. The
	// columns' ids are their own: a view of records drawn in the same tab before (the document read
	// again as another type) leaves an `outline` child whose width a resizable column would keep.
	const float column = std::max(ImGui::GetFontSize() * 12.0f, ImGui::GetContentRegionAvail().x * 0.3f);
	ImGui::BeginDisabled(!workspace.view().allows(EditorRequestKind::EditRecord));
	if (ImGui::BeginChild("outline_column", ImVec2(column, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders))
		outline_->draw(workspace, document);
	ImGui::EndChild();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::BeginChild("viewport_column", ImVec2(0.0f, 0.0f))) main_viewport(workspace, document);
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
