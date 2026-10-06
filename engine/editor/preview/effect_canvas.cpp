#include <editor/preview/effect_canvas.h>

#include <cmath>

#include <editor/preview/effect_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

CanvasSubject subject_of(const ViewportModel &viewport) {
	CanvasSubject subject;
	subject.path = viewport.path();
	return subject;
}

} // namespace

void EffectCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = static_cast<const EffectViewport *>(&viewport);
	// A gesture here edits nothing; it ends with another subject as every canvas's does.
	gesture_.frame(subject_of(viewport), out);
}

void EffectCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_) return;
	const EffectViewport &viewport = *viewport_;
	// The camera this frame moves to, an orbit or a pan then the wheel's dolly on it: one SetViewport.
	OrbitCamera camera = viewport.camera();
	bool moved = false;
	if (in.pressed) {
		gesture_.press(subject_of(viewport), in.screen, out);
		pan_ = in.middle || in.keys.shift;
	}
	if (gesture_.pressed() && !in.down) {
		gesture_.release(out);
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && (in.delta.x != 0.0f || in.delta.y != 0.0f)) {
			if (pan_)
				camera.pan(in.delta.x, in.delta.y, in.width);
			else
				camera.orbit(in.delta.x, in.delta.y);
			moved = true;
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		camera.dolly(std::pow(kEffectWheelDolly, in.wheel));
		moved = true;
	}
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame)) {
		camera = viewport.framed(in.width, in.height);
		moved = true;
	}
	if (moved) out.request(request::set_viewport(viewport.path(), effect_camera_change(camera)));
}

void EffectCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
}

void EffectCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList EffectCanvas::shapes(const ViewportContext &, const CanvasInput &) const {
	return OverlayList();
}

CanvasCursor EffectCanvas::cursor(const ViewportContext &, const CanvasInput &) const {
	return gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
}

std::string EffectCanvas::hover_tip(const ViewportContext &, const CanvasInput &) const {
	// A point of the picture names nothing (the toolbar says how the camera moves).
	return std::string();
}

} // namespace opennova::editor
