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
constexpr ImU32 kBadgeFill = IM_COL32(20, 20, 20, 200); // a badge's ground over the picture
constexpr ImU32 kBadgeText = IM_COL32(235, 235, 235, 255);
// The edge each shape gets over a background of the preview preference's (PreviewBackdrop::halo): a line's and a
// ring's this much wider, a word's a pixel each way, dark enough that a pale shape reads on the light grey.
constexpr ImU32 kHaloColor = IM_COL32(12, 12, 12, 190);
constexpr float kHaloWidth = 2.0f;
constexpr float kBadgeInset = 6.0f; // from the picture's corner
constexpr float kBadgePad = 3.0f; // around its text

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

// A shape's dark edge, drawn under it (PreviewBackdrop::halo): its outline's lines a little wider, a filled
// shape's outline around it, a word's letters a pixel each way. A Marquee's fill and a badge's plate need none.
void draw_halo(ImDrawList &paint, CanvasPoint origin, const OverlayShape &shape) {
	const auto at = [&](int i) {
		return ImVec2(origin.x + shape.points[i].x, origin.y + shape.points[i].y);
	};
	const float wide = shape.thickness + kHaloWidth;
	const float s = shape.size;
	switch (shape.kind) {
		case OverlayKind::Rect:
			if (!shape.filled) paint.AddRect(at(0), at(1), kHaloColor, 0.0f, 0, wide);
			break;
		case OverlayKind::Line:
			paint.AddLine(at(0), at(1), kHaloColor, wide);
			break;
		case OverlayKind::Circle:
			if (shape.filled) paint.AddCircleFilled(at(0), s + kHaloWidth * 0.5f, kHaloColor);
			else paint.AddCircle(at(0), s, kHaloColor, 0, wide);
			break;
		case OverlayKind::Quad:
			if (!shape.filled) paint.AddQuad(at(0), at(1), at(2), at(3), kHaloColor, wide);
			break;
		case OverlayKind::Marker: {
			const ImVec2 p = at(0);
			if (shape.glyph == OverlayGlyph::Cross) {
				paint.AddLine(ImVec2(p.x - s, p.y), ImVec2(p.x + s, p.y), kHaloColor, wide);
				paint.AddLine(ImVec2(p.x, p.y - s), ImVec2(p.x, p.y + s), kHaloColor, wide);
			}
			// A square and a dot have their dark edge already; a corner is filled.
			break;
		}
		case OverlayKind::Text: {
			if (shape.filled) break; // on its plate
			const ImVec2 p = at(0);
			const char *begin = shape.text.c_str();
			const char *end = begin + shape.text.size();
			for (const ImVec2 step : { ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1) })
				paint.AddText(ImVec2(p.x + step.x, p.y + step.y), kHaloColor, begin, end);
			break;
		}
	}
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
		case OverlayKind::Text: {
			const ImVec2 p = at(0);
			const char *begin = shape.text.c_str();
			const char *end = begin + shape.text.size();
			if (shape.filled) {
				const ImVec2 size = ImGui::CalcTextSize(begin, end);
				paint.AddRectFilled(ImVec2(p.x - kBadgePad, p.y - kBadgePad),
						ImVec2(p.x + size.x + kBadgePad, p.y + size.y + kBadgePad), kBadgeFill);
			}
			paint.AddText(p, color, begin, end);
			break;
		}
	}
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

