#include "player/player_viewmodel_rig.h"

#include "object/avatar_database.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "player/player_viewmodel_def.h"
#include "player/player_weapon_effects.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/world/player_present.h>
#include <runtime/world/player_view.h>

using namespace godot;

namespace {

constexpr const char *kCtrlOwnerFpHeat = "first_person:heat";
constexpr const char *kCtrlOwnerFpEmplaced = "first_person:emplaced";
constexpr const char *kCtrlOwnerFpTeam = "first_person:team";
constexpr const char *kCtrlOwnerFpArmsCamo = "first_person:arms_camo";
constexpr const char *kViewmodelProjectionGlobal = "opennova_viewmodel_projection";

} // namespace

PlayerViewmodelRig::PlayerViewmodelRig() {
	pos_units_ = Simulation::viewmodel_fallback_pos_units();
	tpos_units_ = Simulation::viewmodel_fallback_tpos_units();
	rot_ = Vector3(0.0f, Simulation::viewmodel_rig_yaw_deg(), 0.0f);
	rot_bias_def_ = Simulation::viewmodel_fallback_rot_bias_deg();
	renderfov_h_deg_ = Simulation::weapon_render_fov_h_deg_default();
	world_fov_h_deg_ = opennova::world::kPlayerCameraFovHDeg;
}

void PlayerViewmodelRig::set_fp_gun_visible(bool p_visible) {
	fp_gun_visible_ = p_visible;
}

void PlayerViewmodelRig::setup(Node *p_world, LocalPlayerPresenter *p_presenter, Camera3D *p_camera) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
	camera_id_ = p_camera != nullptr ? ObjectID(p_camera->get_instance_id()) : ObjectID();
}

void PlayerViewmodelRig::teardown() {
	clear_viewmodel();
	world_id_ = ObjectID();
	presenter_id_ = ObjectID();
	camera_id_ = ObjectID();
	capture_hidden_ = false;
	submit_viewmodel_ = false;
	// The projection feed is process-wide: leave the project default behind
	// like the lighting block does, so a preview or the next mission never
	// inherits this one's focal ratio.
	if (projection_feed_ != Vector4()) {
		const Dictionary shipped = ProjectSettings::get_singleton()->get_setting(
				String("shader_globals/") + String(kViewmodelProjectionGlobal), Dictionary());
		if (shipped.has("value")) {
			RenderingServer::get_singleton()->global_shader_parameter_set(
					StringName(kViewmodelProjectionGlobal), shipped["value"]);
		}
	}
	projection_feed_ = Vector4();
}

Node *PlayerViewmodelRig::world() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
}

LocalPlayerPresenter *PlayerViewmodelRig::presenter() const {
	return Object::cast_to<LocalPlayerPresenter>(ObjectDB::get_instance(presenter_id_));
}

// The sim, re-resolved per use: mission reloads free the runtime and its sim,
// so a cached reference would go stale (the presenter follows the same rule).
Ref<Simulation> PlayerViewmodelRig::sim() const {
	Node *node = world();
	if (node == nullptr) {
		return Ref<Simulation>();
	}
	return Ref<Simulation>(node->call("get_sim"));
}

TypedArray<ObjectModel> PlayerViewmodelRig::vm_parts() const {
	TypedArray<ObjectModel> out;
	for (const ObjectID &id : vm_parts_) {
		if (ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))) {
			out.push_back(part);
		}
	}
	return out;
}

Node3D *PlayerViewmodelRig::viewmodel() const {
	return Object::cast_to<Node3D>(ObjectDB::get_instance(viewmodel_id_));
}

Camera3D *PlayerViewmodelRig::camera() const {
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(camera_id_));
}

void PlayerViewmodelRig::set_capture_hidden(bool p_hidden) {
	capture_hidden_ = p_hidden;
	if (Node3D *node = viewmodel()) {
		node->set_visible(submit_viewmodel_ && !capture_hidden_);
	}
}

