#include <editor/ui/main_viewport_view.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

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
	// The column keeps the width it was dragged to, within the tab: never narrower than a few words,
	// never so wide that the viewport beside it loses its own least width (a tab narrowed since, a
	// window docked smaller than it first drew at; a viewport's toolbar wraps and narrows its
	// controls down to that width).
	// S15: the viewport keeps room of its own (24 lines' width in a wide tab, the least in a narrow one,
	// growing between) however wide the outline is dragged, and until the outline is dragged its width
	// follows the tab as the tab settles (a resizable child takes its first width once: a tab first
	// drawn wider than its dock, as a docked window is for its first frames, left the outline a column
	// of most of the tab and the picture a strip).
	const float avail = ImGui::GetContentRegionAvail().x;
	const float least = ImGui::GetFontSize() * 6.0f;
	const float room = std::clamp(avail - ImGui::GetFontSize() * 30.0f, least, std::max(least, ImGui::GetFontSize() * 24.0f));
	const float widest = std::max(least, avail - room);
	const float column = std::clamp(std::max(ImGui::GetFontSize() * 12.0f, avail * 0.3f), least, widest);
	const bool follow = !column_dragged_ && avail != column_avail_;
	if (follow) ImGui::SetNextWindowSize(ImVec2(column, 0.0f), ImGuiCond_Always);
	else ImGui::SetNextWindowSizeConstraints(ImVec2(least, 0.0f), ImVec2(widest, FLT_MAX));
	ImGui::BeginDisabled(!workspace.view().allows(EditorRequestKind::EditRecord));
	float width = column;
	if (ImGui::BeginChild("outline_column", ImVec2(column, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders)) {
		width = ImGui::GetWindowWidth();
		outline_->draw(workspace, document);
	}
	ImGui::EndChild();
	ImGui::EndDisabled();
	// A width that moved while the tab stood is the author's drag: kept from then on.
	if (!follow && column_width_ > 0.0f && avail == column_avail_ && std::fabs(width - column_width_) > 0.5f)
		column_dragged_ = true;
	column_width_ = width;
	column_avail_ = avail;
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
