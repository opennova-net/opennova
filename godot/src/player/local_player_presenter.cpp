#include "player/local_player_presenter.h"

#include "env/mission_environment.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_root.h"
#include "object/item_database.h"
#include "player/local_player_visuals.h"
#include "player/player_viewmodel_def.h"
#include "render/visual_layers.h"
#include "simulation/entity_presenter.h"
#include "simulation/player_weapon_view.h"
#include "simulation/simulation.h"
#include "terrain/terrain_data.h"

#include <godot_cpp/classes/camera_attributes.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <cmath>

#include <runtime/world/player_present.h>

using namespace godot;

namespace {

const Vector3 kNoSample(INFINITY, INFINITY, INFINITY);
const Vector2 kNoProjection(INFINITY, INFINITY);
// The stretched-mode blit sits beneath the root canvas and every HUD/menu
// layer: it is the world picture those layers (the screen-reading view
// effects included) draw over.
constexpr int kViewProjectionBlitLayer = -1;
// Ratios within this of the surface's own draw the surface directly.
constexpr float kViewProjectionUnstretched = 0.0001f;

// The target renders the world the surface used to: mirror the surface's 3D
// quality settings onto it (a runtime SubViewport starts from the project
// defaults, which the surface may have been retuned away from).
void mirror_viewport_quality(Viewport *p_surface, SubViewport *p_target) {
	if (p_target->get_msaa_3d() != p_surface->get_msaa_3d()) {
		p_target->set_msaa_3d(p_surface->get_msaa_3d());
	}
	if (p_target->get_screen_space_aa() != p_surface->get_screen_space_aa()) {
		p_target->set_screen_space_aa(p_surface->get_screen_space_aa());
	}
	if (p_target->is_using_taa() != p_surface->is_using_taa()) {
		p_target->set_use_taa(p_surface->is_using_taa());
	}
	if (p_target->is_using_debanding() != p_surface->is_using_debanding()) {
		p_target->set_use_debanding(p_surface->is_using_debanding());
	}
	if (p_target->is_using_occlusion_culling() != p_surface->is_using_occlusion_culling()) {
		p_target->set_use_occlusion_culling(p_surface->is_using_occlusion_culling());
	}
	if (p_target->get_mesh_lod_threshold() != p_surface->get_mesh_lod_threshold()) {
		p_target->set_mesh_lod_threshold(p_surface->get_mesh_lod_threshold());
	}
	if (p_target->get_scaling_3d_mode() != p_surface->get_scaling_3d_mode()) {
		p_target->set_scaling_3d_mode(p_surface->get_scaling_3d_mode());
	}
	if (p_target->get_scaling_3d_scale() != p_surface->get_scaling_3d_scale()) {
		p_target->set_scaling_3d_scale(p_surface->get_scaling_3d_scale());
	}
	if (p_target->get_fsr_sharpness() != p_surface->get_fsr_sharpness()) {
		p_target->set_fsr_sharpness(p_surface->get_fsr_sharpness());
	}
	if (p_target->get_texture_mipmap_bias() != p_surface->get_texture_mipmap_bias()) {
		p_target->set_texture_mipmap_bias(p_surface->get_texture_mipmap_bias());
	}
	if (p_target->get_anisotropic_filtering_level() != p_surface->get_anisotropic_filtering_level()) {
		p_target->set_anisotropic_filtering_level(p_surface->get_anisotropic_filtering_level());
	}
	if (p_target->get_positional_shadow_atlas_size() != p_surface->get_positional_shadow_atlas_size()) {
		p_target->set_positional_shadow_atlas_size(p_surface->get_positional_shadow_atlas_size());
	}
	if (p_target->get_positional_shadow_atlas_16_bits() != p_surface->get_positional_shadow_atlas_16_bits()) {
		p_target->set_positional_shadow_atlas_16_bits(p_surface->get_positional_shadow_atlas_16_bits());
	}
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		if (p_target->get_positional_shadow_atlas_quadrant_subdiv(quadrant) !=
				p_surface->get_positional_shadow_atlas_quadrant_subdiv(quadrant)) {
			p_target->set_positional_shadow_atlas_quadrant_subdiv(quadrant,
					p_surface->get_positional_shadow_atlas_quadrant_subdiv(quadrant));
		}
	}
}

// The thermal view feed, the NVG feed's sibling: the sim's two resolved gates
// (world::LocalPlayerViewFrame) onto the world's environment, resolved the way
// LocalPlayerVisuals resolves it.
void feed_world_thermal_view(Node *p_world, bool p_world_gate, bool p_terrain_gate) {
	if (p_world == nullptr) {
		return;
	}
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			static_cast<Object *>(p_world->call("get_environment_node")));
	if (env != nullptr) {
		env->set_thermal_view(p_world_gate, p_terrain_gate);
	}
}

} // namespace

LocalPlayerPresenter::LocalPlayerPresenter() {
	viewmodel_rig_.instantiate();
}

// The sim, re-resolved per use: mission reloads free the runtime and its sim,
// so a cached reference would go stale (the debug views follow the same rule).
Ref<Simulation> LocalPlayerPresenter::sim() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<Simulation>();
	}
	return Ref<Simulation>(node->call("get_sim"));
}

Node *LocalPlayerPresenter::world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

Camera3D *LocalPlayerPresenter::camera() const {
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(camera_id_));
}

Camera3D *LocalPlayerPresenter::projection_camera() const {
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(projection_camera_id_));
}

SubViewport *LocalPlayerPresenter::projection_viewport() const {
	return Object::cast_to<SubViewport>(ObjectDB::get_instance(projection_viewport_id_));
}

Projection LocalPlayerPresenter::view_projection() const {
	if (Camera3D *through = projection_camera()) {
		return through->get_camera_projection();
	}
	Camera3D *cam = camera();
	return cam != nullptr ? cam->get_camera_projection() : Projection();
}

GameplayCamera *LocalPlayerPresenter::fly_camera() const {
	return Object::cast_to<GameplayCamera>(ObjectDB::get_instance(fly_camera_id_));
}

ObjectModel *LocalPlayerPresenter::avatar() const {
	return Object::cast_to<ObjectModel>(ObjectDB::get_instance(avatar_id_));
}

ObjectModel *LocalPlayerPresenter::held_weapon() const {
	return Object::cast_to<ObjectModel>(ObjectDB::get_instance(held_weapon_id_));
}

TypedArray<ObjectModel> LocalPlayerPresenter::vm_parts() const {
	return viewmodel_rig_->vm_parts();
}

Node3D *LocalPlayerPresenter::viewmodel() const {
	return viewmodel_rig_->viewmodel();
}

void LocalPlayerPresenter::set_fp_gun_visible(bool p_visible) {
	viewmodel_rig_->set_fp_gun_visible(p_visible);
}

