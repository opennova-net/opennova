#include <editor/ui/preview_window.h>

#include <algorithm>
#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/model/document_base.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_view.h>
#include <editor/ui/viewport_views.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const DocumentBase *open_document(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

} // namespace

PreviewWindow::PreviewWindow(Workspace &workspace) : workspace_(workspace) {
	open = true;
}

PreviewWindow::~PreviewWindow() = default;

bool preview_stands_aside(const SessionView &view) {
	const DocumentBase *document = open_document(view, view.documents.active);
	if (!document) return false;
	const DocumentTypeId type = asset_kind_row(document->kind()).document;
	const ViewportKind main = main_viewport_kind(type);
	return main != ViewportKind::kCount && viewport_kind_row(main).canvas && preview_kind_of(type) == ViewportKind::kCount;
}

bool PreviewWindow::stands_aside() const {
	return preview_stands_aside(workspace_.view());
}

ViewportView *PreviewWindow::view_of(const std::string &path, ViewportKind kind) {
	for (Slot &slot : views_)
		if (slot.kind == kind && slot.path == path) return slot.view.get();
	return nullptr;
}

ViewportView &PreviewWindow::view_for_(const std::string &path, ViewportKind kind) {
	if (ViewportView *view = view_of(path, kind)) return *view;
	Slot slot;
	slot.path = path;
	slot.kind = kind;
	slot.view = make_viewport_view(kind);
	views_.push_back(std::move(slot));
	return *views_.back().view;
}

void PreviewWindow::prune_(const SessionView &view) {
	views_.erase(std::remove_if(views_.begin(), views_.end(),
						 [&](const Slot &slot) {
							 return !view.documents.viewports ||
									 !view.documents.viewports->find(slot.path, slot.kind);
						 }),
			views_.end());
}

void PreviewWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = workspace_.view();
	prune_(view);
	const ViewportKind kind = view.documents.preview_shown;
	if (kind == ViewportKind::kCount) {
		ui_kit::empty_state("Open a menu, a model or an animation to preview it.");
		return;
	}
	const std::string &path = view.documents.previews[kind].path;
	header_(view, kind, path);
	// Each viewport in its own id scope: they share labels (the canvas, Play), never an item.
	ImGui::PushID(viewport_kind_token(kind));
	ImGui::PushID(path.c_str());
	view_for_(path, kind).draw(workspace_, path);
	ImGui::PopID();
	ImGui::PopID();
}

// A view not shown, or not drawn at all, lets go of what a press or a key began on it.
void PreviewWindow::end_frame() {
	for (Slot &slot : views_) slot.view->end_frame(workspace_);
}

// "main.mnu - STARTUP", "skinned.3di" or "walk.bad on skinned.3di", cut to the width the
// dot after it leaves; the whole of it, the path and whether it is unsaved in its tooltip.
void PreviewWindow::header_(const SessionView &view, ViewportKind kind, const std::string &path) {
	const DocumentBase *document = open_document(view, path);
	std::string text = basename_of(path);
	const ViewportModel *model = view.documents.viewports ? view.documents.viewports->find(path, kind) : nullptr;
	if (model) text += model->caption();
	const bool dirty = document && document->dirty();
	const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
	const float room = ImGui::GetContentRegionAvail().x - (dirty ? spacing + ui_kit::unsaved_dot_width() : 0.0f);
	ImGui::TextUnformatted(ui_kit::fit(text, room).c_str());
	ui_kit::tooltip(text + "\n" + path + (dirty ? "\nUnsaved changes" : ""));
	if (dirty) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::unsaved_dot();
	}
}

} // namespace opennova::editor
