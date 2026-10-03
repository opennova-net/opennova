#include <editor/preview/texture_canvas.h>

#include <cmath>

#include <editor/preview/texture_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// Past this many picture pixels a texel, the texel under the pointer is ringed.
constexpr float kRingFromScale = 6.0f;

void set_camera(const TextureViewport &viewport, const TextureCamera &camera, CanvasRequests &out) {
	out.request(request::set_viewport(viewport.path(), texture_camera_change(camera)));
}

} // namespace

void TextureCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = static_cast<const TextureViewport *>(&viewport);
	// What the canvas shows: the file at the viewport's path (a gesture here edits nothing; it ends
	// with another subject as every canvas's does).
	CanvasSubject subject;
	subject.path = viewport.path();
	gesture_.frame(subject, out);
}

void TextureCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_ || !viewport_->image()) return;
	const TextureViewport &viewport = *viewport_;
	const TexturePlacement now = viewport.placement(in.width, in.height);
	// The camera this frame moves to, a pan and the wheel's zoom about the pointer on it: one SetViewport.
	TextureCamera camera;
	camera.fit = false;
	camera.scale = now.scale;
	camera.x = now.x;
	camera.y = now.y;
	bool moved = false;
	if (in.pressed) {
		CanvasSubject subject;
		subject.path = viewport.path();
		gesture_.press(subject, in.screen, out);
	}
	if (gesture_.pressed() && !in.down) {
		gesture_.release(out);
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && (in.delta.x != 0.0f || in.delta.y != 0.0f)) {
			camera.x -= in.delta.x / camera.scale;
			camera.y -= in.delta.y / camera.scale;
			moved = true;
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		// The texel under the pointer stays under it.
		float tx = 0.0f, ty = 0.0f;
		TexturePlacement at{camera.scale, camera.x, camera.y};
		at.texel_of(in.mouse.x, in.mouse.y, in.width, in.height, tx, ty);
		float scale = camera.scale;
		for (int steps = int(std::lround(std::fabs(in.wheel))); steps > 0; --steps)
			scale = texture_zoom_step(scale, in.wheel > 0.0f);
		if (std::lround(std::fabs(in.wheel)) == 0) scale = texture_zoom_step(scale, in.wheel > 0.0f);
		camera.scale = scale;
		camera.x = tx - (in.mouse.x - float(in.width) * 0.5f) / scale;
		camera.y = ty - (in.mouse.y - float(in.height) * 0.5f) / scale;
		moved = true;
	}
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame)) {
		camera = viewport.camera();
		camera.fit = true;
		moved = true;
	}
	if (moved && !(camera == viewport.camera())) set_camera(viewport, camera, out);
}

void TextureCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
}

void TextureCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList TextureCanvas::shapes(const ViewportContext &, const CanvasInput &in) const {
	OverlayList list;
	if (!viewport_ || !viewport_->image() || !in.hovered || gesture_.dragging()) return list;
	const TextureViewport &viewport = *viewport_;
	const TexturePlacement at = viewport.placement(in.width, in.height);
	const size_t level = viewport.shown_level();
	const TextureImage &image = *viewport.image();
	if (level >= image.levels.size()) return list;
	// A texel of the level shown spans this many first-level texels.
	const float span_x = float(image.width()) / float(image.levels[level].width);
	const float span_y = float(image.height()) / float(image.levels[level].height);
	if (at.scale * std::min(span_x, span_y) < kRingFromScale) return list;
	uint32_t tx = 0, ty = 0;
	uint8_t rgba[4] = {};
	if (!viewport.texel_at(in.mouse.x, in.mouse.y, in.width, in.height, tx, ty, rgba)) return list;
	CanvasPoint min, max;
	at.pixel_of(float(tx) * span_x, float(ty) * span_y, in.width, in.height, min.x, min.y);
	at.pixel_of(float(tx + 1) * span_x, float(ty + 1) * span_y, in.width, in.height, max.x, max.y);
	list.rect(min, max, OverlayRole::Hover, 1.0f);
	return list;
}

CanvasCursor TextureCanvas::cursor(const ViewportContext &, const CanvasInput &) const {
	return gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
}

std::string TextureCanvas::hover_tip(const ViewportContext &, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging() || !in.hovered) return std::string();
	uint32_t tx = 0, ty = 0;
	uint8_t rgba[4] = {};
	if (!viewport_->texel_at(in.mouse.x, in.mouse.y, in.width, in.height, tx, ty, rgba)) return std::string();
	return viewport_->texel_words(tx, ty, rgba);
}

} // namespace opennova::editor