void LocalPlayerPresenter::setup(Node *p_world, Camera3D *p_camera, GameplayCamera *p_fly_camera,
		const Ref<ControlsModel> &p_controls) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	camera_id_ = p_camera != nullptr ? ObjectID(p_camera->get_instance_id()) : ObjectID();
	fly_camera_id_ = p_fly_camera != nullptr ? ObjectID(p_fly_camera->get_instance_id()) : ObjectID();
	visuals_.unref();
	if (p_world != nullptr) {
		visuals_ = Ref<LocalPlayerVisuals>(p_world->call("local_player_visuals"));
	}
	// The weapon-event presentation lives beside the presenter for the same
	// setup -> teardown span; it resolves userpoints against this presenter's
	// live nodes.
	weapon_effects_.instantiate();
	weapon_effects_->setup(p_world, this);
	input_router_.setup(p_world, this, p_controls);
	camera_saved_fov_ = p_camera != nullptr ? static_cast<float>(p_camera->get_fov()) : -1.0f;
	camera_saved_keep_aspect_ = p_camera != nullptr ? static_cast<int>(p_camera->get_keep_aspect_mode()) : -1;
	camera_saved_cull_mask_ = p_camera != nullptr ? static_cast<int64_t>(p_camera->get_cull_mask()) : -1;
	// The player camera never draws the FP body layer: retail renders no local
	// body in first person, and the water mirror never draws persons either
	// (its reflected entity waves collect only vehicles above water and it has
	// no player-render leg [orig: Terrain_CollectVisibleEntitiesForReflection
	// @ 0x5c90a0]) -- the body stays a silhouette source only. The viewmodel
	// layer stays ADMITTED: the FP arms/weapon draw inside the beauty pass
	// through their shader-side renderfov projection + depth band (retail's
	// "viewmodel first" step; PlayerViewmodelRig feeds the projection). The
	// camera excludes the caster marker layers: the entity-shadow shadow map
	// is retired (the slot pipeline owns entity shadows through SlotShadow's
	// RenderingDevice pass, which reserves no visual layer), so nothing needs
	// the caster markers beauty-admitted any more -- and the FP body/held
	// weapon are camera-renderable (cast ON, hidden by LAYER) so that pass
	// walks their visible geometry; an admitted caster marker would leak them
	// into the beauty pass.
	if (p_camera != nullptr) {
		p_camera->set_cull_mask((p_camera->get_cull_mask() | visual_layers::VIEWMODEL) &
				~(visual_layers::FP_BODY_SHADOW_ONLY | visual_layers::SHADOW_CASTER_MASK));
	}
	viewmodel_rig_->setup(p_world, this, p_camera);
	reset_state();
	// Attachment is the adoption boundary: discard presentation history
	// produced before this presenter existed. Every event produced after setup
	// is live, including a first-tick shot before the first active snapshot is
	// presented. From here the world's fixed-tick drain hands each batch to
	// present_fixed_weapon_tick through its local view presenter.
	if (p_world != nullptr) {
		if (visuals_.is_valid()) {
			visuals_->drain_local_player_weapon_events();
		}
		p_world->call("set_local_view_presenter", this);
	}
}

void LocalPlayerPresenter::teardown() {
	if (Node *node = world()) {
		// Release the world-to-presenter fixed-tick seam (only when it is
		// still ours).
		if (Object::cast_to<LocalPlayerPresenter>(static_cast<Object *>(node->call("local_view_presenter"))) == this) {
			node->call("set_local_view_presenter", Variant());
		}
	}
	set_fly_camera_locked(false);
	input_router_.release_mouse_capture();
	clear_models();
	viewmodel_rig_->teardown();
	if (Camera3D *cam = camera()) {
		if (camera_saved_cull_mask_ >= 0) {
			cam->set_cull_mask(static_cast<uint32_t>(camera_saved_cull_mask_));
		}
		if (camera_saved_fov_ > 0.0f) {
			cam->set_fov(camera_saved_fov_);
		}
		if (camera_saved_keep_aspect_ >= 0) {
			cam->set_keep_aspect_mode(static_cast<Camera3D::KeepAspect>(camera_saved_keep_aspect_));
		}
	}
	camera_saved_keep_aspect_ = -1;
	if (weapon_effects_.is_valid()) {
		weapon_effects_->teardown();
	}
	weapon_effects_.unref();
	input_router_.teardown();
	world_id_ = ObjectID();
	camera_id_ = ObjectID();
	visuals_.unref();
	camera_saved_cull_mask_ = -1;
	reset_state();
}

void LocalPlayerPresenter::refresh_viewmodel() {
	if (weapon_effects_.is_valid()) {
		weapon_effects_->on_viewmodel_refresh();
	}
	viewmodel_rig_->refresh_viewmodel();
}

int LocalPlayerPresenter::viewmodel_generation() const {
	return viewmodel_rig_->viewmodel_generation();
}

void LocalPlayerPresenter::set_input_override(const Ref<PlayerMoveIntent> &p_intent) {
	input_router_.set_input_override(p_intent);
}

void LocalPlayerPresenter::set_third_person_selected(bool p_selected) {
	const Ref<Simulation> pref_sim = sim();
	if (pref_sim.is_valid()) {
		pref_sim->set_local_player_third_person_selected(p_selected);
	}
	refresh_camera_mode();
}

void LocalPlayerPresenter::set_debug_third_person(bool p_enabled) {
	debug_third_person_ = p_enabled;
	const Ref<Simulation> debug_sim = sim();
	if (debug_sim.is_valid()) {
		debug_sim->set_local_player_debug_third_person(p_enabled);
	}
	refresh_camera_mode();
}

// Re-read the resolved mode right after a preference/override write so the
// same frame's layer decisions see it (the tick refresh keeps it current).
// Every non-first-person mode presents the body and hides the FP arms (the
// engine's presents_third_person, world/player_present.h).
void LocalPlayerPresenter::refresh_camera_mode() {
	if (world() == nullptr || sim().is_null() || visuals_.is_null()) {
		third_person_ = debug_third_person_;
		return;
	}
	const Ref<PlayerLocalView> view = visuals_->local_player_view();
	third_person_ = view.is_valid() &&
			opennova::world::presents_third_person(view->get_third_person(), view->get_camera_mode());
}

// Node3D.global_transform never compares: an unchanged write still dirties
// the avatar's whole subtree (ROBJ nodes, mesh instances, the skeleton) into
// the deferred flush's transform notifications. A still player writes nothing.
void LocalPlayerPresenter::set_avatar_transform(ObjectModel *p_avatar, const Transform3D &p_next) {
	if (p_avatar->get_global_transform() != p_next) {
		p_avatar->set_global_transform(p_next);
	}
}

