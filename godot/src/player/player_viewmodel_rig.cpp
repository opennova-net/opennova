#include "player/player_viewmodel_rig.h"

#include "object/avatar_database.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "player/player_viewmodel_def.h"
#include "player/player_weapon_effects.h"
#include "world/game_world_access.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/player_present.h>
#include <runtime/world/player_view.h>

using namespace godot;

namespace {

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
	return world_access::sim(world());
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

// The engine's FP projection (world::viewmodel_projection_feed carries the
// witness), fed to the object shaders as one global: the gun draws INSIDE the
// beauty pass through it. The renderfov and world frusta share the frame's
// vertical scale, so the shader needs only the focal ratio between them,
// which is why the feed reads no camera projection (while the presenter's
// stretched target draws, the gameplay camera carries only a culling superset;
// LocalPlayerPresenter view_projection). shaders/viewmodel_pass.gdshaderinc
// applies it per flagged instance.
void PlayerViewmodelRig::update_viewmodel_projection() {
	Camera3D *cam = camera();
	if (cam == nullptr || !cam->is_inside_tree()) {
		return;
	}
	const opennova::world::ViewmodelProjectionFeed projection = opennova::world::viewmodel_projection_feed(
			world_fov_h_deg_, renderfov_h_deg_, static_cast<float>(cam->get_far()));
	const Vector4 feed(projection.focal_ratio, projection.near_z, projection.far_z, 1.0f);
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
	// The model lives in VIEW space, parented to the camera transform, never
	// oriented in world space (renderer::fp_viewmodel_root carries the witness).
	place_viewmodel_at_camera();
	// The FP overlay rides its own visual layer: the beauty camera admits it
	// and every mesh instance applies the renderfov projection + depth band
	// (world::viewmodel_projection_feed); the mirror and capture cameras
	// exclude the layer; Q3 admits it. The gameplay camera admits the world
	// shadow-caster marker layers; the viewmodel policy strips those markers
	// so the gun never leaks into world shadows. Both stamps are edge-gated on
	// the model (policy value / scene build serial), never per frame.
	for (const ObjectID &id : vm_parts_) {
		if (ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))) {
			part->set_presentation_layer(ObjectModel::PRESENTATION_LAYER_VIEWMODEL);
			part->set_viewmodel_pass(true);
			part->set_viewmodel_rig(true);
		}
	}
	// The alive gate, the card switch, the showhud bit-0 gate (skipped by an
	// Emplaced def), the SEAT gate and the scoped Inset skip compose the
	// retail submission decision (world/player_present.h
	// fp_viewmodel_retail_submit carries the witnesses); per-frame is what
	// makes MOUNTING take effect -- the HUD-init path only ran on the showhud
	// key.
	opennova::world::FpViewmodelSubmitGates gates;
	gates.third_person = p_third_person;
	gates.fp_weapon_view_flag = fp_gun_visible_;
	if (p_view.is_valid()) {
		gates.scope_card_active = p_view->get_scope_card_active();
		gates.binoculars_view_active = p_view->get_binoculars_view_active();
		gates.local_dead = p_view->get_fp_local_dead();
		gates.round_winner_set = p_view->get_fp_round_winner_set();
		gates.emplaced = p_view->get_fp_def_emplaced();
		gates.inset_scoped = p_view->get_fp_inset_scoped();
	}
	const Ref<Simulation> vm_sim = sim();
	if (vm_sim.is_valid()) {
		gates.seat_hides_weapon = vm_sim->local_player_fp_weapon_hidden();
	}
	const bool retail_submit = opennova::world::fp_viewmodel_retail_submit(gates);
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
	// The root is the engine's (renderer::fp_viewmodel_root over the sim's
	// blended view bias and authored ADS rotation; harness sim doubles serve the
	// same seam), which the editor's first-person eye places its viewmodel by
	// too. Sampling the viewport size is this rig's device work; the
	// 4:3-or-narrower RULE the bias's z drop keys on lives engine-side
	// (world::player_view_narrow_aspect). Out-of-tree (teardown frames) passes
	// 0x0, which the engine rule reads as narrow.
	const Ref<Simulation> bias_sim = sim();
	Vector3 sim_rotation;
	Vector3 sim_units;
	if (bias_sim.is_valid()) {
		sim_rotation = bias_sim->local_player_viewmodel_rotation_bias_deg();
		Vector2 vs;
		Node *world_node = world();
		if (world_node != nullptr && world_node->is_inside_tree()) {
			if (Viewport *viewport = world_node->get_viewport()) {
				vs = viewport->get_visible_rect().size;
			}
		}
		sim_units = bias_sim->local_player_viewmodel_bias_view_units(
				pos_units_, tpos_units_, static_cast<int>(vs.x), static_cast<int>(vs.y));
	}
	const float pos[3] = { pos_units_.x, pos_units_.y, pos_units_.z };
	const float def_rot[3] = { rot_bias_def_.x, rot_bias_def_.y, rot_bias_def_.z };
	const float rig_deg[3] = { rot_.x, rot_.y, rot_.z };
	const float sim_view[3] = { sim_units.x, sim_units.y, sim_units.z };
	const float sim_rot[3] = { sim_rotation.x, sim_rotation.y, sim_rotation.z };
	const bool has_sim = bias_sim.is_valid();
	const opennova::renderer::FpViewmodelPose pose = opennova::renderer::fp_viewmodel_root(
			pos, def_rot, rig_deg, has_sim ? sim_view : nullptr, has_sim ? sim_rot : nullptr);
	const Basis vm_basis(pose.basis[0], pose.basis[1], pose.basis[2], pose.basis[3], pose.basis[4], pose.basis[5],
			pose.basis[6], pose.basis[7], pose.basis[8]);
	// An unchanged write would still dirty every part into the flush's
	// transform notifications; a still view writes nothing.
	const Transform3D next = cam->get_global_transform() *
			Transform3D(vm_basis, Vector3(pose.origin[0], pose.origin[1], pose.origin[2]));
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