void PlayerViewmodelRig::ensure_viewmodel() {
	Node *node = world();
	if (node == nullptr) {
		return;
	}
	if (viewmodel() != nullptr) {
		return;
	}
	LocalPlayerPresenter *owner = presenter();
	const Ref<LocalPlayerVisuals> visuals = owner != nullptr ? owner->visuals() : Ref<LocalPlayerVisuals>();
	if (visuals.is_null()) {
		return;
	}
	Node3D *built = visuals->build_local_player_viewmodel();
	viewmodel_id_ = built != nullptr ? ObjectID(built->get_instance_id()) : ObjectID();
	if (built == nullptr) {
		return;
	}
	apply_viewmodel_def();
	vm_parts_.clear();
	const TypedArray<ObjectModel> parts = visuals->local_player_viewmodel_parts();
	for (int64_t i = 0; i < parts.size(); ++i) {
		if (ObjectModel *part = Object::cast_to<ObjectModel>(static_cast<Object *>(parts[i]))) {
			vm_parts_.push_back(ObjectID(part->get_instance_id()));
		}
	}
	// re-sync the clip serial: fresh parts replay the active clip
	if (PlayerWeaponEffects *effects = owner->weapon_effects()) {
		effects->reset_play_serial();
	}
}

void PlayerViewmodelRig::refresh_viewmodel() {
	clear_viewmodel();
	++generation_;
}

void PlayerViewmodelRig::clear_viewmodel() {
	if (Node3D *node = viewmodel()) {
		// queue_free is deferred: retire the old draw immediately, including
		// when a catch-up tick builds its replacement in the same frame.
		node->set_visible(false);
		node->queue_free();
	}
	viewmodel_id_ = ObjectID();
	vm_parts_.clear();
}

// The witnessed FP projection, fed to the object shaders as one global: the
// gun draws INSIDE the beauty pass (retail's "viewmodel first" into the same
// backbuffer) through the weapon renderfov -- a HORIZONTAL fov in degrees --
// with the near plane swapped to 0.05 [orig: Render_SwapProjectionNearZ
// @0x4dee29] and the depth range clamped to the nearest tenth [orig:
// Render_SetViewportDepth01 @0x58a7b0]. The renderfov and world frusta share
// the frame's vertical scale (the FP pass pushes the world pass's scaleY
// [orig: Render_SetViewAndProjectionMatrices @0x58d900, the FP call
// @0x4dee7f]), so the shader needs only the focal ratio between them: the
// engine's world::viewmodel_focal_ratio, the ratio of the horizontal
// half-tangents on both axes whatever the aspect mode or surface -- which is
// why the feed reads no camera projection (while the presenter's stretched
// target draws, the gameplay camera carries only a culling superset;
// LocalPlayerPresenter view_projection). shaders/viewmodel_pass.gdshaderinc
// applies it per flagged instance.
void PlayerViewmodelRig::update_viewmodel_projection() {
	Camera3D *cam = camera();
	if (cam == nullptr || !cam->is_inside_tree()) {
		return;
	}
	const float near = static_cast<float>(Simulation::viewmodel_pass_near_z());
	const float far = static_cast<float>(cam->get_far());
	const float focal_ratio = opennova::world::viewmodel_focal_ratio(world_fov_h_deg_, renderfov_h_deg_);
	const Vector4 feed(focal_ratio, near, far, 1.0f);
	if (feed == projection_feed_) {
		return;
	}
	projection_feed_ = feed;
	RenderingServer::get_singleton()->global_shader_parameter_set(
			StringName(kViewmodelProjectionGlobal), feed);
}

