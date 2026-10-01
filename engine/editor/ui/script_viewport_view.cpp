#include <editor/ui/script_viewport_view.h>

#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

ScriptViewportView::ScriptViewportView() : ViewportView(ViewportKind::Script) {}

void ScriptViewportView::draw_empty(Workspace &, const ViewportModel *, const std::string &) {
	// Nothing of its own: the script view draws the document's lines in its place.
	placed_ = false;
}

void ScriptViewportView::draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &) {
	placed_ = false;
	ViewportDeviceSource *devices = workspace.devices();
	ViewportDevice *device = devices ? devices->device(model.path(), kind()) : nullptr;
	const ImVec2 room = ImGui::GetContentRegionAvail();
	if (!device || room.x < 1.0f || room.y < 1.0f) return;
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const ImVec2 end(at.x + room.x, at.y + room.y);
	// A Control lies over everything Dear ImGui draws: not placed where it would hide something drawn
	// over the rect, nor in another OS window than the one it is a Control of.
	const ImGuiViewport *main = ImGui::GetMainViewport();
	const ui_kit::Cover cover = ui_kit::cover_of(at.x, at.y, end.x, end.y);
	if (ImGui::GetWindowViewport() != main || ui_kit::covered(cover)) return;
	// The rect in the OS window's pixels (Dear ImGui's less the window's place on the screen where its
	// viewports put the windows on the desktop), and the part of the window it shows in.
	ViewportPicture picture;
	picture.x = at.x - main->Pos.x;
	picture.y = at.y - main->Pos.y;
	picture.width = int(room.x);
	picture.height = int(room.y);
	const ImVec2 clip_min = ImGui::GetWindowDrawList()->GetClipRectMin();
	const ImVec2 clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
	picture.clip_left = clip_min.x - main->Pos.x;
	picture.clip_top = clip_min.y - main->Pos.y;
	picture.clip_right = clip_max.x - main->Pos.x;
	picture.clip_bottom = clip_max.y - main->Pos.y;
	picture.canvas_sized = true;
	device->draw(picture);
	placed_ = true;
	placed_frame_ = ImGui::GetFrameCount();
	path_ = model.path();
	cover_ = cover;
	// The surface over the whole rect: a press there is this item's, never the window's (a drag that
	// selects text moves no window), and while the pointer is over it or a press there lasts, Dear
	// ImGui wants none of the pointer the next frame, so the control under it takes it. Hovered is
	// asked past an item that holds the pointer's input (a text field of another window being
	// typed in: the first press out of it is its to end, and this item is hovered all the same), or
	// that press would be Dear ImGui's and never reach the control.
	ImGui::SetCursorScreenPos(at);
	ImGui::InvisibleButton("##script_device", room,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemActive())
		ImGui::SetNextFrameWantCaptureMouse(false);
}

void ScriptViewportView::end_frame(Workspace &workspace) {
	ViewportView::end_frame(workspace);
	// A view whose tab no frame drew keeps nothing placed. A window or a modal begun after the tab this
	// frame was unknown to Dear ImGui when the tab asked (it knows the windows begun by then and those
	// drawn the frame before); with every window begun it is known, and over the rect it is the Control
	// that goes, this frame: a picture with no room hides the device, and the lines come the frame
	// after, as the tab asks again.
	if (placed_ && placed_frame_ != ImGui::GetFrameCount()) placed_ = false;
	if (!placed_ || !ui_kit::covered(cover_)) return;
	ViewportDeviceSource *devices = workspace.devices();
	if (ViewportDevice *device = devices ? devices->device(path_, kind()) : nullptr) device->draw(ViewportPicture());
	placed_ = false;
}

} // namespace opennova::editor