void LocalPlayerPresenter::set_viewmodel_capture_hidden(bool p_hidden) {
	viewmodel_rig_->set_capture_hidden(p_hidden);
}

void LocalPlayerPresenter::restamp_viewmodel_at_camera() {
	viewmodel_rig_->restamp_at_camera();
}

Ref<MissionFrameInput> LocalPlayerPresenter::before_world_tick(double p_delta, bool p_capture_mouse,
		bool p_gameplay_input_active) {
	return input_router_.before_world_tick(p_delta, p_capture_mouse, p_gameplay_input_active);
}

void LocalPlayerPresenter::after_world_tick() {
	if (!has_player()) {
		set_world_nvg_view(false, 0);
		feed_world_thermal_view(world(), false, false);
		set_fly_camera_locked(false);
		input_router_.release_mouse_capture();
		clear_models();
		if (visuals_.is_valid()) {
			visuals_->drain_local_player_weapon_events();
		}
		if (weapon_effects_.is_valid()) {
			weapon_effects_->reset();
		}
		view_.unref();
		set_spectator_camera_active(false);
		return;
	}
	// The display frame's one camera compose; every other read of the view
	// (weapon-event placement, the mode refresh, the HUD) observes it.
	view_ = visuals_->present_local_player_view();
	if (is_local_spectator()) {
		// The first spectator frame starts at the last authoritative player
		// camera pose (or the spectator entity's initial pose on a fresh join).
		// Subsequent frames belong wholly to FlyCamera: never stamp them back
		// onto the hidden team-0 entity.
		if (!spectator_active_) {
			stamp_camera_pose();
		}
		set_spectator_camera_active(true);
		set_fly_camera_locked(false);
		input_router_.release_mouse_capture();
		clear_models();
		set_world_nvg_view(false, 0);
		feed_world_thermal_view(world(), false, false);
		visuals_->drain_local_player_weapon_events();
		if (weapon_effects_.is_valid()) {
			weapon_effects_->reset();
		}
		third_person_ = true;
		return;
	}
	set_spectator_camera_active(false);
	// The camera mode is the sim's resolved word (the arbiter ran this tick).
	third_person_ = view_.is_valid() &&
			opennova::world::presents_third_person(view_->get_third_person(), view_->get_camera_mode());
	set_world_nvg_view(view_.is_valid() && view_->get_nvg_visible(),
			view_.is_valid() ? view_->get_nvg_gain() : 0);
	feed_world_thermal_view(world(), view_.is_valid() && view_->get_thermal_view(),
			view_.is_valid() && view_->get_thermal_terrain_view());
	// Place the camera/viewmodel root for THIS tick before consuming one-shot
	// presentation events. On the first live tick the freshly built model is
	// still at its default transform; on later ticks it otherwise trails
	// movement/look by one frame. Pre-adopt the weapon snapshot so the
	// avatar's body channel remains current while update_player_camera()
	// stamps all visual roots.
	const Ref<PlayerWeaponView> weapon_view = visuals_->local_player_weapon_view();
	weapon_effects_->set_weapon_view(weapon_view);
	update_player_camera();
	// This leg runs after the session leg's entity rows, so the carrier hull
	// the display copies already carries this frame's stamped transform.
	feed_virtual_display(true);
	weapon_effects_->consume_pending(weapon_view);
}

void LocalPlayerPresenter::present_fixed_weapon_tick(const TypedArray<PlayerWeaponEvent> &p_events) {
	++fixed_weapon_batches_consumed_;
	if (!has_player() || is_local_spectator() || weapon_effects_.is_null()) {
		return;
	}
	const Ref<PlayerWeaponView> weapon_view = visuals_->local_player_weapon_view();
	if (p_events.is_empty()) {
		// Keep the active clip at this tick's exact pose, but defer the
		// expensive camera/avatar/viewmodel-root presentation to after the
		// catch-up batch.
		weapon_effects_->consume(weapon_view, p_events);
		return;
	}
	view_ = visuals_->local_player_view();
	third_person_ = view_.is_valid() &&
			opennova::world::presents_third_person(view_->get_third_person(), view_->get_camera_mode());
	weapon_effects_->set_weapon_view(weapon_view);
	update_player_camera();
	weapon_effects_->consume(weapon_view, p_events);
}

bool LocalPlayerPresenter::handle_key_input(const Ref<InputEvent> &p_event, bool p_active) {
	return input_router_.handle_key_input(p_event, p_active);
}

void LocalPlayerPresenter::consume_use_hold() {
	input_router_.consume_use_hold();
}

bool LocalPlayerPresenter::handle_input(const Ref<InputEvent> &p_event, bool p_active) {
	return input_router_.handle_input(p_event, p_active);
}

void LocalPlayerPresenter::reset_state() {
	third_person_ = false;
	// The debug override is mission-run state like the mode itself: a fresh
	// sim starts with it off, and the debug check reads this getter live.
	debug_third_person_ = false;
	if (weapon_effects_.is_valid()) {
		weapon_effects_->reset();
	}
	input_router_.reset();
	view_.unref();
	set_spectator_camera_active(false);
	set_world_nvg_view(false, 0);
	feed_world_thermal_view(world(), false, false);
	const Ref<Simulation> reset_sim = sim();
	if (reset_sim.is_valid()) {
		reset_sim->set_local_player_debug_third_person(false);
	}
}

void LocalPlayerPresenter::set_world_nvg_view(bool p_active, int p_gain) {
	if (visuals_.is_valid()) {
		visuals_->set_local_player_nvg_view(p_active, p_gain);
	}
}

bool LocalPlayerPresenter::has_player() const {
	Node *node = world();
	if (node == nullptr) {
		return false;
	}
	if (!static_cast<bool>(node->call("is_loaded"))) {
		return false;
	}
	const Ref<Simulation> player_sim = sim();
	return player_sim.is_valid() && player_sim->has_local_player();
}

bool LocalPlayerPresenter::is_local_spectator() const {
	const Ref<Simulation> spectator_sim = sim();
	return spectator_sim.is_valid() && spectator_sim->is_local_spectator();
}

void LocalPlayerPresenter::ensure_models() {
	if (world() == nullptr || visuals_.is_null()) {
		return;
	}
	if (avatar() == nullptr) {
		ObjectModel *built = visuals_->build_local_player_avatar();
		avatar_id_ = built != nullptr ? ObjectID(built->get_instance_id()) : ObjectID();
	}
	viewmodel_rig_->ensure_viewmodel();
}

