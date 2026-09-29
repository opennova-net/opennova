#pragma once

#include <cstdint>

#include <editor/assets/asset_kind.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_host.h>
#include <editor/ui/menu_preview_pane.h>
#include <editor/ui/model_preview_pane.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

// The two previews the Preview window shows one of.
enum class PreviewFamily : uint8_t { None, Menu, Model };

// The family of a document of `kind`: a menu, a stylesheet or a string table the menu
// preview's (the screen they draw with), a model, a clip or an animation table the model
// preview's; none for the rest.
PreviewFamily preview_family_of(AssetKind kind);

// The family the Preview window shows: the active document's when it has one, else the
// one it showed last (`last`; the menu's before it showed one). When that family has
// nothing to show (its document never opened, or closed), the other one does if it has
// something; none when neither has.
PreviewFamily preview_family(const SessionView &view, PreviewFamily last);

// The Preview window (ADR 0046 S11d), beside Document: the menu pane (the menu preview) or
// the model pane (the model preview), by preview_family, so a stylesheet's or a string
// table's tab keeps showing the menu screen it feeds and a catalog's tab keeps what was
// shown. A line above the pane names what it shows ("main.mnu - STARTUP", "skinned.3di",
// "walk.bad on skinned.3di"), marked while that file has unsaved changes; with nothing to
// show it says what to open. A pane that stops drawing ends a drag or a nudge in progress
// on it, its gesture's end raised once for the document it began in: when the window
// shows the other pane, and when the window itself does not draw (closed, collapsed, its
// tab hidden), which end_frame() catches. The window never takes the focus, and the
// shell's devices refresh and tick whether a pane shows or not.
class PreviewWindow : public devtools::Window {
public:
	explicit PreviewWindow(EditorHost &host) : host_(host), menu_(host), model_(host) { open = true; }
	const char *title() const override { return "Preview"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::CenterRight; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void set_menu_viewport(MenuPreviewViewport *viewport) { menu_.set_viewport(viewport); }
	void set_model_viewport(ModelPreviewViewport *viewport) {
		model_viewport_ = viewport;
		model_.set_viewport(viewport);
	}
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;
	// After each frame's windows, whether this one drew or not (the workspace's frame
	// bracket): a frame it did not draw ends both panes' gestures.
	void end_frame();

	// The family the window last showed (None before it showed one).
	PreviewFamily shown() const { return shown_; }

private:
	void header_(const SessionView &view, PreviewFamily family);

	EditorHost &host_;
	MenuPreviewPane menu_;
	ModelPreviewPane model_;
	ModelPreviewViewport *model_viewport_ = nullptr; // the model pane's: the line names the rig's model
	PreviewFamily shown_ = PreviewFamily::None;
	bool drawn_ = false; // drew this frame
};

} // namespace opennova::editor
