#include <editor/ui/viewport_canvas.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <utility>

#include <editor/session/editor_request.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/workspace.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

constexpr float kMargin =
		12.0f; // around a design picture: the handles on its edges stay on the canvas
constexpr ImU32 kFrameColor = IM_COL32(90, 90, 90, 255);
constexpr ImU32 kSelectedColor = IM_COL32(255, 200, 60, 255);
constexpr ImU32 kHoverColor = IM_COL32(120, 190, 255, 220);
constexpr ImU32 kMarqueeColor = IM_COL32(140, 210, 255, 230);
constexpr ImU32 kMarqueeFill = IM_COL32(140, 210, 255, 30);
constexpr ImU32 kNoteColor = IM_COL32(255, 170, 40, 230);
constexpr ImU32 kSquareEdge = IM_COL32(40, 40, 40, 255); // a handle's edge
constexpr ImU32 kDotEdge = IM_COL32(20, 20, 20, 255); // a light's ring

// The next zoom step past `scale` in the wheel's direction (the last one at either end).
float next_zoom(float scale, bool in) {
	const float *levels = ViewportCanvas::kZoomLevels;
	const size_t count = std::size(ViewportCanvas::kZoomLevels);
	if (in) {
		for (size_t i = 0; i < count; ++i)
			if (levels[i] > scale + 0.001f)
				return levels[i];
		return levels[count - 1];
	}
	for (size_t i = count; i-- > 0;)
		if (levels[i] < scale - 0.001f)
			return levels[i];
	return levels[0];
}

ImU32 shape_color(const OverlayShape &shape) {
	switch (shape.role) {
		case OverlayRole::Selected:
			return kSelectedColor;
		case OverlayRole::Hover:
			return kHoverColor;
		case OverlayRole::Marquee:
			return kMarqueeColor;
		case OverlayRole::Note:
			return kNoteColor;
		case OverlayRole::Normal:
			break;
	}
	return IM_COL32(
			(shape.rgb >> 16) & 0xFF, (shape.rgb >> 8) & 0xFF, shape.rgb & 0xFF, shape.alpha);
}

void draw_shape(ImDrawList &paint, CanvasPoint origin, const OverlayShape &shape) {
	const auto at = [&](int i) {
		return ImVec2(origin.x + shape.points[i].x, origin.y + shape.points[i].y);
	};
	const ImU32 color = shape_color(shape);
	const float s = shape.size;
	switch (shape.kind) {
		case OverlayKind::Rect:
			if (shape.role == OverlayRole::Marquee)
				paint.AddRectFilled(at(0), at(1), kMarqueeFill);
			if (shape.filled)
				paint.AddRectFilled(at(0), at(1), color);
			else
				paint.AddRect(at(0), at(1), color, 0.0f, 0, shape.thickness);
			break;
		case OverlayKind::Line:
			paint.AddLine(at(0), at(1), color, shape.thickness);
			break;
		case OverlayKind::Circle:
			if (shape.filled)
				paint.AddCircleFilled(at(0), s, color);
			else
				paint.AddCircle(at(0), s, color, 0, shape.thickness);
			break;
		case OverlayKind::Quad:
			if (shape.filled)
				paint.AddQuadFilled(at(0), at(1), at(2), at(3), color);
			else
				paint.AddQuad(at(0), at(1), at(2), at(3), color, shape.thickness);
			break;
		case OverlayKind::Marker: {
			const ImVec2 p = at(0);
			switch (shape.glyph) {
				case OverlayGlyph::Square:
					paint.AddRectFilled(ImVec2(p.x - s, p.y - s), ImVec2(p.x + s, p.y + s), color);
					paint.AddRect(ImVec2(p.x - s, p.y - s), ImVec2(p.x + s, p.y + s), kSquareEdge);
					break;
				case OverlayGlyph::Dot:
					paint.AddCircleFilled(p, s, color);
					paint.AddCircle(p, s, kDotEdge);
					break;
				case OverlayGlyph::Cross:
					paint.AddLine(
							ImVec2(p.x - s, p.y), ImVec2(p.x + s, p.y), color, shape.thickness);
					paint.AddLine(
							ImVec2(p.x, p.y - s), ImVec2(p.x, p.y + s), color, shape.thickness);
					break;
				case OverlayGlyph::Corner:
					paint.AddTriangleFilled(p, ImVec2(p.x + s, p.y), ImVec2(p.x, p.y + s), color);
					break;
			}
			break;
		}
	}
}