void LocalPlayerPresenter::clear_models() {
	if (ObjectModel *weapon = held_weapon()) {
		weapon->queue_free();
	}
	held_weapon_id_ = ObjectID();
	held_weapon_graphic_ = String();
	if (ObjectModel *body = avatar()) {
		body->queue_free();
	}
	avatar_id_ = ObjectID();
	viewmodel_rig_->clear_viewmodel();
	// Every exit from live play (no player, a spectator, teardown) also ends
	// the vehicle's first-person display.
	feed_virtual_display(false);
	release_view_projection();
	Camera3D *cam = camera();
	if (cam != nullptr && camera_saved_fov_ > 0.0f) {
		cam->set_fov(camera_saved_fov_);
	}
	if (cam != nullptr && camera_saved_keep_aspect_ >= 0) {
		cam->set_keep_aspect_mode(static_cast<Camera3D::KeepAspect>(camera_saved_keep_aspect_));
	}
}

void LocalPlayerPresenter::set_fly_camera_locked(bool p_locked) {
	if (GameplayCamera *fly = fly_camera()) {
		fly->set_gameplay_locked(p_locked);
	}
}

void LocalPlayerPresenter::set_spectator_camera_active(bool p_active) {
	if (p_active == spectator_active_) {
		return;
	}
	spectator_active_ = p_active;
	if (GameplayCamera *fly = fly_camera()) {
		fly->set_spectator_mode(p_active);
	}
}

Vector2 LocalPlayerPresenter::aim_screen_point() const {
	if (!third_person_) {
		return kNoProjection; // 1P: the HUD pins the design center [orig: @0x5928a0]
	}
	Camera3D *cam = camera();
	if (world() == nullptr || cam == nullptr || !has_player()) {
		return kNoProjection;
	}
	const Vector2 angles = aim_angles_deg();
	const Ref<Simulation> aim_sim = sim();
	const Vector3 eye = eye_position(aim_sim.is_valid() ? aim_sim->get_local_player_position() : Vector3());
	const Vector3 target = Simulation::aim_ray_endpoint(eye, angles.x, angles.y);
	// Through the frame's projection (view_projection): the stretched target's
	// camera while it is live -- its pixels reach the surface through the
	// blit's stretch -- else the surface camera.
	Camera3D *through = projection_camera();
	SubViewport *target_viewport = projection_viewport();
	Viewport *surface = cam->get_viewport();
	if (through == nullptr || target_viewport == nullptr || surface == nullptr) {
		through = cam;
	}
	if (through->is_position_behind(target)) {
		return kNoProjection;
	}
	Vector2 point = through->unproject_position(target);
	if (through != cam) {
		const Vector2 target_size = Vector2(target_viewport->get_size());
		const Vector2 surface_size = surface->get_visible_rect().size;
		if (target_size.x > 0.0f && target_size.y > 0.0f) {
			point = Vector2(point.x * surface_size.x / target_size.x, point.y * surface_size.y / target_size.y);
		}
	}
	return point;
}

int LocalPlayerPresenter::aim_range_units() const {
	Node *node = world();
	if (node == nullptr || camera() == nullptr || !has_player()) {
		return 1;
	}
	const Ref<Simulation> range_sim = sim();
	const Vector3 pos = range_sim.is_valid() ? range_sim->get_local_player_position() : Vector3();
	const Vector3 eye = eye_position(pos);
	const Vector2 angles = aim_angles_deg();
	Vector3 endpoint = Simulation::aim_ray_endpoint(eye, angles.x, angles.y);
	// The terrain surface, through the ported retail raycast
	// (TerrainData.raycast_terrain -> engine/runtime/terrain_query/terrain_raycast.h
	// [orig: Terrain_RaycastHeightmapHiRes_0 @0x60e710]) rather than a Godot
	// physics query. The terrain heightfield was the only thing that query
	// could ever hit in the runtime -- object pick bodies are editor-only -- so
	// this measures the same surface, and it is now the SAME sampler the round
	// the player fires traces, so the readout and the bullet agree by
	// construction.
	const Ref<TerrainData> terrain = node->call("get_terrain_data");
	if (terrain.is_valid()) {
		const Vector3 hit = terrain->raycast_terrain(eye, endpoint);
		// A miss reports all-NAN.
		if (!(std::isnan(hit.x) || std::isnan(hit.y) || std::isnan(hit.z))) {
			endpoint = hit;
		}
	}
	return Simulation::rangefinder_units(pos, endpoint);
}

Vector2 LocalPlayerPresenter::aim_angles_deg() const {
	const Ref<Simulation> angle_sim = sim();
	float yaw = angle_sim.is_valid() ? angle_sim->get_local_player_yaw_deg() : 0.0f;
	float pitch = angle_sim.is_valid() ? angle_sim->get_local_player_pitch_deg() : 0.0f;
	if (view_.is_valid() && view_->get_binoculars_view_active()) {
		yaw += view_->get_binocular_yaw_offset_deg();
		pitch += view_->get_binocular_pitch_offset_deg();
	}
	return Vector2(yaw, pitch);
}

// The eye anchor: Position + CameraOffset, where the local player's
// CameraOffset is the POSED HEAD BONE minus Position -- sampled from the
// avatar's render skeleton, the same bone matrices the original builds
// sim-side. Stance, lean, the walk/run bob, and the jump arc all move the eye
// exactly as the animation moves the head.
// [orig: the local bone path @0x4b6bb3 (Entity_BuildBoneTransformMatrices ->
// head, CameraOffset = head - Position); consumed by the on-foot person leg
// @0x437f9c. The motor resolves it from the current simulation skeleton.]
Vector3 LocalPlayerPresenter::eye_position(const Vector3 &p_pos) const {
    const Ref<Simulation> eye_sim = sim();
    if (eye_sim.is_valid()) return p_pos + eye_sim->get_local_player_eye_offset();
    return p_pos + Vector3(0.0f, static_cast<float>(Simulation::player_non_person_eye_bump()), 0.0f);
}

Vector3 LocalPlayerPresenter::avatar_root_world() const {
	ObjectModel *body = avatar();
	if (body == nullptr) {
		return kNoSample;
	}
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(EntityPresenter::find_skeleton(body));
	if (skel == nullptr) {
		return kNoSample;
	}
	// The SKELETON's own origin - the exact frame avatar_head_world() expresses
	// the bone in, so head-minus-root is a pure body-relative delta. The avatar
	// node's transform is not it: for the first-person local player that node
	// sits at the world origin, which turned the delta into an absolute point
	// again.
	return skel->get_global_transform().origin;
}

Vector3 LocalPlayerPresenter::avatar_head_world() const {
	ObjectModel *body = avatar();
	if (body == nullptr) {
		return kNoSample;
	}
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(EntityPresenter::find_skeleton(body));
	const int head_bone = Simulation::player_head_bone_index();
	if (skel == nullptr || skel->get_bone_count() <= head_bone) {
		return kNoSample;
	}
	return skel->get_global_transform().xform(skel->get_bone_global_pose(head_bone).origin);
}

