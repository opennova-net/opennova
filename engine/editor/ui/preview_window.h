#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_kinds.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class ViewportView;
struct SessionView;

// The Document window's share of the centre in the workspace's first layout, the Preview's the rest
// (editor_layout): a menu's picture wants the room, its outline little.
inline constexpr float kDocumentShare = 0.375f;
// The width, in the font's ems, a table that feeds the Preview's picture needs beside it (a string
// table's key, section and text columns; a stylesheet's name, value and Pick): less, and the table goes
// first.
inline constexpr float kFeedTableRoomEm = 40.0f;

// The Preview window (ADR 0046 S11d, S13 V5), beside Document: the viewport of the kind the view
// says it shows (DocumentsView::preview_shown, viewport_kinds.h's preview_kind) over the document
// its target names (DocumentsView::previews), so a
// stylesheet's or a string table's tab keeps showing the menu screen it feeds and a catalog's tab
// keeps what was shown. A line above names what it shows ("main.mnu - STARTUP", "skinned.3di",
// "walk.bad on skinned.3di"), marked while that file has unsaved changes; with nothing to show it
// says what to open. A viewport view per (document, kind) it shows (ui/viewport_views), kept with
// its state (its canvas's zoom, its snap, its gesture) while the viewport is kept (its document
// open): each draws its viewport's device's picture on a canvas of its own, and a canvas that does
// not draw a frame ends the drag or the nudge in progress on it, its gesture's end raised once for
// the document it began in: when the window shows another viewport, when it has nothing to show,
// and when the window itself does not draw (closed, collapsed, its tab hidden), which end_frame()
// catches. The window never takes the focus, and the Shell's devices follow their viewports whether
// the window shows them or not.
//
// It steps aside (ADR 0046 S15; the UX round's project lane) while the active document has nothing to
// preview: a document no Preview-role kind shows or is fed by (a definition table, a text, a mission,
// whose picture is its own tab's), and a table that feeds one (a string table, a stylesheet) while no
// picture it feeds is open. Its dock node hides and Document takes the centre. For a table that feeds the
// picture shown (menutxt.bin beside main.mnu), the table comes first: it steps aside while the Document
// beside it would be narrower than kFeedTableRoomEm (document_room). It comes back, where it was docked,
// when a document it shows, or feeds with the room, is made active; it never takes the focus as it does.
class PreviewWindow : public devtools::Window {
public:
	explicit PreviewWindow(Workspace &workspace);
	~PreviewWindow() override;
	const char *title() const override { return "Preview"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::CenterRight; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	bool stands_aside() const override;
	// The Windows menu's tick while it stands aside: shown beside the document active now, until
	// another is made active. A Preview floated off the dockspace never steps aside (it frees no room).
	void show_anyway() override;
	bool focus_on_appearing() const override { return false; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;
	// After each frame's windows, whether this one drew or not (the workspace's frame
	// bracket): a viewport view whose canvas did not draw this frame ends its gesture.
	void end_frame();

	// The view of the viewport of `kind` over `path`, null until the window shows it.
	ViewportView *view_of(const std::string &path, ViewportKind kind);

private:
	struct Slot {
		std::string path;
		ViewportKind kind = ViewportKind::kCount;
		std::unique_ptr<ViewportView> view;
	};
	void header_(const SessionView &view, ViewportKind kind, const std::string &path);
	// The width the Document window has, or would have, beside it in the split they share (FLT_MAX when
	// they share none).
	float document_room() const;
	ViewportView &view_for_(const std::string &path, ViewportKind kind);
	// The views of viewports no longer kept go.
	void prune_(const SessionView &view);

	Workspace &workspace_;
	std::vector<Slot> views_;
	// The document the author asked to see the Preview beside (the Windows menu), until another is
	// active ("" none).
	mutable std::string shown_for_;
};

// Whether the Preview window steps aside over `view` whatever the room (PreviewWindow::stands_aside): a
// document is active and it has nothing of it to preview (no Preview-role kind shows its type or is fed
// by it, or it feeds one that has no picture open).
bool preview_stands_aside(const SessionView &view);
// Whether the active document is a table that feeds the picture the Preview has (a string table, a
// stylesheet beside an open menu), which the Preview leaves the room to when the Document lacks it.
bool preview_feeds_table(const SessionView &view);

} // namespace opennova::editor