void PlayerViewmodelRig::update_viewmodel(const Ref<PlayerLocalView> &p_view,
		const Ref<PlayerWeaponView> &p_weapon_view, bool p_third_person, bool p_force_visible) {
	Node3D *node = viewmodel();
	Camera3D *cam = camera();
	if (node == nullptr || cam == nullptr) {
		return;
	}
	if (p_view.is_valid()) {
		world_fov_h_deg_ = p_view->get_fov_h_deg();
	}
	// The engine draws the FP model with the RAW VIEW MATRIX as its world
	// transform, i.e. the model lives in VIEW space [orig:
	// Player_RenderFirstPersonViewModel @0x4ded60]. So the viewmodel is
	// parented to the CAMERA transform (view-relative), NOT oriented in world
	// space -- a viewmodel must stay fixed to the view, not swing with the
	// aim. PLAYER_VIEWMODEL_ROT lays the model's forward down-range (and picks
	// the correct handedness so a right-handed weapon sits on the right);
	// PLAYER_VIEWMODEL_POS_UNITS is the weapon.def `pos` view offset.
	// camera x bias(def rot, about the eye in view axes) x axis map(rig -> camera).
	// [orig: Player_UpdateFirstPersonCamera @0x4dd380 adds Def.Bone.rot to the
	// view angles and rotates Def.Bone.pos into view orientation before adding
	// to the eye.]
	place_viewmodel_at_camera();
	// The FP overlay rides its own visual layer: the beauty camera admits it
	// and every mesh instance applies the renderfov projection + depth band
	// (retail's "viewmodel first" draw into the same backbuffer [orig:
	// Player_RenderFirstPersonViewModel @ 0x4ded60]); the mirror, Q3, and
	// capture cameras exclude the layer. The gameplay camera admits the world
	// shadow-caster marker layers; the viewmodel policy strips those markers
	// so the gun never leaks into world shadows. Both stamps are edge-gated on
	// the model (policy value / scene build serial), never per frame.
	for (const ObjectID &id : vm_parts_) {
		if (ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))) {
			part->set_presentation_layer(ObjectModel::PRESENTATION_LAYER_VIEWMODEL);
			part->set_viewmodel_pass(true);
		}
	}
	// The card switch, the showhud bit-0 gate and the SEAT gate compose the
	// retail submission decision (world/player_present.h
	// fp_viewmodel_retail_submit carries the witnesses); per-frame is what
	// makes MOUNTING take effect -- the HUD-init path only ran on the showhud
	// key.
	const bool carded = p_view.is_valid() && p_view->get_scope_card_active();
	const bool binoculars = p_view.is_valid() && p_view->get_binoculars_view_active();
	bool seat_hides_weapon = false;
	const Ref<Simulation> vm_sim = sim();
	if (vm_sim.is_valid()) {
		seat_hides_weapon = vm_sim->local_player_fp_weapon_hidden();
	}
	const bool retail_submit = opennova::world::fp_viewmodel_retail_submit(
			p_third_person, carded, binoculars, fp_gun_visible_, seat_hides_weapon);
	// The debug override intentionally extends retail's submission scope, but
	// a model made visible by that probe still needs a coherent CTRL snapshot.
	const bool submit_viewmodel = retail_submit || p_force_visible;
	submit_viewmodel_ = submit_viewmodel;
	node->set_visible(submit_viewmodel && !capture_hidden_);
	apply_viewmodel_control_registers(submit_viewmodel, p_weapon_view);
	update_viewmodel_projection();
}