// The soldier's third-person gun: retail's draw 5. The model is the equipped
// weapon's gfx3 and it is drawn RIGID -- one matrix into every bone slot --
// so it carries no clip and no skeleton of its own; everything is the attach
// transform built here.
//
// Position is bone 16's own pivot, nudged, carried through that bone's posed
// matrix: the original's `M16 . (pivot16 + nudge)`, and since `M16 . pivot16`
// IS the joint world position, that reduces to joint + M16_rotation . nudge,
// which is what the pose gives us directly. Orientation is NOT bone 16's
// rotation and NOT one of the aim-overlay classes -- it is the weapon's own
// attach basis (see PlayerAimOverlay.weapon_attach_angles).
// [orig: draw @0x4e3c87..0x4e3d99; matrix build @0x4b2180..0x4b22f8; gate
//  Entity_CanFireWeapon @0x4dcb10]
void LocalPlayerPresenter::update_held_weapon(const Ref<PlayerAimOverlay> &p_overlay) {
	if (world() == nullptr || visuals_.is_null()) {
		return;
	}
	const Ref<PlayerViewmodelDef> def = visuals_->local_player_viewmodel_def();
	// 27 of the 94 shipped weapon rows author no gfx3 at all; drawing nothing
	// is the correct, retail behaviour there, not a missing asset.
	const String graphic = def.is_valid() ? def->get_gfx3() : String();
	if (graphic != held_weapon_graphic_) {
		if (ObjectModel *stale = held_weapon()) {
			stale->queue_free();
		}
		held_weapon_id_ = ObjectID();
		held_weapon_graphic_ = graphic;
		if (!graphic.is_empty()) {
			ObjectModel *built = visuals_->build_local_player_held_weapon(graphic);
			held_weapon_id_ = built != nullptr ? ObjectID(built->get_instance_id()) : ObjectID();
		}
	}
	ObjectModel *weapon = held_weapon();
	if (weapon == nullptr) {
		return;
	}
	if (p_overlay.is_null() || !p_overlay->get_weapon_visible()) {
		weapon->set_visible(false);
		return;
	}
	ObjectModel *body = avatar();
	const Variant attach = body != nullptr
			? EntityPresenter::held_weapon_attach_transform(body, p_overlay->get_weapon_attach_angles(),
					  p_overlay->get_weapon_hand_frame())
			: Variant();
	if (attach.get_type() == Variant::NIL) {
		weapon->set_visible(false);
		return;
	}
	// Gated like the avatar: an unchanged attach pose writes nothing.
	const Transform3D attach_transform = attach;
	if (weapon->get_global_transform() != attach_transform) {
		weapon->set_global_transform(attach_transform);
	}
	weapon->set_visible(true);
	// Same layer rule as the body: first person hides it from every camera by
	// LAYER while keeping it a shadow source (the witnessed mirror never draws
	// persons or their held weapons -- the reflection collects vehicles only
	// [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0]). The
	// model stores the decision and rewrites its instances on the edge only.
	const bool draw_held_weapon = third_person_ || debug_body_in_first_person_;
	weapon->set_presentation_layer(draw_held_weapon ? ObjectModel::PRESENTATION_LAYER_LOCAL_BODY
													: ObjectModel::PRESENTATION_LAYER_LOCAL_BODY_HIDDEN);
}

// Place the camera from the SIM-COMPOSED pose (world/player_view.h, S8): the
// witnessed composition -- the eye floor + 0.1875 pull-back, the
// recoil-doubled FP pitch, the torso+lean/4 roll, the chased-anchor pivot
// nudge and the march-landed third-person back-off -- runs in the engine per
// drain [orig: Camera_ComputeThirdPersonView @0x437d10 -- the on-foot person
// leg @0x437f9c..0x438031, the TP leg @0x438100..0x4383e2]; this presenter
// only converts the pose to the Godot frame and stamps the node. The mission
// yaw -> Godot forward mirrors the present remap (x,y,z)->(x,z,-y): a mission
// facing yaw faces (sin yaw, cos yaw) -> Godot (sin yaw, 0, -cos yaw), tilted
// by pitch.
void LocalPlayerPresenter::update_player_camera() {
	if (world() == nullptr || camera() == nullptr) {
		return;
	}
	// A fixed-tick switch can retire the viewmodel after the input pass built
	// it. Rebuild before this frame's placement/pose so the handoff never
	// reaches the renderer with a missing gun or an unplaced replacement.
	ensure_models();
	const Ref<Simulation> pose_sim = sim();
	const Vector3 pos = pose_sim.is_valid() ? pose_sim->get_local_player_position() : Vector3();
	stamp_camera_pose();
	update_scope_camera();
	update_avatar(pos);
	update_model_lighting_context();
	viewmodel_rig_->update_viewmodel(view_,
			weapon_effects_.is_valid() ? weapon_effects_->weapon_view() : Ref<PlayerWeaponView>(),
			third_person_, debug_force_viewmodel_);
}

void LocalPlayerPresenter::stamp_camera_pose() {
	Camera3D *cam = camera();
	if (cam == nullptr) {
		return;
	}
	if (view_.is_valid() && view_->get_camera_pose_valid()) {
		const Vector3 forward = Simulation::presentation_forward(view_->get_camera_yaw_deg(),
				view_->get_camera_pitch_deg());
		const Vector3 eye = view_->get_camera_eye();
		// Construct orientation independently of translation. eye + forward
		// rounds away low direction bits far from the origin (07TR's tank),
		// making a fixed aim jitter as the carrier translates.
		// docs/world/tank-parity-re.md (D-VEH-5).
		cam->set_global_transform(Transform3D(
				Basis::looking_at(forward, Vector3(0.0f, 1.0f, 0.0f)), eye));
		// The FP roll (torsoRoll + lean/4, composed in the sim; 0 in third
		// person). Sign pinned presenter-side: lean right (positive lean) tilts
		// the view right. [orig: @0x437fe6]
		if (Math::abs(view_->get_camera_roll_deg()) > 0.001f) {
			cam->rotate_object_local(Vector3(0.0f, 0.0f, -1.0f), Math::deg_to_rad(view_->get_camera_roll_deg()));
		}
	}
}

