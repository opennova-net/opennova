#include <editor/ui/preview_window.h>

#include <algorithm>
#include <cfloat>
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
#include <editor/ui/welcome_view.h>

#include <imgui.h>
#include <imgui_internal.h>

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

namespace {

// The Preview-role kind the active document's type reaches (shown or fed), kCount for none or no document.
ViewportKind active_preview_kind(const SessionView &view, DocumentTypeId &type) {
	const DocumentBase *document = open_document(view, view.documents.active);
	if (!document) return ViewportKind::kCount;
	type = asset_kind_row(document->kind()).document;
	return preview_kind_of(type);
}

} // namespace

bool preview_stands_aside(const SessionView &view) {
	DocumentTypeId type = DocumentTypeId::None;
	if (view.documents.active.empty()) return false; // nothing open: it says what to open
	const ViewportKind kind = active_preview_kind(view, type);
	// Nothing of the document shows in it: a definition table, a text, a mission (whose picture is its tab's).
	if (kind == ViewportKind::kCount) return true;
	// A table that feeds a picture (a string table, a stylesheet) with no picture of it open (no menu).
	return !viewport_kind_shows(kind, type) && view.documents.previews[kind].path.empty();
}

bool preview_feeds_table(const SessionView &view) {
	DocumentTypeId type = DocumentTypeId::None;
	const ViewportKind kind = active_preview_kind(view, type);
	return kind != ViewportKind::kCount && !viewport_kind_shows(kind, type) && !view.documents.previews[kind].path.empty();
}

// The room the Document window would have beside the Preview in the split they share (the default layout's
// centre): its width while the two show side by side; while the Preview stands aside, what ImGui's split
// would give it, from the sizes the two nodes ask for (the author's drag of the splitter kept in them): the
// central node takes what the other leaves, else the two share by their asks [imgui.cpp
// DockNodeTreeUpdatePosSize]. A large room where the two share no split (the author docked them
// otherwise), so the rule never takes the Preview away.
float PreviewWindow::document_room() const {
	if (ImGui::GetCurrentContext() == nullptr) return FLT_MAX;
	const ImGuiWindow *preview = ImGui::FindWindowByName(title());
	const ImGuiWindow *document = ImGui::FindWindowByName("Document");
	const ImGuiDockNode *p = preview && preview->DockId ? ImGui::DockBuilderGetNode(preview->DockId) : nullptr;
	const ImGuiDockNode *d = document && document->DockId ? ImGui::DockBuilderGetNode(document->DockId) : nullptr;
	if (!p || !d || !p->ParentNode || p->ParentNode != d->ParentNode || p->ParentNode->SplitAxis != ImGuiAxis_X)
		return FLT_MAX;
	if (p->IsVisible && d->IsVisible) return d->Size.x;
	const ImGuiStyle &style = ImGui::GetStyle();
	const float room = std::max(p->ParentNode->Size.x - style.DockingSeparatorSize, 0.0f);
	const float least = std::min(room, style.WindowMinSize.x * 2.0f) * 0.5f;
	if (d->HasCentralNodeChild && p->SizeRef.x > 0.0f) return room - std::min(room - least, p->SizeRef.x);
	if (p->HasCentralNodeChild && d->SizeRef.x > 0.0f) return std::min(room - least, d->SizeRef.x);
	const float asked = p->SizeRef.x + d->SizeRef.x;
	return asked > 0.0f ? room * d->SizeRef.x / asked : room * kDocumentShare;
}

bool PreviewWindow::stands_aside() const {
	const SessionView &view = workspace_.view();
	// With no project open, for the welcome page (aside_for_welcome).
	if (aside_for_welcome(view, welcome_asked_)) return true;
	if (!view.project.open) return false;
	// The author's ask (the Windows menu's tick) holds for the document active then.
	if (!shown_for_.empty() && shown_for_ != view.documents.active) shown_for_.clear();
	if (!shown_for_.empty()) return false;
	const bool aside = preview_stands_aside(view);
	if (!aside && !preview_feeds_table(view)) return false;
	// Floated off the workspace's dockspace (its own window, another monitor), stepping aside frees no
	// room beside Document: it stays.
	if (ImGui::GetCurrentContext() != nullptr) {
		const ImGuiWindow *window = ImGui::FindWindowByName(title());
		const ImGuiDockNode *node = window && window->DockId ? ImGui::DockBuilderGetNode(window->DockId) : nullptr;
		while (node && node->ParentNode) node = node->ParentNode;
		if (window && (!node || !node->IsDockSpace())) return false;
	}
	if (aside) return true;
	// A table that feeds the picture: the table first, while the Document beside the Preview would be
	// narrower than it needs.
	return ImGui::GetCurrentContext() != nullptr && document_room() < kFeedTableRoomEm * ImGui::GetFontSize();
}

void PreviewWindow::show_anyway() {
	if (!workspace_.view().project.open) welcome_asked_ = true;
	else shown_for_ = workspace_.view().documents.active;
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
