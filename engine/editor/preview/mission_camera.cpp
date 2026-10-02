#include <editor/preview/mission_camera.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <runtime/world/presentation_frame.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;

constexpr double kDegreesPerRadian = 1.0 / io::kRadiansPerDegree;
constexpr float kTwoPi = 6.28318530717958647692f;

JsonValue point_json(const double point[3]) {
	JsonValue out = JsonValue::make_array();
	for (int i = 0; i < 3; ++i) out.push(json_number(point[i]));
	return out;
}

} // namespace

PreviewVec3 mission_to_preview(const double mission[3]) {
	const float m[3] = { float(mission[0]), float(mission[1]), float(mission[2]) };
	float p[3];
	world::presentation_from_mission(m, p);
	return PreviewVec3{ p[0], p[1], p[2] };
}

void preview_to_mission(const PreviewVec3 &point, double out[3]) {
	const float p[3] = { point.x, point.y, point.z };
	float m[3];
	world::mission_from_presentation(p, m);
	for (int i = 0; i < 3; ++i) out[i] = double(m[i]);
}

void mission_camera_look(OrbitCamera &camera, float dx, float dy) {
	const PreviewVec3 eye = camera.eye();
	// Yaw 0 looks along -z and a positive yaw turns the view left (its heading is the yaw negated):
	// a travel to the right turns it right.
	camera.yaw = std::fmod(camera.yaw - dx * kMissionLookRadiansPerPixel, kTwoPi);
	camera.pitch = std::clamp(camera.pitch + dy * kMissionLookRadiansPerPixel, -kOrbitPitchLimit, kOrbitPitchLimit);
	// The eye stays: the target is where the camera now looks, as far ahead as it was.
	PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	camera.target = PreviewVec3{ eye.x - back.x * camera.distance, eye.y - back.y * camera.distance,
		eye.z - back.z * camera.distance };
}

void mission_camera_fly(OrbitCamera &camera, float right, float up, float forward) {
	PreviewVec3 r, u, back;
	camera.axes(r, u, back);
	camera.target.x += r.x * right - back.x * forward;
	camera.target.y += r.y * right + up - back.y * forward;
	camera.target.z += r.z * right - back.z * forward;
}

bool mission_camera_on_height(const OrbitCamera &camera, float x, float y, int width, int height, double z,
		double out[3]) {
	PreviewVec3 from, along;
	if (!camera.ray(x, y, width, height, from, along)) return false;
	// The presentation frame's y is the mission's z.
	const double fall = double(along.y);
	if (std::fabs(fall) < 1e-9) return false;
	const double t = (z - double(from.y)) / fall;
	if (!(t > 0.0)) return false;
	const PreviewVec3 hit{ float(double(from.x) + double(along.x) * t), float(z),
		float(double(from.z) + double(along.z) * t) };
	preview_to_mission(hit, out);
	out[2] = z;
	return true;
}

double mission_camera_heading(const OrbitCamera &camera) {
	double heading = std::fmod(-double(camera.yaw) * kDegreesPerRadian, 360.0);
	if (heading < 0.0) heading += 360.0;
	return heading;
}

double mission_camera_pitch(const OrbitCamera &camera) {
	return double(camera.pitch) * kDegreesPerRadian;
}

void mission_camera_top(OrbitCamera &camera) {
	camera.yaw = 0.0f;
	camera.pitch = kOrbitPitchLimit;
}

io::JsonValue mission_camera_to_json(const OrbitCamera &camera) {
	JsonValue out = JsonValue::make_object();
	double target[3], eye[3];
	preview_to_mission(camera.target, target);
	preview_to_mission(camera.eye(), eye);
	out.set("target", point_json(target));
	out.set("yaw", json_number(mission_camera_heading(camera)));
	out.set("pitch", json_number(mission_camera_pitch(camera)));
	out.set("distance", json_number(double(camera.distance)));
	out.set("eye", point_json(eye));
	out.set("fov", json_number(double(OrbitCamera::fov_horizontal_degrees())));
	return out;
}

bool mission_camera_from_json(const io::JsonValue &json, OrbitCamera &camera, std::string &error) {
	if (!json.is_object()) {
		error = "\"camera\" is an object, {target, yaw, pitch, distance}.";
		return false;
	}
	OrbitCamera held = camera;
	const double limit = double(kOrbitPitchLimit) * kDegreesPerRadian;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "target") {
			double target[3] = { 0.0, 0.0, 0.0 };
			bool read = value.is_array() && value.array.size() == 3;
			for (int i = 0; read && i < 3; ++i) {
				float checked = 0.0f;
				read = io::json_float(value.array[size_t(i)], checked);
				target[i] = double(checked);
			}
			if (!read) {
				error = "camera.target is [x, y, z], a point of the mission.";
				return false;
			}
			held.target = mission_to_preview(target);
		} else if (key == "yaw" || key == "pitch" || key == "distance") {
			float number = 0.0f;
			if (!io::json_float(value, number)) {
				error = "camera." + key + " is a number.";
				return false;
			}
			if (key == "yaw") {
				// The heading it looks along, any number of degrees: the orbit's yaw is its negation.
				held.yaw = float(-std::fmod(double(number), 360.0) * io::kRadiansPerDegree);
			} else if (key == "pitch") {
				if (!(std::fabs(double(number)) <= limit + 1e-3)) {
					error = "camera.pitch is the degrees it looks down, from -" + std::to_string(int(limit)) + " to " +
							std::to_string(int(limit)) + ".";
					return false;
				}
				held.pitch = std::clamp(float(double(number) * io::kRadiansPerDegree), -kOrbitPitchLimit, kOrbitPitchLimit);
			} else {
				if (!(number > 0.0f)) {
					error = "camera.distance is more than 0.";
					return false;
				}
				held.distance = number;
			}
		} else {
			error = "Unknown camera member \"" + key + "\" (it takes target, yaw, pitch, distance).";
			return false;
		}
	}
	// The distance in the orbit's own range, the far plane past it (as a dolly keeps them).
	const float distance = held.distance;
	held.distance = 1.0f;
	held.dolly(distance);
	camera = held;
	return true;
}

} // namespace opennova::editor