// The local player's lighting contexts: the body/held gun take the entity's
// sun-quality factor, the FP submit keeps effectScale=1, both key the
// interior group from blink_hits[0] (the engine's
// local_player_lighting_context, world/player_present.h, carries the
// witnesses). Updating on every presentation frame makes portal crossings
// live.
void LocalPlayerPresenter::update_model_lighting_context() {
	const Ref<Simulation> light_sim = sim();
	const int interior_item_id = light_sim.is_valid() ? light_sim->local_player_interior_item_id() : 0;
	float transfer = 0.0f;
	Node *node = world();
	const Ref<ItemDatabase> item_db = node != nullptr ? Ref<ItemDatabase>(node->call("get_item_db"))
														: Ref<ItemDatabase>();
	if (interior_item_id != 0 && item_db.is_valid()) {
		transfer = item_db->get_light_transfer(interior_item_id);
	}
	float body_scale = 1.0f;
	if (light_sim.is_valid()) {
		// quality -> effectScale maps engine-side (one owner:
		// renderer::sun_visibility_factor via sun_quality_factor).
		body_scale = light_sim->sun_quality_factor(light_sim->get_local_player_sun_quality());
	}
	const opennova::world::LocalPlayerLightingContext context =
			opennova::world::local_player_lighting_context(interior_item_id, transfer, body_scale);
	if (avatar()) avatar()->set_thermal_entity_wave(true);
	if (held_weapon()) held_weapon()->set_thermal_entity_wave(true);
	set_model_lighting_context(avatar(), context.interior, context.light_transfer, context.body_effect_scale);
	set_model_lighting_context(held_weapon(), context.interior, context.light_transfer,
			context.body_effect_scale);
	const TypedArray<ObjectModel> parts = vm_parts();
	for (int64_t i = 0; i < parts.size(); ++i) {
		set_model_lighting_context(Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i])),
				context.interior, context.light_transfer, context.fp_effect_scale);
	}
}

void LocalPlayerPresenter::set_model_lighting_context(ObjectModel *p_model, bool p_interior,
		float p_transfer, float p_effect_scale) {
	if (p_model != nullptr) {
		p_model->set_entity_lighting_context(p_effect_scale, p_interior, p_transfer);
	}
}

// The engine decides the swap (the claimant seat, the first-person camera mode
// and the def's authored display ride world::LocalPlayerViewFrame; the hull's
// present row is local-view suppressed the same frame). This presenter holds
// the frame's one composed view snapshot, so it is the feed; the entity
// presenter owns the vehicle's nodes and does the drawing.
void LocalPlayerPresenter::feed_virtual_display(bool p_live) {
	Node *node = world();
	MissionRoot *runtime = node != nullptr
			? Object::cast_to<MissionRoot>(static_cast<Object *>(node->call("get_runtime")))
			: nullptr;
	EntityPresenter *entities = runtime != nullptr ? runtime->get_entity_presenter() : nullptr;
	if (entities == nullptr) {
		return;
	}
	if (!p_live || view_.is_null() || !view_->get_virtual_display_active()) {
		entities->present_virtual_display(false, Simulation::INVALID_WIRE_HANDLE, String());
		return;
	}
	entities->present_virtual_display(true, view_->get_virtual_display_carrier(),
			view_->get_virtual_display_model());
}

// The ADS camera: the fov POLICY is sim state (80 base, 80/mag for sighted
// defs, eased by the 15-tick interp, suppressed in third person --
// engine/runtime/world player_view [orig: g_cameraFovTargetQ16 @0x26C6848;
// Player_ToggleWeaponScope @0x4df401; @0x4df3fa]); the frame's projection over
// the surface -- the mode-invariant horizontal fov, the vertical half-extent
// of the SELECTED ratio -- is the engine's world::view_projection [orig:
// @0x58d900]. The gameplay camera carries that frustum's CULLING SUPERSET: the
// frustum itself when the selected ratio is the surface's, otherwise the
// vertical fov under the surface's wider horizontal (a selected ratio taller
// than the surface) or the horizontal fov under the surface's taller vertical
// -- the terrain, foliage, particle and water-strip legs read this camera and
// must never clip what the projection target draws.
void LocalPlayerPresenter::update_scope_camera() {
	Camera3D *cam = camera();
	if (cam == nullptr || view_.is_null()) {
		return;
	}
	Viewport *viewport = cam->get_viewport();
	if (viewport == nullptr) {
		return;
	}
	const Vector2 size = viewport->get_visible_rect().size;
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return;
	}
	const Ref<Simulation> projection_sim = sim();
	const opennova::world::ViewProjection projection = opennova::world::view_projection(
			view_->get_fov_h_deg(),
			projection_sim.is_valid() ? projection_sim->get_local_player_aspect_mode() : -1,
			static_cast<int>(size.x), static_cast<int>(size.y));
	if (projection.scale_y < 1.0f) {
		cam->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
		cam->set_fov(projection.fov_h_deg);
	} else {
		cam->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
		cam->set_fov(projection.fov_v_deg);
	}
	// The world pass's near plane is 0.2 u every frame (retail re-pins it
	// beside the FOV; the far plane is floor(fog)+1, which rides the fog
	// owner) — Godot's 0.05 default rendered surfaces retail clips.
	// (engine witness: render-order-re.md, the Render_ProcessMainSceneFrame
	// per-frame depth pins)
	cam->set_near(0.2f);
	update_view_projection(projection);
}

// The stretched-mode target (view_projection): a SubViewport of the selected
// aspect under this presenter, sharing the surface's World3D, its own camera
// mirroring the gameplay camera at the mode-invariant horizontal fov, and a
// CanvasLayer beneath the root canvas blitting the target over the whole
// surface (whose own 3D draw is switched off meanwhile). Built on the first
// stretched frame, sized and mirrored every frame, released when the mode
// returns to the surface's ratio or the player goes away.
void LocalPlayerPresenter::update_view_projection(const opennova::world::ViewProjection &p_projection) {
	Camera3D *cam = camera();
	Viewport *surface = cam != nullptr ? cam->get_viewport() : nullptr;
	if (surface == nullptr || !is_inside_tree() ||
			Math::abs(p_projection.scale_y - 1.0f) < kViewProjectionUnstretched) {
		release_view_projection();
		return;
	}
	SubViewport *target = projection_viewport();
	Camera3D *through = projection_camera();
	if (target == nullptr || through == nullptr) {
		release_view_projection();
		target = memnew(SubViewport);
		target->set_name("ViewProjectionTarget");
		target->set_update_mode(SubViewport::UPDATE_ALWAYS);
		target->set_clear_mode(SubViewport::CLEAR_MODE_ALWAYS);
		target->set_disable_input(true);
		add_child(target);
		through = memnew(Camera3D);
		through->set_name("ViewProjectionCamera");
		through->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
		target->add_child(through);
		through->make_current();
		CanvasLayer *blit_layer = memnew(CanvasLayer);
		blit_layer->set_name("ViewProjectionBlit");
		blit_layer->set_layer(kViewProjectionBlitLayer);
		add_child(blit_layer);
		TextureRect *blit = memnew(TextureRect);
		blit->set_name("ViewProjectionImage");
		blit->set_texture(target->get_texture());
		blit->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		blit->set_stretch_mode(TextureRect::STRETCH_SCALE);
		blit->set_texture_filter(CanvasItem::TEXTURE_FILTER_LINEAR);
		blit->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		blit->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		blit_layer->add_child(blit);
		projection_viewport_id_ = ObjectID(target->get_instance_id());
		projection_camera_id_ = ObjectID(through->get_instance_id());
		projection_blit_layer_id_ = ObjectID(blit_layer->get_instance_id());
	}
	const Vector2i target_size(p_projection.target_w, p_projection.target_h);
	if (target->get_size() != target_size) {
		target->set_size(target_size);
	}
	mirror_viewport_quality(surface, target);
	// The gameplay pose and everything of the camera but the frustum.
	through->set_global_transform(cam->get_global_transform());
	through->set_fov(p_projection.fov_h_deg);
	through->set_near(cam->get_near());
	through->set_far(cam->get_far());
	through->set_cull_mask(cam->get_cull_mask());
	through->set_h_offset(cam->get_h_offset());
	through->set_v_offset(cam->get_v_offset());
	if (through->get_environment() != cam->get_environment()) {
		through->set_environment(cam->get_environment());
	}
	if (through->get_attributes() != cam->get_attributes()) {
		through->set_attributes(cam->get_attributes());
	}
	if (through->get_compositor() != cam->get_compositor()) {
		through->set_compositor(cam->get_compositor());
	}
	if (!surface->is_3d_disabled()) {
		surface->set_disable_3d(true);
		projection_surface_id_ = ObjectID(surface->get_instance_id());
	}
	projection_scale_y_ = p_projection.scale_y;
}