void PlayerViewmodelRig::write_fp_team(ObjectModel &p_part, bool p_write, int p_team_byte) {
	if (p_write) {
		p_part.set_ctrl_override(kCtrlOwnerFpTeam, "TEX_TEAM", p_team_byte);
	} else {
		p_part.clear_ctrl_override(kCtrlOwnerFpTeam, "TEX_TEAM");
	}
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
		const bool team = writes.team && ctrl_sim.is_valid();
		write_fp_team(*visual, team,
				team ? Simulation::viewmodel_team_byte(ctrl_sim->get_local_player_team()) : 0);
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
			visual->set_ctrl_override(kCtrlOwnerFpEmplaced, "WEAP_SPIN",
					p_weapon_view->get_emplaced_spin_phase());
		} else {
			visual->clear_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNYAW");
			visual->clear_ctrl_override(kCtrlOwnerFpEmplaced, "EWEAP_GUNPITCH");
			visual->clear_ctrl_override(kCtrlOwnerFpEmplaced, "WEAP_SPIN");
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
// equipped weapon resolves in the mounted root -- nothing draws then, and the
// placement values stay as they were (renderer::FpViewmodelDef carries the
// field witnesses).
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
	ClassDB::bind_method(D_METHOD("is_capture_hidden"), &PlayerViewmodelRig::is_capture_hidden);
	ClassDB::bind_method(D_METHOD("projection_feed"), &PlayerViewmodelRig::projection_feed);
	ClassDB::bind_method(D_METHOD("refresh_viewmodel"), &PlayerViewmodelRig::refresh_viewmodel);
	ClassDB::bind_method(D_METHOD("viewmodel_generation"), &PlayerViewmodelRig::viewmodel_generation);
}