SelectMode select_mode(CanvasJoin join) {
	switch (join) {
		case CanvasJoin::Add:
			return SelectMode::Add;
		case CanvasJoin::Toggle:
			return SelectMode::Toggle;
		case CanvasJoin::Replace:
			break;
	}
	return SelectMode::Replace;
}

ImGuiMouseCursor imgui_cursor(CanvasCursor cursor) {
	switch (cursor) {
		case CanvasCursor::ResizeEW:
			return ImGuiMouseCursor_ResizeEW;
		case CanvasCursor::ResizeNS:
			return ImGuiMouseCursor_ResizeNS;
		case CanvasCursor::ResizeNWSE:
			return ImGuiMouseCursor_ResizeNWSE;
		case CanvasCursor::ResizeNESW:
			return ImGuiMouseCursor_ResizeNESW;
		case CanvasCursor::Move:
		case CanvasCursor::Default:
			break;
	}
	return ImGuiMouseCursor_ResizeAll;
}

} // namespace

void CanvasWindowRequests::select(
		const std::string &path, const NodeAddress &record, CanvasJoin join) {
	workspace_.request(request::select_record(path, record, select_mode(join)));
}

void CanvasWindowRequests::edits(const std::string &path, std::vector<Edit> batch) {
	workspace_.request(request::edit_record(path, std::move(batch)));
}

void CanvasWindowRequests::end_edit(const std::string &path) {
	workspace_.request(request::end_edit(path));
}

ViewportCanvas::ViewportCanvas(int design_width, int design_height) :
		design_width_(design_width), design_height_(design_height) {
	zoom_ = design_width > 0 && design_height > 0 ? Zoom::Fit : Zoom::Fill;
}

void ViewportCanvas::set_zoom(Zoom zoom, float scale) {
	if (zoom_ == Zoom::Fill || zoom == Zoom::Fill)
		return; // a picture that fills the canvas has no zoom
	zoom_ = zoom;
	if (zoom == Zoom::Scale)
		scale_ = scale;
}

