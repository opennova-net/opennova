#include <editor/preview/hud_canvas.h>

#include <runtime/hud/hud_elements.h>

#include <editor/preview/hud_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

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

} // namespace

void HudCanvas::screen_point(float x, float y, int width, int height, int screen_width, int screen_height, float &sx,
		float &sy) {
	sx = width > 0 ? x * float(screen_width) / float(width) : x;
	sy = height > 0 ? y * float(screen_height) / float(height) : y;
}

void HudCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = static_cast<const HudViewport *>(&viewport);
	CanvasSubject subject;
	subject.path = viewport.path();
	gesture_.frame(subject, out);
}

void HudCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_) return;
	const HudViewport &viewport = *viewport_;
	if (in.pressed) {
		CanvasSubject subject;
		subject.path = viewport.path();
		gesture_.press(subject, in.screen, out);
	}
	if (gesture_.pressed() && !in.down) {
		// A click picks the element under it, or nothing.
		if (gesture_.release(out)) {
			const HudPreviewElement *element = under(viewport, in);
			HudViewportOptions options = viewport.options();
			options.picked = element ? opennova::hud::hud_element_token(element->element) : std::string();
			if (options != viewport.options()) out.request(request::set_viewport(viewport.path(), hud_options_change(options)));
		}
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
	}
	// Esc lets go of the element picked.
	if (in.keyboard.focused && in.keyboard.escape && !viewport.options().picked.empty()) {
		HudViewportOptions options = viewport.options();
		options.picked.clear();
		out.request(request::set_viewport(viewport.path(), hud_options_change(options)));
	}
}

void HudCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
}

void HudCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList HudCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	OverlayList list;
	if (!viewport_) return list;
	const HudViewport &viewport = *viewport_;
	const int screen_width = viewport.options().width, screen_height = viewport.options().height;
	if (const HudPreviewElement *picked = viewport.picked()) {
		CanvasPoint min, max;
		picture_box(*picked, in.width, in.height, screen_width, screen_height, min, max);
		list.rect(min, max, OverlayRole::Selected, 2.0f);
	}
	if (const HudPreviewElement *hovered = under(viewport, in)) {
		CanvasPoint min, max;
		picture_box(*hovered, in.width, in.height, screen_width, screen_height, min, max);
		list.rect(min, max, OverlayRole::Hover, 1.0f);
	}
	return list;
}

CanvasCursor HudCanvas::cursor(const ViewportContext &, const CanvasInput &) const {
	return CanvasCursor::Default;
}

std::string HudCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging()) return std::string();
	const HudPreviewElement *element = under(*viewport_, in);
	if (!element) return std::string();
	return viewport_->element_words(*element) + "\nClick to pick it; double click to go to its line.";
}

} // namespace opennova::editor