// Leaving the tree without a teardown (the shell freeing the presenter, a
// test's autofree) hands the surface its own 3D draw back: the target and
// the blit go with this node, the surface flag would not.
void LocalPlayerPresenter::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		release_view_projection();
	}
}

void LocalPlayerPresenter::release_view_projection() {
	if (Viewport *surface = Object::cast_to<Viewport>(ObjectDB::get_instance(projection_surface_id_))) {
		surface->set_disable_3d(false);
	}
	projection_surface_id_ = ObjectID();
	if (Node *blit_layer = Object::cast_to<Node>(ObjectDB::get_instance(projection_blit_layer_id_))) {
		blit_layer->queue_free();
	}
	if (Node *target = Object::cast_to<Node>(ObjectDB::get_instance(projection_viewport_id_))) {
		target->queue_free();
	}
	projection_blit_layer_id_ = ObjectID();
	projection_viewport_id_ = ObjectID();
	projection_camera_id_ = ObjectID();
	projection_scale_y_ = 1.0f;
}

void LocalPlayerPresenter::update_avatar(const Vector3 &p_pos) {
	ObjectModel *body = avatar();
	Node *node = world();
	if (body == nullptr || node == nullptr) {
		return;
	}
	// MATCHTERRAIN follows the simulation's exact MoveOrder stance latch. The
	// body stays submitted as a shadow source in first person, so keep this
	// independent of the camera-visible layer verdict below.
	const Ref<Simulation> stance_sim = sim();
	body->set_match_terrain_enabled(stance_sim.is_valid() &&
			stance_sim->get_local_player_stance_latch() != Simulation::STANCE_STAND);
	// The avatar node carries the BODY frame (the lagged body heading), not
	// the aim yaw: the aim/body split is what the per-segment overlay renders
	// as the torso twist, and the body-class delta is identity by construction
	// so the hips stay glued to the node.
	// [orig: Entity_BuildBoneTransformMatrices @0x4b1290 -- every overlay
	// blends toward bodyHeading/bodyPitch; docs/world/world-wac-ai-re.md §14
	// (D-INF-11)]
	MissionRoot *runtime = Object::cast_to<MissionRoot>(static_cast<Object *>(node->call("get_runtime")));
	const Ref<PlayerAimOverlay> overlay = runtime != nullptr ? runtime->local_player_aim_overlay()
															 : Ref<PlayerAimOverlay>();
	if (overlay.is_valid()) {
		const Basis body_basis = MissionObjectPlacer::bms_to_godot_basis(overlay->get_body_angles());
		set_avatar_transform(body, body->compose_entity_transform(body_basis, p_pos));
		const Basis inv = body_basis.inverse();
		Array deltas;
		const PackedVector3Array segments = overlay->get_segment_angles();
		for (int64_t i = 0; i < segments.size(); ++i) {
			deltas.push_back(inv * MissionObjectPlacer::bms_to_godot_basis(segments[i]));
		}
		body->set_aim_overlay(deltas);
	} else {
		const Ref<Simulation> yaw_sim = sim();
		const Basis body_basis = MissionObjectPlacer::bms_to_godot_basis(
				Vector3(0.0f, yaw_sim.is_valid() ? yaw_sim->get_local_player_yaw_deg() : 0.0f, 0.0f));
		set_avatar_transform(body, body->compose_entity_transform(body_basis, p_pos));
		body->set_aim_overlay(Array());
	}
	// The body renders only in third person; first person hides it from every
	// camera by LAYER, not by visible = false, so it stays a live shadow
	// source. The 2026-08-05 witness corrected the earlier mirror-visible
	// reading: retail's reflected entity waves collect only vehicles above
	// water, and the reflected world has no player-render leg, so no person --
	// the local body included -- ever enters the mirror [orig:
	// Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0 filterMask 0x400;
	// Entity_InitFromModel @ 0x40e20a; the viewmodel pass stays
	// Player_RenderFirstPersonViewModel @ 0x4ded60]. Retail's "not drawn" is a
	// skipped submit, not a state write: the model stores the policy and
	// rewrites its instances only when it changes (and inside its own
	// rebuild), never per frame.
	body->set_visible(true);
	const bool draw_avatar = third_person_ || debug_body_in_first_person_;
	body->set_presentation_layer(draw_avatar ? ObjectModel::PRESENTATION_LAYER_LOCAL_BODY
											 : ObjectModel::PRESENTATION_LAYER_LOCAL_BODY_HIDDEN);
	const Ref<Simulation> anim_sim = sim();
	const String anim_key = anim_sim.is_valid() ? anim_sim->get_local_player_anim_key() : String();
	const int anim_phase = anim_sim.is_valid() ? anim_sim->get_local_player_anim_phase_ticks() : 0;
	const String anim_source_key = anim_sim.is_valid() ? anim_sim->get_local_player_anim_source_key() : String();
	const int anim_source_phase = anim_sim.is_valid() ? anim_sim->get_local_player_anim_source_phase_ticks() : 0;
	const float anim_blend_weight = anim_sim.is_valid() ? anim_sim->get_local_player_anim_blend_weight() : 1.0f;
	// The upper-body weapon channel: the sim's secondary-channel clip (reload
	// etc.) posed at its own playhead onto the mask bones, composed under the
	// aim overlay. Equal state ids still carry the secondary playhead; an empty
	// key means the gate is off.
	// [orig: producer @0x4b5dad, override @0x4b14db; world-wac-ai-re.md §14.8]
	const Ref<PlayerWeaponView> weapon_view = weapon_effects_.is_valid() ? weapon_effects_->weapon_view()
																		 : Ref<PlayerWeaponView>();
	if (weapon_view.is_valid()) {
		body->set_weapon_channel(weapon_view->get_body_anim_key(), weapon_view->get_body_anim_phase(),
				weapon_view->get_body_anim_prev_key(), weapon_view->get_body_anim_prev_phase(),
				weapon_view->get_body_anim_blend_weight(), weapon_view->get_body_anim_variant(),
				weapon_view->get_body_anim_prev_variant());
	} else {
		body->set_weapon_channel(String(), 0);
	}
	if (!anim_source_key.is_empty() && !anim_key.is_empty() && anim_blend_weight < 1.0f) {
		body->play_body_blend_at(anim_source_key, anim_source_phase, anim_key, anim_phase, anim_blend_weight);
	} else if (!anim_key.is_empty()) {
		body->play_body_clip_at(anim_key, anim_phase);
	} else {
		body->play_body_anim_at(anim_sim.is_valid() ? anim_sim->get_local_player_body_anim_slot() : -1, anim_phase);
	}
	// Attach only after this frame's body clip and weapon layer have been posed.
	update_held_weapon(overlay);
}