bool ViewportCanvas::begin(float height, int device_width, int device_height) {
	drawn_ = true;
	right_clicked_ = false;
	input_ = CanvasInput();
	const ImGuiIO &io = ImGui::GetIO();
	// The keys the kinds act on, while the canvas's window (the pane's, its canvas among its
	// children) has the keyboard and no text field takes it.
	CanvasKeyboard &keyboard = input_.keyboard;
	keyboard.focused =
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
	if (keyboard.focused) {
		keyboard.arrow_x = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) -
				(ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
		keyboard.arrow_y = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) -
				(ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
		keyboard.arrow_held = ImGui::IsKeyDown(ImGuiKey_LeftArrow) ||
				ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
				ImGui::IsKeyDown(ImGuiKey_DownArrow);
		keyboard.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		keyboard.frame = ImGui::IsKeyPressed(ImGuiKey_F, false);
	}
	const bool design = zoom_ != Zoom::Fill;
	const ImVec2 region(ImGui::GetContentRegionAvail().x, height);
	// The picture's device size, and where it sits on the canvas.
	int width = std::max(64, int(region.x)), tall = std::max(48, int(height));
	ImVec2 pad(0.0f, 0.0f);
	if (design) {
		pad = ImVec2(kMargin, kMargin);
		switch (zoom_) {
			case Zoom::Fit: {
				// The design's aspect, as the game letterboxes it: whole steps of it (4 by 3 for
				// the menu's 800 x 600), so the picture keeps the aspect exactly whatever room it
				// has.
				const int common = std::gcd(design_width_, design_height_);
				const int across = design_width_ / common, down = design_height_ / common;
				const int step = std::max(16,
						std::min(int(region.x - 2.0f * kMargin) / across,
								int(region.y - 2.0f * kMargin) / down));
				width = step * across;
				tall = step * down;
				pad.x = std::max(kMargin, std::floor((region.x - float(width)) * 0.5f));
				break;
			}
			case Zoom::Scale:
				width = int(std::lround(float(design_width_) * scale_));
				tall = int(std::lround(float(design_height_) * scale_));
				break;
			case Zoom::Device:
				width = device_width;
				tall = device_height;
				break;
			case Zoom::Fill:
				break;
		}
	}
	const ImVec2 content(float(width) + 2.0f * pad.x, float(tall) + 2.0f * pad.y);
	if (design) {
		if (scroll_pending_) {
			ImGui::SetNextWindowScroll(ImVec2(scroll_x_, scroll_y_));
			scroll_pending_ = false;
		}
		ImGui::SetNextWindowContentSize(content);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool visible = ImGui::BeginChild(
				"##canvas", region, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::PopStyleVar();
		child_ = true;
		if (!visible)
			return false;
	}
	// Every press on the canvas, the picture and its margin, is the canvas's: the surface is the
	// first item, so the device's own item under it never takes the mouse.
	const ImVec2 base = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##surface", content,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const bool activated = ImGui::IsItemActivated();
	surface_min_ = CanvasPoint{ base.x, base.y };
	surface_max_ = CanvasPoint{ base.x + content.x, base.y + content.y };
	origin_ = CanvasPoint{ base.x + pad.x, base.y + pad.y };
	const ImVec2 mouse = io.MousePos;
	input_.width = width;
	input_.height = tall;
	input_.mouse = CanvasPoint{ mouse.x - origin_.x, mouse.y - origin_.y };
	input_.screen = CanvasPoint{ mouse.x, mouse.y };
	input_.delta = CanvasPoint{ io.MouseDelta.x, io.MouseDelta.y };
	input_.hovered = hovered;
	// A left press on a design picture lasts until the button comes up (the canvas may lose the
	// active item before: a popup, the focus taken); a picture's camera drag while the surface
	// holds it.
	input_.down = design ? ImGui::IsMouseDown(ImGuiMouseButton_Left) : active;
	input_.double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
	input_.keys = CanvasKeys{ io.KeyShift, io.KeyCtrl, io.KeyAlt };
	if (design) {
		// Ctrl+wheel zooms about the mouse: the design point under it stays under it.
		if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f) {
			const float sx = float(width) / float(design_width_);
			const float sy = float(tall) / float(design_height_);
			const float px = input_.mouse.x / sx, py = input_.mouse.y / sy;
			scale_ = next_zoom(
					zoom_ == Zoom::Scale ? scale_ : (sx + sy) * 0.5f, io.MouseWheel > 0.0f);
			zoom_ = Zoom::Scale;
			const ImVec2 at = ImGui::GetWindowPos();
			scroll_x_ = std::max(0.0f, at.x + kMargin + px * scale_ - mouse.x);
			scroll_y_ = std::max(0.0f, at.y + kMargin + py * scale_ - mouse.y);
			scroll_pending_ = true;
		}
		// The middle button, or Space with the left, pans; the left alone is the kind's.
		const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
		if (activated && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || space))
			panning_ = true;
		if (panning_) {
			if (!active) {
				panning_ = false;
			} else {
				ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
				ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			}
		} else if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			input_.pressed = true;
		}
		input_.panning = panning_;
	} else {
		// The kind's camera zooms and pans: every press and the wheel are its.
		input_.pressed = activated;
		input_.middle = activated && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
		input_.wheel = hovered ? io.MouseWheel : 0.0f;
	}
	right_clicked_ = hovered && !panning_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
	return true;
}

void ViewportCanvas::picture(const Device &device, const Tip &tip) {
	// The kind's hover tip, on the surface (the item just drawn): made only while it shows.
	if (tip)
		ui_kit::tooltip_lazy(tip);
	ImGui::SetCursorScreenPos(ImVec2(origin_.x, origin_.y));
	device(input_.width, input_.height);
	// The picture's edge: a design picture's just outside it, on its margin.
	const float edge = zoom_ != Zoom::Fill ? 1.0f : 0.0f;
	ImGui::GetWindowDrawList()->AddRect(ImVec2(origin_.x - edge, origin_.y - edge),
			ImVec2(origin_.x + float(input_.width) + edge, origin_.y + float(input_.height) + edge),
			kFrameColor);
}

void ViewportCanvas::draw(const OverlayList &shapes, CanvasCursor cursor) {
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	paint.PushClipRect(
			ImVec2(surface_min_.x, surface_min_.y), ImVec2(surface_max_.x, surface_max_.y), true);
	for (const OverlayShape &shape : shapes.shapes)
		draw_shape(paint, origin_, shape);
	paint.PopClipRect();
	if (cursor != CanvasCursor::Default)
		ImGui::SetMouseCursor(imgui_cursor(cursor));
}

void ViewportCanvas::end() {
	if (child_)
		ImGui::EndChild();
	child_ = false;
}

void ViewportCanvas::end_frame() {
	if (!drawn_)
		panning_ = false;
	drawn_ = false;
}

} // namespace opennova::editor