// The gun's world pose is load-bearing for the beauty-pass fold (the shader
// folds the camera's view of the instance's WORLD transform), so the root
// sits at the camera every frame.
void PlayerViewmodelRig::place_viewmodel_at_camera() {
	Node3D *node = viewmodel();
	Camera3D *cam = camera();
	if (node == nullptr || cam == nullptr) {
		return;
	}
	// The def cant as camera euler radians: the engine's axis map
	// (simassets/fp_viewmodel_spec.h viewmodel_bias_euler_rad).
	const Basis bias = Basis::from_euler(Simulation::viewmodel_bias_euler_rad(rot_bias_def_));
	const Basis vm_basis = bias * Basis::from_euler(Vector3(
			Math::deg_to_rad(rot_.x), Math::deg_to_rad(rot_.y), Math::deg_to_rad(rot_.z)));
	// The ADS pos -> tpos blend, the /256 scale, and the NoCardSwitch reload
	// suppression run in the SIM (world/player_view.h player_view_bias_view_units,
	// S8) -- one blended VIEW-FRAME offset per frame; viewmodel_view_offset
	// maps the view axes onto Godot camera axes. Harness sim doubles implement
	// the same seam.
	// [orig: Player_UpdateFirstPersonCamera @0x4dd380 adds Bone(+0xF4) + the
	//  interp bias; the interp CNetPlayerInterp_Setup @0x4df36e runs +0x10C ->
	//  +0x124]
	const Ref<Simulation> bias_sim = sim();
	Vector3 view_offset = viewmodel_offset(pos_units_);
	if (bias_sim.is_valid()) {
		// Sampling the viewport size is this rig's device work; the
		// 4:3-or-narrower RULE the z drop keys on lives engine-side
		// (world/player_view.h player_view_narrow_aspect; retail: the viewport
		// block @0x4dd571). Out-of-tree (teardown frames) passes 0x0, which the
		// engine rule reads as narrow -- the same posture as the old 4:3 default.
		Vector2 vs;
		Node *world_node = world();
		if (world_node != nullptr && world_node->is_inside_tree()) {
			if (Viewport *viewport = world_node->get_viewport()) {
				vs = viewport->get_visible_rect().size;
			}
		}
		view_offset = viewmodel_view_offset(bias_sim->local_player_viewmodel_bias_view_units(
				pos_units_, tpos_units_, static_cast<int>(vs.x), static_cast<int>(vs.y)));
	}
	// An unchanged write would still dirty every part into the flush's
	// transform notifications; a still view writes nothing.
	const Transform3D next = cam->get_global_transform() * Transform3D(vm_basis, bias.xform(view_offset));
	if (node->get_global_transform() != next) {
		node->set_global_transform(next);
	}
}

void PlayerViewmodelRig::restamp_at_camera() {
	if (viewmodel() == nullptr || camera() == nullptr) {
		return;
	}
	place_viewmodel_at_camera();
	update_viewmodel_projection();
}

// The FP CTRL writers, per part and per frame: which of them execute is the
// engine's fp_ctrl_register_writes (world/player_present.h carries the
// TEX_TEAM / HEAT_GLOW / arms-camo witnesses); a writer that does not execute
// clears its registers for the frame.
void PlayerViewmodelRig::apply_viewmodel_control_registers(bool p_submit_viewmodel,
		const Ref<PlayerWeaponView> &p_weapon_view) {
	// setup()'s world contract already includes get_sim.
	const Ref<Simulation> ctrl_sim = sim();
	const PackedStringArray camo_registers = AvatarDatabase::part_camo_registers();
	for (const ObjectID &id : vm_parts_) {
		ObjectModel *visual = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id));
		if (visual == nullptr) {
			continue;
		}
		const bool arms_part = visual->get_avatar_part() == ObjectModel::AVATAR_PART_ARMS;
		const opennova::world::FpCtrlRegisterWrites writes = opennova::world::fp_ctrl_register_writes(
				p_submit_viewmodel, p_weapon_view.is_valid(),
				p_weapon_view.is_valid() && p_weapon_view->get_emplaced_controls_valid(), arms_part);
		visual->begin_ctrl_update();
		if (writes.team && ctrl_sim.is_valid()) {
			visual->set_ctrl_override(kCtrlOwnerFpTeam, "TEX_TEAM",
					Simulation::viewmodel_team_byte(ctrl_sim->get_local_player_team()));
		} else {
			visual->clear_ctrl_override(kCtrlOwnerFpTeam, "TEX_TEAM");
		}
		if (writes.heat) {
			visual->set_ctrl_override(kCtrlOwnerFpHeat, "HEAT_GLOW", p_weapon_view->get_heat_glow());
		} else {
			visual->clear_ctrl_override(kCtrlOwnerFpHeat, "HEAT_GLOW");
		}
		if (writes.emplaced) {
			visual->set_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNYAW",
					p_weapon_view->get_emplaced_gun_yaw());
			visual->set_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNPITCH",
					p_weapon_view->get_emplaced_gun_pitch());
		} else {
			visual->clear_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNYAW");
			visual->clear_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNPITCH");
		}
		if (arms_part) {
			if (writes.arms_camo) {
				AvatarDatabase::apply_part_camo(visual, visual->get_avatar_camo(), kCtrlOwnerFpArmsCamo);
			} else {
				for (int64_t i = 0; i < camo_registers.size(); ++i) {
					visual->clear_ctrl_override(kCtrlOwnerFpArmsCamo, camo_registers[i]);
				}
			}
		}
		visual->end_ctrl_update();
	}
}

