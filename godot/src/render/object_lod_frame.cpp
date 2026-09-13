#include "render/object_lod_frame.h"

#include <godot_cpp/core/math.hpp>
#include <base/io/fixed.h>

#include <runtime/renderer/object_lod.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace godot {

ObjectLodFrame ObjectLodFrame::make(const Transform3D &p_camera_transform,
		float p_vertical_fov_degrees, float p_viewport_width,
		float p_viewport_height) {
	ObjectLodFrame frame;
	if (p_viewport_width <= 0.0f || p_viewport_height <= 0.0f) {
		return frame;
	}
	const float half_fov =
			Math::deg_to_rad(CLAMP(p_vertical_fov_degrees, 1.0f, 179.0f)) * 0.5f;
	frame.tan_half_vertical = Math::tan(half_fov);
	frame.tan_half_horizontal =
			frame.tan_half_vertical * (p_viewport_width / p_viewport_height);
	frame.vertical_plane_scale =
			std::sqrt(1.0f + frame.tan_half_vertical * frame.tan_half_vertical);
	frame.horizontal_plane_scale = std::sqrt(
			1.0f + frame.tan_half_horizontal * frame.tan_half_horizontal);
	// Retail's viewport focal length is an integer pixel count.
	frame.focal_pixels = static_cast<int32_t>(Math::round(
			p_viewport_height * 0.5f / frame.tan_half_vertical));
	frame.projection_scale = opennova::renderer::object_lod_frame_scale(
			opennova::renderer::kObjectLodDetailLevelMax, p_viewport_width);
	frame.origin = p_camera_transform.origin;
	frame.forward = -p_camera_transform.basis.get_column(2).normalized();
	frame.right = p_camera_transform.basis.get_column(0).normalized();
	frame.up = p_camera_transform.basis.get_column(1).normalized();
	frame.valid = frame.focal_pixels > 0 && frame.projection_scale > 0.0f;
	return frame;
}

float ObjectLodFrame::uniform_scale(const Basis &p_basis) {
	return std::max({ p_basis.get_column(0).length(),
			p_basis.get_column(1).length(), p_basis.get_column(2).length() });
}

Vector3 ObjectLodFrame::projection_center(const Transform3D &p_transform,
		const opennova::renderer::ObjectProjectionSphere &p_sphere,
		int32_t p_entity_scale_q16) {
	const auto &center = p_sphere.center_q16;
	const Vector3 local(static_cast<float>(center[1]) / 65536.0f,
			static_cast<float>(center[2]) / 65536.0f,
			static_cast<float>(center[0]) / 65536.0f);
	Basis pose = p_transform.basis;
	if (p_entity_scale_q16 != 0) {
		const float inverse_scale = 65536.0f / static_cast<float>(p_entity_scale_q16);
		pose = pose.scaled(Vector3(inverse_scale, inverse_scale, inverse_scale));
	}
	return p_transform.origin + pose.xform(local);
}

bool ObjectLodFrame::sphere_in_frustum(const Vector3 &p_center,
		float p_radius) const {
	if (!valid) {
		return false;
	}
	const Vector3 relative = p_center - origin;
	const float depth = relative.dot(forward);
	if (depth + p_radius <= 0.0f) {
		return false;
	}
	// The frustum side planes, with the sphere's true plane distance.
	const float lateral = std::fabs(relative.dot(right));
	if (lateral - depth * tan_half_horizontal >
			p_radius * horizontal_plane_scale) {
		return false;
	}
	const float vertical = std::fabs(relative.dot(up));
	return vertical - depth * tan_half_vertical <=
			p_radius * vertical_plane_scale;
}

bool ObjectLodFrame::project(const Vector3 &p_center, float p_radius,
		int32_t &r_radius_q16) const {
	return project_q16(p_center, opennova::io::float_to_fp16_16_round_sat(p_radius),
			r_radius_q16);
}

bool ObjectLodFrame::project_q16(const Vector3 &p_center, int32_t p_radius_q16,
		int32_t &r_projected_q16) const {
	if (!sphere_in_frustum(p_center, static_cast<float>(p_radius_q16) / 65536.0f)) {
		return false;
	}
	const float depth = (p_center - origin).dot(forward);
	r_projected_q16 = opennova::renderer::project_bound_sphere_radius_q16(
			p_radius_q16, opennova::io::float_to_fp16_16_round_sat(depth), focal_pixels);
	return true;
}

} // namespace godot
