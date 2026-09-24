// DevTools — the world-space overlay feed (ADR 0039 d6 as amended): the game
// viewport's camera as the engine's mission-frame OverlayCamera record, and
// the per-tick records the enabled overlay layers draw, pushed BEFORE the
// layout pass so a layer never lags the image it draws over. The engine
// layers project and draw; this file only converts (the camera, the axis
// map) and forwards the ONE engine function per fact.
#include "devtools/dev_tools.h"

#include "devtools/debug_control_table.h"
#include "devtools/debug_shell_host.h"
#include "player/local_player_presenter.h"
#include "simulation/simulation.h"
#include "util/axes.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/variant/projection.hpp>

#include <runtime/devtools/overlay_camera.h>

#include <cmath>
#include <limits>

#if OPENNOVA_DEVTOOLS
#include <godot_cpp/classes/time.hpp>
#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/collision_overlay.h>
#include <runtime/devtools/entity_overlay.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/hitbox_overlay.h>
#include <runtime/mission/debug_oracles.h>
#include <runtime/world/collision_debug_rows.h>
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

// The camera the Game image's pixels come from: the presenter's stretched-
// frame camera while its target is live (the surface's gameplay camera then
// carries only a culling superset of the frustum, and the target's image is
// blitted over the whole surface), else the surface's own camera.
Camera3D *DevTools::image_camera() const {
	Camera3D *surface_camera = game_viewport_ != nullptr ? game_viewport_->get_camera_3d() : nullptr;
	const Ref<DebugShellHost> host = control_table_.is_valid() ? control_table_->get_host() : Ref<DebugShellHost>();
	LocalPlayerPresenter *presenter = host.is_valid() ? host->player_presenter() : nullptr;
	Camera3D *through = presenter != nullptr ? presenter->projection_camera() : nullptr;
	return through != nullptr && presenter->projection_viewport() != nullptr ? through : surface_camera;
}

// The per-frame overlay feed, ahead of the layout pass. The camera always
// (while any layer is on); each layer's record only when its source moved
// (a new logic tick, a changed selection).
void DevTools::push_overlay_frame() {
	if (!tools_->needs_overlay_camera() || game_viewport_ == nullptr) {
		if (overlay_live_) {
			tools_->clear_overlay_records();
			overlay_live_ = false;
			// A reopen (or a layer back on) re-reads every record at once,
			// even with the clock paused.
			overlay_tick_ = static_cast<uint64_t>(-1);
			overlay_wants_ = 0;
		}
		return;
	}
	overlay_live_ = true;
	const opennova::devtools::OverlayCamera camera =
			overlay_camera_from(image_camera(), game_viewport_->get_size());
	tools_->set_overlay_camera(camera);
	Simulation *sim = simulation();
	if (sim == nullptr) {
		tools_->clear_overlay_records();
		return;
	}
	// A record re-reads on a new logic tick, and on the frame its layer comes
	// on (a paused world's tick never moves).
	const uint32_t wants = (tools_->needs_entity_markers() ? 1u : 0u) |
			(tools_->needs_ai_overlay() ? 2u : 0u) | (tools_->needs_rays_overlay() ? 4u : 0u) |
			(tools_->needs_contacts_overlay() ? 8u : 0u) | (tools_->needs_hitbox_overlay() ? 16u : 0u);
	const bool wants_grew = (wants & ~overlay_wants_) != 0;
	overlay_wants_ = wants;
	if ((wants & 16u) != 0 && wants_grew) last_hitbox_push_ms_ = -1;
	const uint64_t tick = static_cast<uint64_t>(sim->get_logic_tick());
	const bool new_tick = tick != overlay_tick_ || wants_grew;
	overlay_tick_ = tick;
	const opennova::world::Vec3 eye{camera.eye[0], camera.eye[1], camera.eye[2]};

	if (tools_->needs_entity_marker_refresh(tick, eye)) {
		opennova::devtools::EntityMarkersRecord record;
		record.logic_tick = tick;
		record.query = tools_->entity_marker_query(eye);
		record.valid = sim->native_entity_markers(record.query, record.rows);
		tools_->set_entity_markers(std::move(record));
	}
	if (tools_->needs_ai_overlay() && new_tick) {
		// The layers draw the AI window's record, so a tick's push serves the
		// window too (its cadence push then skips).
		opennova::devtools::AiDebugSnapshot snapshot;
		snapshot.valid = sim->native_ai_debug(snapshot.report);
		snapshot.logic_tick = tick;
		tools_->set_ai_debug(std::move(snapshot));
		last_ai_push_ms_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
	}
	// The capture rows move with the tick and with the windows' filters.
	if (tools_->needs_rays_overlay() && (new_tick || overlay_filters_dirty_)) {
		opennova::devtools::RaysOverlayRecord record;
		record.logic_tick = tick;
		record.valid = sim->native_ray_debug_rows(record.rows, record.ttl_ticks);
		tools_->set_rays_overlay(std::move(record));
	}
	if (tools_->needs_contacts_overlay() && (new_tick || overlay_filters_dirty_)) {
		opennova::devtools::ContactsOverlayRecord record;
		record.logic_tick = tick;
		record.valid = sim->native_contact_debug_rows(record.rows, record.ttl_ticks);
		tools_->set_contacts_overlay(std::move(record));
	}
	overlay_filters_dirty_ = false;
	// The hitbox oracle transforms whole bullet meshes: its own cadence.
	if (tools_->needs_hitbox_overlay() &&
			push_due(last_hitbox_push_ms_, 1.0 / opennova::devtools::HitboxOverlayLayer::kRefreshHz)) {
		opennova::devtools::HitboxOverlayRecord record;
		record.logic_tick = tick;
		record.valid = sim->native_hitbox_debug(eye, record.report);
		tools_->set_hitbox_overlay(std::move(record));
	}
}

#endif

} // namespace godot