// Pull the resolved weapon.def view record from the world; null when no
// weapon.def (or the weapon) resolves in the mounted root -- the witnessed
// JOX WPN_AK47AUTO constants above stay in force. The def rows carry xyz raw
// file units + yaw/pitch/roll degrees [orig: weapon.def 'pos'/'tpos' handlers
// @0x54471f; 'renderfov' @0x54482a, default 80.0].
void PlayerViewmodelRig::apply_viewmodel_def() {
	LocalPlayerPresenter *owner = presenter();
	const Ref<LocalPlayerVisuals> visuals = owner != nullptr ? owner->visuals() : Ref<LocalPlayerVisuals>();
	if (visuals.is_null()) {
		return;
	}
	const Ref<PlayerViewmodelDef> def = visuals->local_player_viewmodel_def();
	if (def.is_null()) {
		return;
	}
	pos_units_ = def->get_pos_units();
	rot_bias_def_ = def->get_rot_bias_deg();
	tpos_units_ = def->get_tpos_units();
	renderfov_h_deg_ = def->get_renderfov_h_deg();
	// flags / scope_max_mag / clipsize stay with the SIM (the weapon dict
	// feeds set_local_player_weapon): the ADS gates + fov policy run there
	// (ADR 0016).
}

// Map a VIEW-FRAME offset (world units, from the sim's blended bias) onto
// Godot camera-local axes. The view/def frame is X = FORWARD, Y = LEFT, Z =
// UP -- proven by the aim ray's far point being {+65536000, 0, 0} through the
// SAME transform [orig: HUD_DrawCrosshair @0x592a0f aim_direction = (1000.0,
// 0, 0) q16; the view-local rotate Math_FixedPointTransformPoint22
// @0x4dd5d8]. Godot camera-local is (x right, y up, -z forward), so:
//   view x (forward) -> Godot -z   (M4 tpos x -50.9 = ~0.2u BACK into the shoulder)
//   view y (left)    -> Godot -x
//   view z (up)      -> Godot  y   (e.g. MP5SD pos.z -183 -> grip ~0.715u below the eye)
// (The 2026-07-11 grill REFUTED the earlier x=right/y=forward reading: the
// AK's |x| ~= |y| masked the swap; the JOX/REVX M4 tpos made it glare -- the
// canted-ADS report. oscarmike's onhook-derived map agrees with the witnessed
// frame.) The velocity lead (>>7, clamps @0x4dd4f2..) and the prone Z drop
// (-1280 @0x4dd578) are recorded unported tails.
Vector3 PlayerViewmodelRig::viewmodel_view_offset(const Vector3 &p_view_units) const {
	return Simulation::viewmodel_camera_local_from_view(p_view_units);
}

