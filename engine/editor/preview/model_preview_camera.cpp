#include <editor/preview/model_preview_camera.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <base/io/fixed.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/world/model_geometry.h>
#include <runtime/world/player_view.h>

namespace opennova::editor {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kOrbitRadiansPerPixel = 0.01f;
constexpr float kMinDistance = 0.01f;
constexpr float kMaxDistance = 100000.0f;
constexpr float kFrameMargin = 1.15f; // the sphere with a little room around it

float dot(const PreviewVec3 &a, const PreviewVec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

PreviewVec3 sub(const PreviewVec3 &a, const PreviewVec3 &b) { return PreviewVec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

float tan_half_fov() { return std::tan(OrbitCamera::fov_horizontal_degrees() * kPi / 360.0f); }

} // namespace

float OrbitCamera::fov_horizontal_degrees() { return world::kPlayerCameraFovHDeg; }

PreviewVec3 OrbitCamera::eye() const {
	const float c = std::cos(pitch);
	return PreviewVec3{target.x + distance * c * std::sin(yaw), target.y + distance * std::sin(pitch),
	                   target.z + distance * c * std::cos(yaw)};
}

void OrbitCamera::axes(PreviewVec3 &right, PreviewVec3 &up, PreviewVec3 &back) const {
	const float c = std::cos(pitch);
	back = PreviewVec3{c * std::sin(yaw), std::sin(pitch), c * std::cos(yaw)};
	// right = world up x back, normalized (the pitch stays short of the poles).
	right = PreviewVec3{back.z, 0.0f, -back.x};
	const float length = std::sqrt(right.x * right.x + right.z * right.z);
	right.x /= length;
	right.z /= length;
	up = PreviewVec3{back.y * right.z - back.z * right.y, back.z * right.x - back.x * right.z,
	                 back.x * right.y - back.y * right.x};
}

float OrbitCamera::focal_pixels(int width) { return 0.5f * static_cast<float>(width) / tan_half_fov(); }

bool OrbitCamera::project(const PreviewVec3 &point, int width, int height, float &x, float &y, float *depth) const {
	PreviewVec3 right, up, back;
	axes(right, up, back);
	const PreviewVec3 relative = sub(point, eye());
	const float ahead = -dot(relative, back);
	if (depth) *depth = ahead;
	if (ahead < near_plane) return false;
	const float focal = focal_pixels(width);
	x = 0.5f * static_cast<float>(width) + dot(relative, right) * focal / ahead;
	y = 0.5f * static_cast<float>(height) - dot(relative, up) * focal / ahead;
	return true;
}

bool OrbitCamera::on_view_plane(float x, float y, int width, int height, const PreviewVec3 &through,
                                PreviewVec3 &out) const {
	if (width <= 0 || height <= 0) return false;
	PreviewVec3 right, up, back;
	axes(right, up, back);
	const float focal = focal_pixels(width);
	const float sx = (x - 0.5f * static_cast<float>(width)) / focal;
	const float sy = (0.5f * static_cast<float>(height) - y) / focal;
	const PreviewVec3 ray{right.x * sx + up.x * sy - back.x, right.y * sx + up.y * sy - back.y,
	                      right.z * sx + up.z * sy - back.z};
	const float facing = dot(ray, back);
	if (!(facing < 0.0f)) return false;
	const PreviewVec3 from = eye();
	const float t = dot(sub(through, from), back) / facing;
	if (!(t > 0.0f)) return false;
	out = PreviewVec3{from.x + ray.x * t, from.y + ray.y * t, from.z + ray.z * t};
	return true;
}

void OrbitCamera::frame(const PreviewVec3 &center, float radius, int width, int height) {
	radius = std::max(radius, 0.01f);
	const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 4.0f / 3.0f;
	// The narrower half-angle of the two (keep-width: the vertical one on a wide device).
	const float half = std::min(std::atan(tan_half_fov()), std::atan(tan_half_fov() / aspect));
	target = center;
	distance = std::clamp(kFrameMargin * radius / std::sin(half), kMinDistance, kMaxDistance);
	near_plane = std::max(0.001f, std::min(0.05f, radius * 0.01f));
	far_plane = distance + radius * 20.0f + 100.0f;
}

void OrbitCamera::orbit(float dx, float dy) {
	yaw = std::fmod(yaw - dx * kOrbitRadiansPerPixel, 2.0f * kPi);
	pitch = std::clamp(pitch + dy * kOrbitRadiansPerPixel, -kOrbitPitchLimit, kOrbitPitchLimit);
}

void OrbitCamera::pan(float dx, float dy, int width) {
	if (width <= 0) return;
	PreviewVec3 right, up, back;
	axes(right, up, back);
	const float units = distance / focal_pixels(width); // world units per pixel at the target
	target.x += (-dx * right.x + dy * up.x) * units;
	target.y += (-dx * right.y + dy * up.y) * units;
	target.z += (-dx * right.z + dy * up.z) * units;
}

void OrbitCamera::dolly(float factor) {
	if (!(factor > 0.0f)) return;
	distance = std::clamp(distance * factor, kMinDistance, kMaxDistance);
	far_plane = std::max(far_plane, distance * 2.0f + 100.0f);
}

void model_preview_sphere(const threedi::Threedi3di3 &model, PreviewVec3 &center, float &radius) {
	const renderer::ObjectProjectionSphere sphere = world::collision_projection_sphere_from_3di(model);
	if (sphere.valid && sphere.radius_q16 > 0) {
		const float at[3] = {sphere.center_q16[0] * io::kInvFp16One, sphere.center_q16[1] * io::kInvFp16One,
		                     sphere.center_q16[2] * io::kInvFp16One};
		center = preview_from_model(at);
		radius = sphere.radius_q16 * io::kInvFp16One;
		return;
	}
	center = PreviewVec3{};
	radius = static_cast<float>(static_cast<uint32_t>(model.header.max_radius_fp16)) * io::kInvFp16One;
	if (!(radius > 0.0f)) radius = 1.0f;
}

int model_preview_auto_lod(const threedi::Threedi3di3 &model, const OrbitCamera &camera, int width,
                           int32_t *projected_q16) {
	if (projected_q16) *projected_q16 = 0;
	if (!model.lods || model.lod_count == 0 || width <= 0) return -1;
	const renderer::ObjectProjectionSphere sphere = world::collision_projection_sphere_from_3di(model);
	const float at[3] = {sphere.center_q16[0] * io::kInvFp16One, sphere.center_q16[1] * io::kInvFp16One,
	                     sphere.center_q16[2] * io::kInvFp16One};
	PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	const float depth = -dot(sub(preview_from_model(at), camera.eye()), back);
	const double depth_q16 = std::clamp(static_cast<double>(depth) * io::kFp16OneD, -2147483647.0, 2147483647.0);
	const int32_t focal = renderer::object_lod_focal_pixels(static_cast<float>(width), tan_half_fov());
	const int32_t radius = renderer::project_bound_sphere_radius_q16(sphere.radius_q16, static_cast<int32_t>(depth_q16), focal);
	if (projected_q16) *projected_q16 = radius;
	std::vector<int32_t> thresholds;
	for (size_t i = 0; i < model.lod_count; ++i)
		thresholds.push_back(renderer::rlod_threshold_q16_from_rmdl(model.lods[i].lod_threshold));
	const renderer::ObjectLodSelection selection = renderer::select_object_lod(
	    thresholds, radius, renderer::object_lod_frame_scale(renderer::kObjectLodDetailLevelMax, static_cast<float>(width)));
	if (selection.lod_index < 0) return static_cast<int>(model.lod_count) - 1;
	return std::min(selection.lod_index, static_cast<int>(model.lod_count) - 1);
}

} // namespace opennova::editor