void LocalPlayerPresenter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "world", "camera", "fly_camera", "controls"),
			&LocalPlayerPresenter::setup, DEFVAL(Variant()), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("teardown"), &LocalPlayerPresenter::teardown);
	ClassDB::bind_method(D_METHOD("refresh_viewmodel"), &LocalPlayerPresenter::refresh_viewmodel);
	ClassDB::bind_method(D_METHOD("viewmodel_generation"), &LocalPlayerPresenter::viewmodel_generation);
	ClassDB::bind_method(D_METHOD("set_input_override", "intent"), &LocalPlayerPresenter::set_input_override);
	ClassDB::bind_method(D_METHOD("set_third_person_selected", "selected"),
			&LocalPlayerPresenter::set_third_person_selected);
	ClassDB::bind_method(D_METHOD("set_debug_third_person", "enabled"),
			&LocalPlayerPresenter::set_debug_third_person);
	ClassDB::bind_method(D_METHOD("is_debug_third_person"), &LocalPlayerPresenter::is_debug_third_person);
	ClassDB::bind_method(D_METHOD("is_third_person"), &LocalPlayerPresenter::is_third_person);
	ClassDB::bind_method(D_METHOD("set_debug_force_viewmodel", "enabled"),
			&LocalPlayerPresenter::set_debug_force_viewmodel);
	ClassDB::bind_method(D_METHOD("is_debug_force_viewmodel"), &LocalPlayerPresenter::is_debug_force_viewmodel);
	ClassDB::bind_method(D_METHOD("set_debug_body_in_first_person", "enabled"),
			&LocalPlayerPresenter::set_debug_body_in_first_person);
	ClassDB::bind_method(D_METHOD("is_debug_body_in_first_person"),
			&LocalPlayerPresenter::is_debug_body_in_first_person);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_force_viewmodel"), "set_debug_force_viewmodel",
			"is_debug_force_viewmodel");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_body_in_first_person"), "set_debug_body_in_first_person",
			"is_debug_body_in_first_person");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_third_person"), "set_debug_third_person",
			"is_debug_third_person");
	ClassDB::bind_method(D_METHOD("set_fp_gun_visible", "visible"), &LocalPlayerPresenter::set_fp_gun_visible);
	ClassDB::bind_method(D_METHOD("avatar"), &LocalPlayerPresenter::avatar);
	ClassDB::bind_method(D_METHOD("vm_parts"), &LocalPlayerPresenter::vm_parts);
	ClassDB::bind_method(D_METHOD("viewmodel"), &LocalPlayerPresenter::viewmodel);
	ClassDB::bind_method(D_METHOD("held_weapon"), &LocalPlayerPresenter::held_weapon);
	ClassDB::bind_method(D_METHOD("camera"), &LocalPlayerPresenter::camera);
	ClassDB::bind_method(D_METHOD("view_projection"), &LocalPlayerPresenter::view_projection);
	ClassDB::bind_method(D_METHOD("presented_view"), &LocalPlayerPresenter::presented_view);
	ClassDB::bind_method(D_METHOD("projection_camera"), &LocalPlayerPresenter::projection_camera);
	ClassDB::bind_method(D_METHOD("projection_viewport"), &LocalPlayerPresenter::projection_viewport);
	ClassDB::bind_method(D_METHOD("projection_scale_y"), &LocalPlayerPresenter::projection_scale_y);
	ClassDB::bind_method(D_METHOD("viewmodel_rig"), &LocalPlayerPresenter::viewmodel_rig);
	ClassDB::bind_method(D_METHOD("set_viewmodel_capture_hidden", "hidden"),
			&LocalPlayerPresenter::set_viewmodel_capture_hidden);
	ClassDB::bind_method(D_METHOD("restamp_viewmodel_at_camera"),
			&LocalPlayerPresenter::restamp_viewmodel_at_camera);
	ClassDB::bind_method(D_METHOD("world"), &LocalPlayerPresenter::world);
	ClassDB::bind_method(D_METHOD("before_world_tick", "delta", "capture_mouse", "gameplay_input_active"),
			&LocalPlayerPresenter::before_world_tick, DEFVAL(false), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("after_world_tick"), &LocalPlayerPresenter::after_world_tick);
	ClassDB::bind_method(D_METHOD("fixed_weapon_batches_consumed"),
			&LocalPlayerPresenter::fixed_weapon_batches_consumed);
	ClassDB::bind_method(D_METHOD("handle_key_input", "event", "active"), &LocalPlayerPresenter::handle_key_input);
	ClassDB::bind_method(D_METHOD("consume_use_hold"), &LocalPlayerPresenter::consume_use_hold);
	ClassDB::bind_method(D_METHOD("handle_input", "event", "active"), &LocalPlayerPresenter::handle_input);
	ClassDB::bind_method(D_METHOD("has_player"), &LocalPlayerPresenter::has_player);
	ClassDB::bind_method(D_METHOD("is_local_spectator"), &LocalPlayerPresenter::is_local_spectator);
	ClassDB::bind_method(D_METHOD("aim_screen_point"), &LocalPlayerPresenter::aim_screen_point);
	ClassDB::bind_method(D_METHOD("aim_range_units"), &LocalPlayerPresenter::aim_range_units);
	ClassDB::bind_method(D_METHOD("avatar_root_world"), &LocalPlayerPresenter::avatar_root_world);
	ClassDB::bind_method(D_METHOD("avatar_head_world"), &LocalPlayerPresenter::avatar_head_world);
}
