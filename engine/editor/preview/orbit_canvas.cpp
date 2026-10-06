#include <editor/preview/orbit_canvas.h>

#include <algorithm>
#include <cmath>

#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>

namespace opennova::editor {

namespace {

CanvasSubject subject_of(const ViewportModel &viewport) {
	CanvasSubject subject;
	subject.path = viewport.path();
	return subject;
}

io::JsonValue vec3(const PreviewVec3 &v) {
	io::JsonValue out = io::JsonValue::make_array();
	out.array.push_back(io::json_number(v.x));
	out.array.push_back(io::json_number(v.y));
	out.array.push_back(io::json_number(v.z));
	return out;
}

} // namespace

io::JsonValue orbit_camera_to_json(const OrbitCamera &camera) {
	io::JsonValue out = io::JsonValue::make_object();
	out.set("target", vec3(camera.target));
	out.set("yaw", io::json_number(camera.yaw));
	out.set("pitch", io::json_number(camera.pitch));
	out.set("distance", io::json_number(camera.distance));
	return out;
}

bool read_orbit_camera(const io::JsonValue &json, OrbitCamera &held, bool &frame, std::string &error) {
	if (!json.is_object()) {
		error = "\"camera\" is an object.";
		return false;
	}
	OrbitCamera camera = held;
	frame = false;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const io::JsonValue &value = member.value;
		if (key == "yaw" || key == "pitch" || key == "distance") {
			float f = 0.0f;
			if (!io::json_float(value, f) || !std::isfinite(f)) {
				error = "camera." + key + " is a number.";
				return false;
			}
			if (key == "distance" && !(f > 0.0f)) {
				error = "camera.distance is more than 0.";
				return false;
			}
			(key == "yaw" ? camera.yaw : key == "pitch" ? camera.pitch : camera.distance) = f;
		} else if (key == "target") {
			bool numbers = value.is_array() && value.array.size() == 3;
			float at[3] = {};
			for (size_t i = 0; numbers && i < 3; ++i) numbers = io::json_float(value.array[i], at[i]);
			if (!numbers) {
				error = "camera.target is [x, y, z].";
				return false;
			}
			camera.target = PreviewVec3{at[0], at[1], at[2]};
		} else if (key == "frame") {
			if (!value.is_bool()) {
				error = "camera.frame is true or false.";
				return false;
			}
			frame = value.boolean;
		} else {
			error = "Unknown camera member \"" + key + "\" (it takes yaw, pitch, distance, target, frame).";
			return false;
		}
	}
	camera.pitch = std::clamp(camera.pitch, -kOrbitPitchLimit, kOrbitPitchLimit);
	held = camera;
	return true;
}

void OrbitCanvas::follow(const ViewportModel &viewport, const ViewportContext &, CanvasRequests &out) {
	viewport_ = &viewport;
	// A gesture here edits nothing; it ends with another subject as every canvas's does.
	gesture_.frame(subject_of(viewport), out);
}

void OrbitCanvas::input(const ViewportContext &, const CanvasInput &in, CanvasRequests &out) {
	if (!viewport_ || !hooks_.camera || !hooks_.framed || !hooks_.change) return;
	const ViewportModel &viewport = *viewport_;
	// The camera this frame moves to, an orbit or a pan then the wheel's dolly on it: one SetViewport.
	OrbitCamera camera = hooks_.camera(viewport);
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
		camera.dolly(std::pow(kOrbitWheelDolly, in.wheel));
		moved = true;
	}
	if (in.double_clicked || (in.keyboard.focused && in.keyboard.frame)) {
		camera = hooks_.framed(viewport, in.width, in.height);
		moved = true;
	}
	if (moved) out.request(request::set_viewport(viewport.path(), hooks_.change(camera)));
}

void OrbitCanvas::end(CanvasRequests &out) {
	gesture_.end(out);
}

void OrbitCanvas::end_frame(CanvasRequests &out) {
	gesture_.end_frame(out);
}

OverlayList OrbitCanvas::shapes(const ViewportContext &, const CanvasInput &) const {
	return OverlayList();
}

CanvasCursor OrbitCanvas::cursor(const ViewportContext &, const CanvasInput &) const {
	return gesture_.dragging() ? CanvasCursor::Move : CanvasCursor::Default;
}

std::string OrbitCanvas::hover_tip(const ViewportContext &, const CanvasInput &) const {
	// A point of the picture names nothing (the toolbar says how the camera moves).
	return std::string();
}

} // namespace opennova::editor
