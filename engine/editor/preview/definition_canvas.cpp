#include <editor/preview/definition_canvas.h>

#include <cmath>
#include <utility>
#include <vector>

#include <editor/preview/definition_viewport.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

// The eyes' colours: the hip's warm, the aimed one's cool, their path between grey.
constexpr uint32_t kHipRgb = 0xFFB040;
constexpr uint32_t kAimedRgb = 0x40C8FF;
constexpr uint32_t kPathRgb = 0xB0B0B0;
// How far an eye's look line reaches, metres.
constexpr float kLookReach = 0.25f;

CanvasSubject subject_of(const ViewportModel &viewport) {
	CanvasSubject subject;
	subject.path = viewport.path();
	return subject;
}

PreviewVec3 along(const PreviewVec3 &at, const PreviewVec3 &way, float metres) {
	return PreviewVec3{at.x + way.x * metres, at.y + way.y * metres, at.z + way.z * metres};
}

const char *eye_words(const char *handle) {
	return std::string(handle) == "pos" ? "Hip eye (pos)" : "Aimed eye (tpos)";
}

} // namespace

void DefinitionCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = static_cast<const DefinitionViewport *>(&viewport);
	gesture_.frame(subject_of(viewport), out);
}

int DefinitionCanvas::eye_under(const ViewportContext &context, float x, float y) const {
	if (!viewport_) return -1;
	const std::vector<DefinitionViewport::Eye> eyes = viewport_->eyes(context.width, context.height);
	int best = -1;
	float nearest = kDefinitionEyeSlop;
	for (size_t i = 0; i < eyes.size(); ++i) {
		float px = 0.0f, py = 0.0f;
		if (!viewport_->camera().project(eyes[i].at, context.width, context.height, px, py)) continue;
		const float distance = std::hypot(px - x, py - y);
		if (distance <= nearest) {
			nearest = distance;
			best = int(i);
		}
	}
	return best;
}

void DefinitionCanvas::input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_) return;
	const DefinitionViewport &viewport = *viewport_;
	OrbitCamera camera = viewport.camera();
	bool moved = false;
	const std::vector<DefinitionViewport::Eye> eyes = viewport.eyes(in.width, in.height);
	if (in.pressed) {
		gesture_.press(subject_of(viewport), in.screen, out);
		pan_ = in.middle || in.keys.shift;
		grabbed_ = in.middle ? -1 : eye_under(context, in.mouse.x, in.mouse.y);
		if (grabbed_ >= 0) {
			float px = 0.0f, py = 0.0f;
			camera.project(eyes[size_t(grabbed_)].at, in.width, in.height, px, py);
			offset_ = CanvasPoint{px - in.mouse.x, py - in.mouse.y};
			along_ = eyes[size_t(grabbed_)].at;
			along_y_ = in.mouse.y;
		}
	}
	if (gesture_.pressed() && !in.down) {
		gesture_.release(out);
		grabbed_ = -1;
	} else if (gesture_.pressed()) {
		gesture_.move(in.screen);
		if (gesture_.dragging() && grabbed_ >= 0 && size_t(grabbed_) < eyes.size() && context.editable() &&
		    context.input.document) {
			// The eye follows the pointer (kept where the press took it) in the plane through it that faces the
			// camera; Alt slides it along the way it looks (up the picture: forward), as far as a pixel is at it.
			const DefinitionViewport::Eye &eye = eyes[size_t(grabbed_)];
			PreviewVec3 to;
			bool placed = false;
			if (in.keys.alt) {
				float px = 0.0f, py = 0.0f, depth = 0.0f;
				if (camera.project(along_, in.width, in.height, px, py, &depth) && depth > 0.0f) {
					const float metres = (along_y_ - in.mouse.y) * depth / camera.focal(in.width);
					to = along(along_, eye.look, metres);
					placed = true;
				}
			} else {
				placed = camera.on_view_plane(in.mouse.x + offset_.x, in.mouse.y + offset_.y, in.width, in.height,
				                              eye.at, to);
				along_ = to;
				along_y_ = in.mouse.y;
			}
			std::vector<Edit> edits;
			if (placed &&
			    viewport.eye_edits(*context.input.document, eye.handle, to, in.width, in.height, gesture_.token(), edits) &&
			    !edits.empty()) {
				out.request(request::edit_record(gesture_.path(), std::move(edits)));
				gesture_.sent();
			}
		} else if (gesture_.dragging() && grabbed_ < 0 && (in.delta.x != 0.0f || in.delta.y != 0.0f)) {
			if (pan_)
				camera.pan(in.delta.x, in.delta.y, in.width);
			else
				camera.orbit(in.delta.x, in.delta.y);
			moved = true;
		}
	}
	if (in.hovered && in.wheel != 0.0f) {
		camera.dolly(std::pow(kOrbitWheelDolly, in.wheel));
		moved = true;
	}
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame)) {
		camera = viewport.framed(in.width, in.height);
		moved = true;
	}
	// The first-person eye stands where the game's camera does: no gesture moves it.
	if (moved && !camera.posed) out.request(request::set_viewport(viewport.path(), definition_camera_change(camera)));
}

void DefinitionCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
	grabbed_ = -1;
}

void DefinitionCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList DefinitionCanvas::shapes(const ViewportContext &context, const CanvasInput &in) const {
	OverlayList list;
	if (!viewport_) return list;
	const OrbitCamera &camera = viewport_->camera();
	const std::vector<DefinitionViewport::Eye> eyes = viewport_->eyes(in.width, in.height);
	if (eyes.empty()) return list;
	const int under = grabbed_ >= 0 ? grabbed_ : (in.hovered ? eye_under(context, in.mouse.x, in.mouse.y) : -1);
	// The path the scope eases along, hip to aimed.
	CanvasPoint a, b;
	if (eyes.size() == 2 && camera.project(eyes[0].at, in.width, in.height, a.x, a.y) &&
	    camera.project(eyes[1].at, in.width, in.height, b.x, b.y))
		list.line(a, b, kPathRgb, 1.0f, OverlayRole::Normal, 180);
	for (size_t i = 0; i < eyes.size(); ++i) {
		const DefinitionViewport::Eye &eye = eyes[i];
		const uint32_t rgb = std::string(eye.handle) == "pos" ? kHipRgb : kAimedRgb;
		const OverlayRole role = int(i) == under ? (grabbed_ >= 0 ? OverlayRole::Selected : OverlayRole::Hover)
		                                         : OverlayRole::Normal;
		CanvasPoint at, tip, top;
		if (!camera.project(eye.at, in.width, in.height, at.x, at.y)) continue;
		// The way it looks, and a short tick of its up.
		if (camera.project(along(eye.at, eye.look, kLookReach), in.width, in.height, tip.x, tip.y))
			list.line(at, tip, rgb, 2.0f, role);
		if (camera.project(along(eye.at, eye.up, kLookReach * 0.25f), in.width, in.height, top.x, top.y))
			list.line(at, top, rgb, 1.0f, role, 200);
		list.ring(at, 6.0f, role, 2.0f, rgb);
		list.marker(at, OverlayGlyph::Dot, 3.0f, role, rgb);
		list.text(CanvasPoint{at.x + 9.0f, at.y - 7.0f}, eye_words(eye.handle), rgb, role);
	}
	return list;
}

CanvasCursor DefinitionCanvas::cursor(const ViewportContext &context, const CanvasInput &in) const {
	if (gesture_.dragging()) return CanvasCursor::Move;
	return in.hovered && eye_under(context, in.mouse.x, in.mouse.y) >= 0 ? CanvasCursor::Move : CanvasCursor::Default;
}

std::string DefinitionCanvas::hover_tip(const ViewportContext &context, const CanvasInput &in) const {
	if (!viewport_ || gesture_.dragging() || !in.hovered) return std::string();
	const int under = eye_under(context, in.mouse.x, in.mouse.y);
	if (under < 0) return std::string();
	const std::vector<DefinitionViewport::Eye> eyes = viewport_->eyes(in.width, in.height);
	const std::string handle = eyes[size_t(under)].handle;
	return handle == "pos"
	               ? "The hip eye: where the game's camera stands before the gun at the hip (weapon.def pos). Drag to "
	                 "move it in the picture's plane; Alt drags it along the way it looks."
	               : "The aimed eye: where the game's camera stands before the gun aimed down its sights (weapon.def "
	                 "tpos, its turn tpos's rotation). Drag to move it; Alt drags it along the way it looks.";
}

} // namespace opennova::editor