void CanvasWindowRequests::request(EditorRequest request) {
	// An edit held back while an operation holds the documents (S13 A3), as the session would refuse
	// it.
	if (request.kind == EditorRequestKind::EditRecord && !workspace_.view().allows(EditorRequestKind::EditRecord))
		return;
	workspace_.request(std::move(request));
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
		// An arrow with Alt held is Back's or Forward's (the navigation history: EditorWindows' shortcuts),
		// never a nudge.
		if (!io.KeyAlt) {
			keyboard.arrow_x = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0) -
					(ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1 : 0);
			keyboard.arrow_y = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) -
					(ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
			keyboard.arrow_held = ImGui::IsKeyDown(ImGuiKey_LeftArrow) ||
					ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
					ImGui::IsKeyDown(ImGuiKey_DownArrow);
		}
		// Esc that closes a popup (a toolbar's dropdown) is the popup's: Dear ImGui closes it as the frame
		// starts, so a popup open last frame takes this frame's Esc (the audit's 3.6: it walked the
		// selection up as well).
		keyboard.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !popup_last_frame_;
		keyboard.frame = !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false); // Ctrl+F finds
		keyboard.duplicate = io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false);
		// The keys a camera flies by, held; no chord's letter is one (Ctrl+S saves, Ctrl+D duplicates).
		if (!io.KeyCtrl) {
			const auto held = [](ImGuiKey positive, ImGuiKey negative) {
				return (ImGui::IsKeyDown(positive) ? 1 : 0) - (ImGui::IsKeyDown(negative) ? 1 : 0);
			};
			keyboard.move_x = held(ImGuiKey_D, ImGuiKey_A);
			keyboard.move_y = held(ImGuiKey_E, ImGuiKey_Q);
			keyboard.move_z = held(ImGuiKey_W, ImGuiKey_S);
		}
		keyboard.fast = io.KeyShift;
		keyboard.remove = ImGui::IsKeyPressed(ImGuiKey_Delete, false);
		keyboard.page = (ImGui::IsKeyPressed(ImGuiKey_PageUp) ? 1 : 0) -
				(ImGui::IsKeyPressed(ImGuiKey_PageDown) ? 1 : 0);
	}
	popup_last_frame_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	input_.dt = io.DeltaTime;
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
	// A picture that fills the canvas takes the right button too (its kind's camera looks with it).
	ImGui::InvisibleButton("##surface", content,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
					(design ? 0 : ImGuiButtonFlags_MouseButtonRight));
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
	const bool left_or_middle = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	input_.down = design ? ImGui::IsMouseDown(ImGuiMouseButton_Left) : active && left_or_middle;
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
		// The kind's camera zooms and pans: every press and the wheel are its. A press is the left
		// button's or the middle one's; the right one's is apart (a look).
		input_.pressed = activated && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
											  ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
		input_.middle = activated && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
		input_.wheel = hovered ? io.MouseWheel : 0.0f;
		input_.right_pressed = activated && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
		if (input_.right_pressed)
			right_held_ = true;
		input_.right_down = right_held_ && ImGui::IsMouseDown(ImGuiMouseButton_Right);
	}
	if (design) {
		right_clicked_ = hovered && !panning_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
	} else if (right_held_ && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
		// A picture that fills the canvas: the right button is a click once it comes up having
		// travelled less than a drag does (else it was a look).
		right_held_ = false;
		right_clicked_ = hovered &&
				io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < kDragThreshold * kDragThreshold;
	}
	return true;
}

void ViewportCanvas::picture(const Device &device, const Tip &tip, bool pointer) {
	// The kind's hover tip, on the surface (the item just drawn): made only while it shows.
	if (tip)
		ui_kit::tooltip_lazy(tip);
	ImGui::SetCursorScreenPos(ImVec2(origin_.x, origin_.y));
	ViewportPicture shown;
	shown.x = origin_.x;
	shown.y = origin_.y;
	shown.width = input_.width;
	shown.height = input_.height;
	shown.clip_left = surface_min_.x;
	shown.clip_top = surface_min_.y;
	shown.clip_right = surface_max_.x;
	shown.clip_bottom = surface_max_.y;
	shown.canvas_sized = zoom_ != Zoom::Device;
	shown.pointer = pointer;
	shown.pointer_x = input_.mouse.x;
	shown.pointer_y = input_.mouse.y;
	shown.hovered = input_.hovered && input_.mouse.x >= 0.0f && input_.mouse.y >= 0.0f &&
			input_.mouse.x < float(input_.width) && input_.mouse.y < float(input_.height);
	device(shown);
	// The picture's edge: a design picture's just outside it, on its margin.
	const float edge = zoom_ != Zoom::Fill ? 1.0f : 0.0f;
	// Over a background of the preview preference's, a dark frame: the Grey's top is the old frame's grey.
	const ImU32 frame = backdrop_.own ? kFrameColor
	                                  : IM_COL32((backdrop_.frame >> 16) & 0xFF, (backdrop_.frame >> 8) & 0xFF,
	                                             backdrop_.frame & 0xFF, 255);
	ImGui::GetWindowDrawList()->AddRect(ImVec2(origin_.x - edge, origin_.y - edge),
			ImVec2(origin_.x + float(input_.width) + edge, origin_.y + float(input_.height) + edge), frame);
}

