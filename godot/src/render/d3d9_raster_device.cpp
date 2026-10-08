#include "render/d3d9_raster_device.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot {

void draw_camera_through_d3d9_raster(Camera3D *p_camera, const opennova::renderer::NdcShift &p_shift) {
	if (p_camera == nullptr || !p_camera->is_inside_tree()) {
		return;
	}
	Viewport *viewport = p_camera->get_viewport();
	if (viewport == nullptr) {
		return;
	}
	const Vector2 size = viewport->get_visible_rect().size;
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return;
	}
	const double aspect = static_cast<double>(size.x) / static_cast<double>(size.y);
	const double near_z = p_camera->get_near();
	const double tan_half = Math::tan(Math::deg_to_rad(p_camera->get_fov()) * 0.5);
	// The near plane the perspective form draws: the fov spans the width under
	// KEEP_WIDTH, the height otherwise (Projection::set_perspective's flip).
	double width = 2.0 * near_z * tan_half;
	double height = width / aspect;
	if (p_camera->get_keep_aspect_mode() != Camera3D::KEEP_WIDTH) {
		height = 2.0 * near_z * tan_half;
		width = height * aspect;
	}
	const opennova::renderer::NearPlaneOffset offset = opennova::renderer::near_plane_offset(
			p_shift, static_cast<float>(width), static_cast<float>(height));
	// The frustum form's size is the near plane's HEIGHT to the node
	// (Camera3D::get_camera_projection builds it with no flip), but the server
	// reads it as the WIDTH while its vertical-aspect flag is on (KEEP_WIDTH): the
	// flag goes off, so the matrix the world draws with is the one the node's
	// readers (the HUD, the particle passes, the water mirror) project through.
	// The node keeps its keep mode and its fov for the readers of those.
	p_camera->set_frustum(height, Vector2(offset.x, offset.y), near_z, p_camera->get_far());
	RenderingServer::get_singleton()->camera_set_use_vertical_aspect(p_camera->get_camera_rid(), false);
}

Projection ndc_translation(const opennova::renderer::NdcShift &p_shift) {
	Projection out;
	out.columns[3][0] = p_shift.x;
	out.columns[3][1] = p_shift.y;
	return out;
}

Transform2D d3d9_screen_to_canvas() {
	return Transform2D(0.0f, Vector2(opennova::renderer::kD3d9PixelCentre,
									 opennova::renderer::kD3d9PixelCentre));
}

} // namespace godot
