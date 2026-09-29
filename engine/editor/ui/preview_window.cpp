#include <editor/ui/preview_window.h>

#include <filesystem>
#include <string>

#include <editor/model/document.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

const Document *open_document(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

std::string file_name(const std::string &path) { return std::filesystem::path(path).filename().generic_string(); }

} // namespace

PreviewFamily preview_family_of(AssetKind kind) {
	switch (kind) {
	case AssetKind::Menu:
	case AssetKind::MenuStyle:
	case AssetKind::Strings: return PreviewFamily::Menu;
	case AssetKind::Model:
	case AssetKind::Animation:
	case AssetKind::AnimationMap: return PreviewFamily::Model;
	default: return PreviewFamily::None;
	}
}

PreviewFamily preview_family(const SessionView &view, PreviewFamily last) {
	PreviewFamily family = last;
	if (const Document *active = open_document(view, view.active_document))
		if (preview_family_of(active->kind()) != PreviewFamily::None) family = preview_family_of(active->kind());
	// What each has to show: the view keeps a preview's target until its document closes.
	const PreviewFamily menu = view.menu_preview.path.empty() ? PreviewFamily::None : PreviewFamily::Menu;
	const PreviewFamily model = view.model_preview.path.empty() ? PreviewFamily::None : PreviewFamily::Model;
	const auto either = [](PreviewFamily first, PreviewFamily second) { return first != PreviewFamily::None ? first : second; };
	return family == PreviewFamily::Model ? either(model, menu) : either(menu, model);
}

void PreviewWindow::draw(devtools::ImGuiPass &, uint64_t) {
	drawn_ = true;
	const SessionView &view = host_.view();
	const PreviewFamily family = preview_family(view, shown_);
	// The pane not shown lets go of what a press or a key began on it.
	if (family != PreviewFamily::Menu) menu_.end_gestures();
	if (family != PreviewFamily::Model) model_.end_gestures();
	if (family == PreviewFamily::None) {
		ui_kit::empty_state("Open a menu, a model or an animation to preview it.");
		return;
	}
	shown_ = family;
	header_(view, family);
	// Each pane in its own id scope: they share labels (the canvas, Play), never an item.
	ImGui::PushID(family == PreviewFamily::Menu ? "menu" : "model");
	if (family == PreviewFamily::Menu) menu_.draw();
	else model_.draw();
	ImGui::PopID();
}

void PreviewWindow::end_frame() {
	if (!drawn_) {
		menu_.end_gestures();
		model_.end_gestures();
	}
	drawn_ = false;
}

// "main.mnu - STARTUP", "skinned.3di" or "walk.bad on skinned.3di", cut to the width the
// dot after it leaves; the whole of it, the path and whether it is unsaved in its tooltip.
void PreviewWindow::header_(const SessionView &view, PreviewFamily family) {
	const std::string &path = family == PreviewFamily::Menu ? view.menu_preview.path : view.model_preview.path;
	const Document *document = open_document(view, path);
	std::string text = file_name(path);
	if (family == PreviewFamily::Menu) {
		if (const Node *screen = document ? document->row(view.menu_preview.screen) : nullptr)
			text += " - " + screen->name();
	} else if (model_viewport_) {
		const ModelPreviewModel &model = model_viewport_->model();
		if (model.shown_path() == path && model.animating() && !model.rig().model.empty()) text += " on " + model.rig().model;
	}
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