void ViewportCanvas::badge(const std::string &text) {
	if (text.empty())
		return;
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	const ImVec2 at(origin_.x + kBadgeInset, origin_.y + kBadgeInset);
	const ImVec2 size = ImGui::CalcTextSize(text.c_str());
	paint.PushClipRect(
			ImVec2(surface_min_.x, surface_min_.y), ImVec2(surface_max_.x, surface_max_.y), true);
	paint.AddRectFilled(ImVec2(at.x - kBadgePad, at.y - kBadgePad),
			ImVec2(at.x + size.x + kBadgePad, at.y + size.y + kBadgePad), kBadgeFill);
	paint.AddText(at, kBadgeText, text.c_str());
	paint.PopClipRect();
}

void ViewportCanvas::legend(const std::string &title, const std::vector<LegendRow> &rows) {
	if (title.empty() && rows.empty())
		return;
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	const float line = ImGui::GetTextLineHeight();
	const float swatch = line * 0.8f;
	const float gap = kBadgePad * 2.0f;
	// The title may run to several lines.
	const ImVec2 heading = title.empty() ? ImVec2(0.0f, 0.0f) : ImGui::CalcTextSize(title.c_str());
	float width = heading.x;
	for (const LegendRow &row : rows)
		width = std::max(width, swatch + gap + ImGui::CalcTextSize(row.text.c_str()).x);
	const float height = heading.y + line * float(rows.size());
	const ImVec2 at(origin_.x + kBadgeInset, surface_max_.y - kBadgeInset - height);
	paint.PushClipRect(
			ImVec2(surface_min_.x, surface_min_.y), ImVec2(surface_max_.x, surface_max_.y), true);
	paint.AddRectFilled(ImVec2(at.x - kBadgePad, at.y - kBadgePad),
			ImVec2(at.x + width + kBadgePad, at.y + height + kBadgePad), kBadgeFill);
	float y = at.y;
	if (!title.empty()) {
		paint.AddText(ImVec2(at.x, y), kBadgeText, title.c_str());
		y += heading.y;
	}
	for (const LegendRow &row : rows) {
		const float top = y + (line - swatch) * 0.5f;
		paint.AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + swatch, top + swatch),
				IM_COL32(row.rgb[0], row.rgb[1], row.rgb[2], 255));
		paint.AddText(ImVec2(at.x + swatch + gap, y), kBadgeText, row.text.c_str());
		y += line;
	}
	paint.PopClipRect();
}

void ViewportCanvas::draw(const OverlayList &shapes, CanvasCursor cursor) {
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	paint.PushClipRect(
			ImVec2(surface_min_.x, surface_min_.y), ImVec2(surface_max_.x, surface_max_.y), true);
	// Every edge first, so no shape's edge covers another shape.
	if (backdrop_.halo)
		for (const OverlayShape &shape : shapes.shapes)
			draw_halo(paint, origin_, shape);
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
	if (!drawn_) {
		panning_ = false;
		right_held_ = false;
	}
	drawn_ = false;
}

} // namespace opennova::editor
