#include "render/object_lod_frame.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <base/io/fixed.h>

#include <runtime/renderer/object_lod.h>
#include <runtime/environment/water_mirror.h> // mirror_collect_focal_pixels

#include <algorithm>
#include <cmath>
#include <limits>

namespace godot {

ObjectLodFrame ObjectLodFrame::make(const Transform3D &p_camera_transform,
		float p_vertical_fov_degrees, float p_viewport_width,
		float p_viewport_height, int p_object_detail) {
	if (p_viewport_width <= 0.0f || p_viewport_height <= 0.0f) {
		return ObjectLodFrame();
	}
	const float tan_half_vertical = Math::tan(
			Math::deg_to_rad(CLAMP(p_vertical_fov_degrees, 1.0f, 179.0f)) * 0.5f);
	return from_tangents(p_camera_transform,
			tan_half_vertical * (p_viewport_width / p_viewport_height), tan_half_vertical,
			p_viewport_width, p_object_detail);
}

ObjectLodFrame ObjectLodFrame::from_camera(const Camera3D *p_camera, float p_viewport_width,
		int p_object_detail) {
	float tan_h = 0.0f;
	float tan_v = 0.0f;
	if (!camera_tangents(p_camera, tan_h, tan_v)) {
		return ObjectLodFrame();
	}
	return from_tangents(p_camera->get_global_transform(), tan_h, tan_v, p_viewport_width,
			p_object_detail);
}

ObjectLodFrame ObjectLodFrame::from_tangents(const Transform3D &p_camera_transform,
		float p_tan_half_horizontal, float p_tan_half_vertical, float p_viewport_width,
		int p_object_detail) {
	ObjectLodFrame frame;
	if (p_viewport_width <= 0.0f || !(p_tan_half_horizontal > 0.0f) ||
			!(p_tan_half_vertical > 0.0f)) {
		return frame;
	}
	frame.tan_half_vertical = p_tan_half_vertical;
	frame.tan_half_horizontal = p_tan_half_horizontal;
	// Retail's viewport focal: half the viewport width over the horizontal
	// half-angle tangent, an integer pixel count (engine:
	// renderer::object_lod_focal_pixels).
	frame.focal_pixels = opennova::renderer::object_lod_focal_pixels(
			p_viewport_width, static_cast<double>(frame.tan_half_horizontal));
	frame.projection_scale = opennova::renderer::object_lod_frame_scale(
			p_object_detail, p_viewport_width);
	frame.origin = p_camera_transform.origin;
	frame.forward = -p_camera_transform.basis.get_column(2).normalized();
	frame.right = p_camera_transform.basis.get_column(0).normalized();
	frame.up = p_camera_transform.basis.get_column(1).normalized();
	frame.frustum = eye_frustum(p_camera_transform, frame.tan_half_horizontal,
			frame.tan_half_vertical, 0.0f, 0.0f);
	frame.valid = frame.focal_pixels > 0 && frame.projection_scale > 0.0f;
	return frame;
}

ObjectLodFrame ObjectLodFrame::mirror_collect(const Transform3D &p_eye,
		float p_tan_half_horizontal, float p_screen_width, int p_object_detail) {
	ObjectLodFrame frame = from_tangents(p_eye, p_tan_half_horizontal, p_tan_half_horizontal,
			p_screen_width, p_object_detail);
	if (!frame.valid) {
		return frame;
	}
	frame.focal_pixels =
			opennova::env::mirror_collect_focal_pixels(static_cast<double>(p_tan_half_horizontal));
	frame.valid = frame.focal_pixels > 0;
	return frame;
}

bool ObjectLodFrame::camera_tangents(const Camera3D *p_camera,
		float &r_tan_half_horizontal, float &r_tan_half_vertical) {
	if (p_camera == nullptr) {
		return false;
	}
	Viewport *viewport = p_camera->get_viewport();
	if (viewport == nullptr) {
		return false;
	}
	const Vector2 size = viewport->get_visible_rect().size;
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return false;
	}
	const float tan_half = Math::tan(
			Math::deg_to_rad(CLAMP(static_cast<float>(p_camera->get_fov()), 1.0f, 179.0f)) * 0.5f);
	const float aspect = size.x / size.y;
	if (p_camera->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH) {
		r_tan_half_horizontal = tan_half;
		r_tan_half_vertical = tan_half / aspect;
	} else {
		r_tan_half_vertical = tan_half;
		r_tan_half_horizontal = tan_half * aspect;
	}
	return true;
}

