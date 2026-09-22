#include "hud/hud_inset_scope.h"
#include "render/visual_layers.h"
#include "simulation/player_local_view.h"
#include "util/axes.h"
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

namespace godot {
void HudInsetScope::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("update_view", "view", "source", "aspect_mode"), &HudInsetScope::update_view);
	ClassDB::bind_method(D_METHOD("is_scope_active"), &HudInsetScope::is_scope_active);
	ClassDB::bind_method(D_METHOD("get_render_viewport"), &HudInsetScope::get_render_viewport);
	ClassDB::bind_method(
			D_METHOD("get_active_render_camera"), &HudInsetScope::get_active_render_camera);
	ClassDB::bind_method(D_METHOD("get_render_bounds"), &HudInsetScope::get_render_bounds);
}
Camera3D *HudInsetScope::get_active_render_camera() const {
	// update_view and the visibility notification keep the target's update mode
	// equal to "this pass renders", so it is the one predicate to read.
	const bool rendering =
			active_ && target_ && target_->get_update_mode() == SubViewport::UPDATE_ALWAYS;
	return rendering ? camera_ : nullptr;
}
Rect2 HudInsetScope::get_render_bounds() const {
	return Rect2(geometry_.left, geometry_.top, geometry_.right - geometry_.left + 1,
			geometry_.bottom - geometry_.top + 1);
}
void HudInsetScope::_notification(int what) {
	if (what == NOTIFICATION_VISIBILITY_CHANGED && target_ && !is_visible_in_tree())
		target_->set_update_mode(SubViewport::UPDATE_DISABLED);
}
void HudInsetScope::update_view(const Ref<PlayerLocalView> &view, Camera3D *source, int mode) {
	active_ = view.is_valid() && view->native_frame().inset_scope_active &&
			view->native_frame().camera_pose_valid && source;
	set_visible(active_);
	if (!active_) {
		if (target_)
			target_->set_update_mode(SubViewport::UPDATE_DISABLED);
		return;
	}
	const auto &v = view->native_frame();
	const Vector2 surface = get_size();
	geometry_ =
			opennova::hud::inset_scope_geometry(surface.x, surface.y, mode, v.inset_fov_over_zoom);
	if (!geometry_.valid) {
		active_ = false;
		if (target_)
			target_->set_update_mode(SubViewport::UPDATE_DISABLED);
		return;
	}
	if (!target_) {
		target_ = memnew(SubViewport);
		target_->set_name("InsetTerrainTarget");
		target_->set_disable_input(true);
		target_->set_clear_mode(SubViewport::CLEAR_MODE_ALWAYS);
		add_child(target_);
		camera_ = memnew(Camera3D);
		camera_->set_name("InsetTerrainCamera");
		camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
		target_->add_child(camera_);
		camera_->make_current();
	}
	target_->set_world_3d(source->get_world_3d());
	const Vector2i size(geometry_.right - geometry_.left + 1, geometry_.bottom - geometry_.top + 1);
	if (target_->get_size() != size)
		target_->set_size(size);
	Viewport *surface_view = source->get_viewport();
	target_->set_msaa_3d(surface_view->get_msaa_3d());
	target_->set_screen_space_aa(surface_view->get_screen_space_aa());
	target_->set_mesh_lod_threshold(surface_view->get_mesh_lod_threshold());
	target_->set_use_debanding(surface_view->is_using_debanding());
	camera_->set_near(0.2);
	camera_->set_far(source->get_far());
	camera_->set_fov(geometry_.fov_h_deg);
	// The gameplay camera admits the first-person viewmodel layer; this second
	// scene pass draws terrain, sky and the world only, so the aimed gun must
	// never render magnified inside the aperture.
	camera_->set_cull_mask(source->get_cull_mask() & ~visual_layers::SECOND_SCENE_VIEW_EXCLUDED);
	camera_->set_environment(source->get_environment());
	camera_->set_attributes(source->get_attributes());
	// The same stamp as the gameplay camera: the basis straight from the
	// composed angles, so the magnified Inset view keeps its direction far
	// from the origin (D-VEH-5, util/axes.h).
	const auto &pose = v.inset_camera;
	camera_->set_global_transform(mission_view_transform(
			mission_to_godot(pose.eye), pose.yaw_deg, pose.pitch_deg, pose.roll_deg));
	target_->set_update_mode(
			is_visible_in_tree() ? SubViewport::UPDATE_ALWAYS : SubViewport::UPDATE_DISABLED);
	queue_redraw();
}
void HudInsetScope::_draw() {
	if (!active_ || !geometry_.valid || !target_)
		return;
	PackedVector2Array points, uv;
	PackedColorArray colors;
	colors.push_back(Color(1, 1, 1, 1));
	for (int i = 0; i < 32; ++i) {
		points.push_back(Vector2(geometry_.inner[i].x, geometry_.inner[i].y));
		uv.push_back(Vector2(geometry_.uv[i].x, geometry_.uv[i].y));
	}
	// The inner polygon IS the aperture: the square scene target reaches the
	// surface only inside it. Retail's strip between the inner and the doubled
	// outer ring is no visible band -- black drawn additively (one/one) adds
	// nothing to colour; with depth writes on and the depth compare off it
	// stamps near depth over the square so the scene pass that follows is
	// clipped to the circle. Drawing the target through this polygon is that
	// clip, so the main view stays visible right up to the ring.
	draw_polygon(points, colors, uv, target_->get_texture());
}
} // namespace godot