// The raw-def-units fallback without a sim: the same axis map over the /256
// scale the sim's blend otherwise applies [orig: flt_7D1D70=256 @0x544770].
Vector3 PlayerViewmodelRig::viewmodel_offset(const Vector3 &p_units) const {
	return viewmodel_view_offset(p_units / static_cast<float>(Simulation::weapon_def_pos_scale()));
}

void PlayerViewmodelRig::_bind_methods() {
#define PLAYER_VIEWMODEL_RIG_TUNABLE(m_variant, m_property, m_accessor)                         \
	ClassDB::bind_method(D_METHOD("get_" #m_accessor), &PlayerViewmodelRig::get_##m_accessor);  \
	ClassDB::bind_method(D_METHOD("set_" #m_accessor, "value"), &PlayerViewmodelRig::set_##m_accessor); \
	ADD_PROPERTY(PropertyInfo(m_variant, m_property), "set_" #m_accessor, "get_" #m_accessor);
	PLAYER_VIEWMODEL_RIG_TUNABLE(Variant::VECTOR3, "PLAYER_VIEWMODEL_POS_UNITS", player_viewmodel_pos_units)
	PLAYER_VIEWMODEL_RIG_TUNABLE(Variant::VECTOR3, "PLAYER_VIEWMODEL_TPOS_UNITS", player_viewmodel_tpos_units)
	PLAYER_VIEWMODEL_RIG_TUNABLE(Variant::VECTOR3, "PLAYER_VIEWMODEL_ROT", player_viewmodel_rot)
	PLAYER_VIEWMODEL_RIG_TUNABLE(Variant::VECTOR3, "PLAYER_VIEWMODEL_ROT_BIAS_DEF", player_viewmodel_rot_bias_def)
	PLAYER_VIEWMODEL_RIG_TUNABLE(Variant::FLOAT, "PLAYER_VIEWMODEL_RENDERFOV_H_DEG", player_viewmodel_renderfov_h_deg)
#undef PLAYER_VIEWMODEL_RIG_TUNABLE
	ClassDB::bind_method(D_METHOD("set_fp_gun_visible", "visible"), &PlayerViewmodelRig::set_fp_gun_visible);
	ClassDB::bind_method(D_METHOD("setup", "world", "presenter", "camera"), &PlayerViewmodelRig::setup);
	ClassDB::bind_method(D_METHOD("teardown"), &PlayerViewmodelRig::teardown);
	ClassDB::bind_method(D_METHOD("vm_parts"), &PlayerViewmodelRig::vm_parts);
	ClassDB::bind_method(D_METHOD("viewmodel"), &PlayerViewmodelRig::viewmodel);
	ClassDB::bind_method(D_METHOD("camera"), &PlayerViewmodelRig::camera);
	ClassDB::bind_method(D_METHOD("set_capture_hidden", "hidden"), &PlayerViewmodelRig::set_capture_hidden);
	ClassDB::bind_method(D_METHOD("is_capture_hidden"), &PlayerViewmodelRig::is_capture_hidden);
	ClassDB::bind_method(D_METHOD("projection_feed"), &PlayerViewmodelRig::projection_feed);
	ClassDB::bind_method(D_METHOD("ensure_viewmodel"), &PlayerViewmodelRig::ensure_viewmodel);
	ClassDB::bind_method(D_METHOD("refresh_viewmodel"), &PlayerViewmodelRig::refresh_viewmodel);
	ClassDB::bind_method(D_METHOD("viewmodel_generation"), &PlayerViewmodelRig::viewmodel_generation);
	ClassDB::bind_method(D_METHOD("clear_viewmodel"), &PlayerViewmodelRig::clear_viewmodel);
	ClassDB::bind_method(D_METHOD("update_viewmodel", "view", "weapon_view", "third_person", "force_visible"),
			&PlayerViewmodelRig::update_viewmodel);
	ClassDB::bind_method(D_METHOD("restamp_at_camera"), &PlayerViewmodelRig::restamp_at_camera);
}
