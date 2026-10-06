#include <editor/preview/texture_canvas.h>

#include <cmath>

#include <editor/preview/texture_viewport.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// Past this many picture pixels a texel, the texel under the pointer is ringed.
constexpr float kRingFromScale = 6.0f;
// A cell edge's colour: a mid yellow that reads over dark and light texels alike.
constexpr uint32_t kCellRgb = 0xE0C040;
// A compare's split, a cyan line; the worst block, a red outline.
constexpr uint32_t kSplitRgb = 0x40D0E0;
constexpr uint32_t kWorstRgb = 0xFF4040;

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
	if (!viewport_ || !viewport_->image()) return list;
	const TextureViewport &viewport = *viewport_;
	const TexturePlacement at = viewport.placement(in.width, in.height);
	// The cells the game cuts the texture in, for the use shown (a tile atlas's 64 texels), each edge a
	// line over the picture while a cell is at least a few pixels wide.
	const uint32_t cells = viewport.shown_use().cells;
	if (cells > 0 && at.scale * float(cells) >= 4.0f) {
		const float w = float(viewport.image()->width()), h = float(viewport.image()->height());
		for (uint32_t x = cells; float(x) < w; x += cells) {
			CanvasPoint from, to;
			at.pixel_of(float(x), 0.0f, in.width, in.height, from.x, from.y);
			at.pixel_of(float(x), h, in.width, in.height, to.x, to.y);
			list.line(from, to, kCellRgb, 1.0f);
		}
		for (uint32_t y = cells; float(y) < h; y += cells) {
			CanvasPoint from, to;
			at.pixel_of(0.0f, float(y), in.width, in.height, from.x, from.y);
			at.pixel_of(w, float(y), in.width, in.height, to.x, to.y);
			list.line(from, to, kCellRgb, 1.0f);
		}
	}
	// A compare's split, the reference left of it, and the worst block of the level shown, outlined.
	const TextureViewportOptions &options = viewport.options();
	const float tw = float(viewport.image()->width()), th = float(viewport.image()->height());
	if (options.compare == TextureCompareView::Split && viewport.compression() && viewport.compression()->made) {
		CanvasPoint from, to;
		at.pixel_of(options.split * tw, 0.0f, in.width, in.height, from.x, from.y);
		at.pixel_of(options.split * tw, th, in.width, in.height, to.x, to.y);
		list.line(from, to, kSplitRgb, 2.0f);
	}
	if (const TextureLevelError *error = viewport.shown_error(); error && error->worst_rms > 0.0 && error->width > 0) {
		const float span_x = tw / float(error->width), span_y = th / float(error->height);
		CanvasPoint corner[4];
		const float xs[2] = {float(error->worst_x) * span_x, float(std::min(error->worst_x + 4, error->width)) * span_x};
		const float ys[2] = {float(error->worst_y) * span_y, float(std::min(error->worst_y + 4, error->height)) * span_y};
		at.pixel_of(xs[0], ys[0], in.width, in.height, corner[0].x, corner[0].y);
		at.pixel_of(xs[1], ys[0], in.width, in.height, corner[1].x, corner[1].y);
		at.pixel_of(xs[1], ys[1], in.width, in.height, corner[2].x, corner[2].y);
		at.pixel_of(xs[0], ys[1], in.width, in.height, corner[3].x, corner[3].y);
		for (int i = 0; i < 4; ++i) list.line(corner[i], corner[(i + 1) % 4], kWorstRgb, 2.0f);
	}
	if (!in.hovered || gesture_.dragging()) return list;
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