opennova::renderer::ViewFrustum ObjectLodFrame::eye_frustum(const Transform3D &p_eye,
		float p_tan_half_horizontal, float p_tan_half_vertical, float p_near, float p_far) {
	const Vector3 forward_axis = -p_eye.basis.get_column(2).normalized();
	const Vector3 right_axis = p_eye.basis.get_column(0).normalized();
	const Vector3 up_axis = p_eye.basis.get_column(1).normalized();
	const float eye[3] = { p_eye.origin.x, p_eye.origin.y, p_eye.origin.z };
	const float forward[3] = { forward_axis.x, forward_axis.y, forward_axis.z };
	const float right[3] = { right_axis.x, right_axis.y, right_axis.z };
	const float up[3] = { up_axis.x, up_axis.y, up_axis.z };
	return opennova::renderer::view_frustum(eye, forward, right, up, p_tan_half_horizontal,
			p_tan_half_vertical, p_near, p_far);
}

opennova::renderer::ViewFrustum ObjectLodFrame::camera_frustum(const Camera3D *p_camera) {
	float tan_half_horizontal = 0.0f;
	float tan_half_vertical = 0.0f;
	if (p_camera == nullptr || p_camera->get_far() <= p_camera->get_near() ||
			!camera_tangents(p_camera, tan_half_horizontal, tan_half_vertical)) {
		return opennova::renderer::ViewFrustum();
	}
	return eye_frustum(p_camera->get_camera_transform(), tan_half_horizontal, tan_half_vertical,
			static_cast<float>(p_camera->get_near()), static_cast<float>(p_camera->get_far()));
}

float ObjectLodFrame::uniform_scale(const Basis &p_basis) {
	return std::max({ p_basis.get_column(0).length(),
			p_basis.get_column(1).length(), p_basis.get_column(2).length() });
}

Vector3 ObjectLodFrame::cobj_center_local(const std::array<int32_t, 3> &p_center_q16) {
	return Vector3(opennova::io::fp16_16_to_float(p_center_q16[1]),
			opennova::io::fp16_16_to_float(p_center_q16[2]),
			opennova::io::fp16_16_to_float(p_center_q16[0]));
}

Vector3 ObjectLodFrame::projection_center(const Transform3D &p_transform,
		const opennova::renderer::ObjectProjectionSphere &p_sphere,
		int32_t p_entity_scale_q16) {
	const Vector3 local = cobj_center_local(p_sphere.center_q16);
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
	const float center[3] = { p_center.x, p_center.y, p_center.z };
	return opennova::renderer::view_frustum_sphere_visible(frustum, center, p_radius);
}

bool ObjectLodFrame::project(const Vector3 &p_center, float p_radius,
		int32_t &r_radius_q16) const {
	return project_q16(p_center, opennova::io::float_to_fp16_16_round_sat(p_radius),
			r_radius_q16);
}

bool ObjectLodFrame::project_q16(const Vector3 &p_center, int32_t p_radius_q16,
		int32_t &r_projected_q16) const {
	if (!sphere_in_frustum(p_center, opennova::io::fp16_16_to_float(p_radius_q16))) {
		return false;
	}
	const float depth = (p_center - origin).dot(forward);
	r_projected_q16 = opennova::renderer::project_bound_sphere_radius_q16(
			p_radius_q16, opennova::io::float_to_fp16_16_round_sat(depth), focal_pixels);
	return true;
}

} // namespace godot
