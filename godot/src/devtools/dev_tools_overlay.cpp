// DevTools — the world-space overlay feed (ADR 0039 d6 as amended): the game
// viewport's camera as the engine's mission-frame OverlayCamera record, and
// the per-tick records the enabled overlay layers draw, pushed BEFORE the
// layout pass so a layer never lags the image it draws over. The engine
// layers project and draw; this file only converts (the camera, the axis
// map) and forwards the ONE engine function per fact.
#include "devtools/dev_tools.h"

#include "simulation/simulation.h"
#include "util/axes.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/variant/projection.hpp>

#include <runtime/devtools/overlay_camera.h>

#include <cmath>
#include <limits>

#if OPENNOVA_DEVTOOLS
#include <runtime/devtools/entity_overlay.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/world/inspect_markers.h>
#endif

namespace godot {

namespace {

// The camera as the engine's overlay record: projection x view x the
// mission -> presentation map, composed once (column-major), plus the
// mission-frame eye and forward the near-plane clip and the range culls use.
opennova::devtools::OverlayCamera overlay_camera_from(Camera3D *p_camera, const Vector2i &p_size) {
	opennova::devtools::OverlayCamera out;
	if (p_camera == nullptr || p_size.x <= 0 || p_size.y <= 0) {
		return out;
	}
	const Transform3D camera_xform = p_camera->get_camera_transform();
	const Projection view_projection = p_camera->get_camera_projection() *
			Projection(camera_xform.affine_inverse() * mission_to_godot_transform());
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			out.view_projection[c * 4 + r] = static_cast<float>(view_projection.columns[c][r]);
		}
	}
	const opennova::env::Vec3 eye = godot_to_mission(camera_xform.origin);
	const Vector3 forward_godot = -camera_xform.basis.get_column(2).normalized();
	const opennova::env::Vec3 forward = godot_to_mission(forward_godot);
	out.eye[0] = eye.x;
	out.eye[1] = eye.y;
	out.eye[2] = eye.z;
	out.forward[0] = forward.x;
	out.forward[1] = forward.y;
	out.forward[2] = forward.z;
	out.near_distance = static_cast<float>(p_camera->get_near());
	out.viewport_width = p_size.x;
	out.viewport_height = p_size.y;
	out.valid = true;
	return out;
}

} // namespace

// Both flavours (the projection is header-only): the test seam that pins the
// engine's projection against Camera3D::unproject_position.
Vector2 DevTools::project_mission_point(Camera3D *p_camera, const Vector3 &p_mission_point) {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	if (p_camera == nullptr || !p_camera->is_inside_tree()) {
		return Vector2(nan, nan);
	}
	const Vector2 size = p_camera->get_viewport()->get_visible_rect().size;
	const opennova::devtools::OverlayCamera camera = overlay_camera_from(
			p_camera, Vector2i(static_cast<int>(size.x), static_cast<int>(size.y)));
	opennova::devtools::OverlayRect rect;
	rect.max_x = size.x;
	rect.max_y = size.y;
	const float point[3] = {static_cast<float>(p_mission_point.x), static_cast<float>(p_mission_point.y),
			static_cast<float>(p_mission_point.z)};
	float out[2];
	if (!opennova::devtools::overlay_project_point(camera, rect, point, out)) {
		return Vector2(nan, nan);
	}
	return Vector2(out[0], out[1]);
}

#if OPENNOVA_DEVTOOLS

// The per-frame overlay feed, ahead of the layout pass. The camera always
// (while any layer is on); each layer's record only when its source moved
// (a new logic tick, a changed selection).
void DevTools::push_overlay_frame() {
	if (!tools_->needs_overlay_camera() || game_viewport_ == nullptr) {
		if (overlay_live_) {
			tools_->clear_overlay_records();
			overlay_live_ = false;
		}
		return;
	}
	overlay_live_ = true;
	const opennova::devtools::OverlayCamera camera =
			overlay_camera_from(game_viewport_->get_camera_3d(), game_viewport_->get_size());
	tools_->set_overlay_camera(camera);
	Simulation *sim = simulation();
	if (sim == nullptr) {
		tools_->clear_overlay_records();
		return;
	}
	const uint64_t tick = static_cast<uint64_t>(sim->get_logic_tick());
	const bool new_tick = tick != overlay_tick_;
	overlay_tick_ = tick;
	const opennova::world::Vec3 eye{camera.eye[0], camera.eye[1], camera.eye[2]};

	if (tools_->needs_entity_markers()) {
		const uint16_t selected = tools_->selected_entity_handle();
		if (new_tick || selected != overlay_selection_) {
			overlay_selection_ = selected;
			opennova::devtools::EntityMarkersRecord record;
			record.logic_tick = tick;
			record.valid = sim->native_entity_markers(tools_->entity_marker_query(eye), record.rows);
			tools_->set_entity_markers(std::move(record));
		}
	}
}

#endif

} // namespace godot
