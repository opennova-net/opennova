#include <editor/preview/hud_canvas.h>

#include <cmath>

#include <runtime/hud/hud_elements.h>
#include <runtime/hud/hud_math.h>

#include <editor/preview/hud_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// The design space hudpos.def's places are in (hud_math.h).
constexpr float kDesignWidth = static_cast<float>(hud::kDesignWidth);
constexpr float kDesignHeight = static_cast<float>(hud::kDesignHeight);

// A box of the screen as the picture shows it.
void picture_box(const HudPreviewElement &element, int width, int height, int screen_width, int screen_height,
		CanvasPoint &min, CanvasPoint &max) {
	const float sx = screen_width > 0 ? float(width) / float(screen_width) : 1.0f;
	const float sy = screen_height > 0 ? float(height) / float(screen_height) : 1.0f;
	min = CanvasPoint{ element.x0 * sx, element.y0 * sy };
	max = CanvasPoint{ element.x1 * sx, element.y1 * sy };
}

const HudPreviewElement *under(const HudViewport &viewport, const CanvasInput &in) {
	if (!in.hovered) return nullptr;
	float x = 0.0f, y = 0.0f;
	HudCanvas::screen_point(in.mouse.x, in.mouse.y, in.width, in.height, viewport.options().width,
			viewport.options().height, x, y);
	return viewport.element_at(x, y);
}

CanvasPoint corner_point(HudHandle handle, CanvasPoint min, CanvasPoint max) {
	const bool left = handle == HudHandle::TopLeft || handle == HudHandle::BottomLeft;
	const bool top = handle == HudHandle::TopLeft || handle == HudHandle::TopRight;
	return CanvasPoint{ left ? min.x : max.x, top ? min.y : max.y };
}

// The picked element's corner under the pointer, where it is resizable.
bool picked_corner(const HudViewport &viewport, const CanvasInput &in, HudHandle &out) {
	const HudPreviewElement *picked = viewport.picked();
	if (!picked || !picked->resizable || !in.hovered) return false;
	CanvasPoint min, max;
	picture_box(*picked, in.width, in.height, viewport.options().width, viewport.options().height, min, max);
	return HudCanvas::corner_at(in.mouse, min, max, out);
}

} // namespace

void HudCanvas::screen_point(float x, float y, int width, int height, int screen_width, int screen_height, float &sx,
		float &sy) {
	sx = width > 0 ? x * float(screen_width) / float(width) : x;
	sy = height > 0 ? y * float(screen_height) / float(height) : y;
}

bool HudCanvas::corner_at(CanvasPoint at, CanvasPoint min, CanvasPoint max, HudHandle &out) {
	const float reach = kHudHandleSize * 0.5f + kHudHandleSlop;
	for (const HudHandle handle : kHudCorners) {
		const CanvasPoint point = corner_point(handle, min, max);
		if (std::fabs(at.x - point.x) <= reach && std::fabs(at.y - point.y) <= reach) {
			out = handle;
			return true;
		}
	}
	return false;
}

void HudCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = static_cast<const HudViewport *>(&viewport);
	CanvasSubject subject;
	subject.path = viewport.path();
	gesture_.frame(subject, out);
}

void HudCanvas::step_(const ViewportContext &context, float dx, float dy, CanvasRequests &out) {
	if (!viewport_ || !press_.element) return;
	std::vector<HudValueChange> changes;
	std::string error;
	// Whole design units: the 1024 x 768 grid the game's places are in, whatever the screen shown.
	if (!hud_drag_changes(press_.start, press_.handle, dx, dy, 1, changes, error) || changes == press_.sent) return;
	if (!context.editable()) return; // no edit while the session takes none
	press_.sent = changes;
	struct Counted final : CanvasRequests {
		CanvasRequests &out;
		bool any = false;
		explicit Counted(CanvasRequests &to) : out(to) {}
		void request(EditorRequest each) override {
			any = true;
			out.request(std::move(each));
		}
	} counted(out);
	if (viewport_->plan_changes(context, changes, gesture_.token(), counted, error) && counted.any) gesture_.sent();
}

void HudCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_) return;
	const HudViewport &viewport = *viewport_;
	CanvasSubject subject;
	subject.path = viewport.path();
	if (in.pressed && !in.middle && !in.panning) {
		gesture_.press(subject, in.screen, out);
		press_ = Press();
		// A corner of the picked element resizes it; any other press on an element moves it once it drags.
		HudHandle corner = HudHandle::Move;
		const HudPreviewElement *element = nullptr;
		if (picked_corner(viewport, in, corner)) {
			element = viewport.picked();
			press_.handle = corner;
		} else {
			element = under(viewport, in);
		}
		// Where it begins: the document as it is now.
		HudLayoutModel now;
		const TextDocument *text = nullptr;
		std::string error;
		if (element && viewport.current(context.input) && viewport.read_now(context, now, text, error))
			press_.element = hud_drag_start(element->element, now.file.hud, press_.start);
		// The design units a picture pixel is now: a zoom while the button is down moves nothing.
		press_.units_x = in.width > 0 ? kDesignWidth / float(in.width) : 1.0f;
		press_.units_y = in.height > 0 ? kDesignHeight / float(in.height) : 1.0f;
	}
	if (gesture_.pressed() && !in.down) {
		// A click picks the element under it, or nothing; a drag's end is its gesture's.
		if (gesture_.release(out)) {
			const HudPreviewElement *element = under(viewport, in);
			HudViewportOptions options = viewport.options();
			options.picked = element ? opennova::hud::hud_element_token(element->element) : std::string();
			if (options != viewport.options()) out.request(request::set_viewport(viewport.path(), hud_options_change(options)));
		}
		press_ = Press();
	} else if (gesture_.pressed()) {
		const bool began = gesture_.move(in.screen);
		if (began && press_.element) {
			// The element a drag takes is the picked one from then on.
			HudViewportOptions options = viewport.options();
			options.picked = opennova::hud::hud_element_token(press_.start.element);
			if (options != viewport.options()) out.request(request::set_viewport(viewport.path(), hud_options_change(options)));
		}
		if (gesture_.dragging()) {
			const CanvasPoint travel = gesture_.travel(in.screen);
			step_(context, travel.x * press_.units_x, travel.y * press_.units_y, out);
		}
	}
	// The arrows nudge the picked element a design unit (Shift: 8), one undo step while they are held.
	const CanvasKeyboard &keys = in.keyboard;
	if (keys.focused && (keys.arrow_x || keys.arrow_y) && !gesture_.pressed()) {
		const HudPreviewElement *picked = viewport.picked();
		HudLayoutModel now;
		const TextDocument *text = nullptr;
		std::string error;
		if (!gesture_.nudging() && picked && viewport.current(context.input) && viewport.read_now(context, now, text, error)) {
			press_ = Press();
			press_.element = hud_drag_start(picked->element, now.file.hud, press_.start);
			if (press_.element) {
				gesture_.nudge(subject, out);
				nudge_x_ = nudge_y_ = 0;
			}
		}
		if (gesture_.nudging()) {
			const int step = in.keys.shift ? 8 : 1;
			nudge_x_ += keys.arrow_x * step;
			nudge_y_ += keys.arrow_y * step;
			step_(context, float(nudge_x_), float(nudge_y_), out);
		}
	} else if (gesture_.nudging() && !keys.arrow_held) {
		gesture_.end(out);
		press_ = Press();
	}
	// Esc lets go of the element picked.
	if (keys.focused && keys.escape && !viewport.options().picked.empty() && !gesture_.pressed()) {
		HudViewportOptions options = viewport.options();
		options.picked.clear();
		out.request(request::set_viewport(viewport.path(), hud_options_change(options)));
	}
}

void HudCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	press_ = Press();
}

void HudCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList HudCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	OverlayList list;
	if (!viewport_) return list;
	const HudViewport &viewport = *viewport_;
	const int screen_width = viewport.options().width, screen_height = viewport.options().height;
	if (const HudPreviewElement *hovered = under(viewport, in); hovered && !gesture_.dragging()) {
		CanvasPoint min, max;
		picture_box(*hovered, in.width, in.height, screen_width, screen_height, min, max);
		list.rect(min, max, OverlayRole::Hover, 1.0f);
	}
	if (const HudPreviewElement *picked = viewport.picked()) {
		CanvasPoint min, max;
		picture_box(*picked, in.width, in.height, screen_width, screen_height, min, max);
		list.rect(min, max, OverlayRole::Selected, 2.0f);
		// A handle at each corner where the game reads a size for it.
		if (picked->resizable)
			for (const HudHandle handle : kHudCorners)
				list.marker(corner_point(handle, min, max), OverlayGlyph::Square, kHudHandleSize * 0.5f, OverlayRole::Normal);
	}
	return list;
}

CanvasCursor HudCanvas::cursor(const ViewportContext &, const CanvasInput &in) const {
	if (!viewport_ || in.panning) return CanvasCursor::Default;
	HudHandle handle = HudHandle::Move;
	if (gesture_.pressed() && press_.element) handle = press_.handle;
	else if (!gesture_.pressed() && !picked_corner(*viewport_, in, handle)) {
		const HudPreviewElement *element = under(*viewport_, in);
		return element && element->movable ? CanvasCursor::Move : CanvasCursor::Default;
	}
	switch (handle) {
	case HudHandle::TopLeft:
	case HudHandle::BottomRight: return CanvasCursor::ResizeNWSE;
	case HudHandle::TopRight:
	case HudHandle::BottomLeft: return CanvasCursor::ResizeNESW;
	case HudHandle::Move: break;
	}
	return CanvasCursor::Move;
}

std::string HudCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging()) return std::string();
	const HudPreviewElement *element = under(*viewport_, in);
	if (!element) return std::string();
	std::string tip = viewport_->element_words(*element) + "\nClick to pick it; double click to go to its line.";
	if (element->movable) tip += " Drag to move it" + std::string(element->resizable ? ", a corner of it picked to size it." : ".");
	return tip;
}

} // namespace opennova::editor
