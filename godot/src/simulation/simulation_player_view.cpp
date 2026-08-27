// Simulation — the LOCAL PLAYER view cluster's device face: Godot <-> mission
// frame conversion, the wire routing of a mount-slot selection, the session
// inputs the arbiter reads, and the Dictionary read. The orchestration and
// every witnessed gate live in <runtime/world/local_player_view.h>.
#include "simulation/simulation_internal.h"

#include <net/npwire/ingame_message_id.h> // c2s:: mounted-weapon slot select on scope toggle
#include <runtime/world/local_player_view.h>
#include <runtime/simassets/fp_viewmodel_spec.h>
#include <runtime/world/presentation_frame.h>

#include <cstdlib>

using namespace sim_internal;

void Simulation::reset_local_player_view_effects() {
	opennova::world::local_player_view_reset(world_.get(), local_weapon_, player_view_, view_tracker_);
}

void Simulation::refresh_local_player_view_effects() {
	opennova::world::local_player_view_refresh(world_.get(), player_view_);
}

bool Simulation::request_local_player_scope_toggle() {
	if (!local_weapon_.active || world_ == nullptr) return false;
	// Action 6 first toggles the selected MountSlot on a designated-G carried
	// EWeap; the engine validates the route, this leg only carries it: the joiner
	// queues it toward the authority, a serving host sends it to its own loopback
	// client, a standalone/tool world applies the same validated transition.
	opennova::world::MountSlotSelectRequest req;
	if (opennova::world::local_player_mount_slot_select(*world_, local_weapon_, req)) {
		if (joiner_ && runtime_)
			return runtime_->queue_mounted_weapon_slot_selection(req.use_parent_slot);
		if (host_owner_.serve_and_play) {
			opennova::MountedWeaponSlotSelection selection;
			selection.use_parent_slot = req.use_parent_slot;
			host_loop_.client_send(
					opennova::c2s::MOUNTED_WEAPON_SLOT_SELECT,
					opennova::encode_mounted_weapon_slot_selection(selection));
			return true;
		}
		opennova::world::local_player_apply_mount_slot_select(*world_, local_weapon_, req);
		return true;
	}
	opennova::world::WeaponSlotState *active_slot = active_local_weapon_slot();
	if (active_slot == nullptr) return false;
	return opennova::world::local_player_scope_toggle(local_weapon_, player_view_, *active_slot);
}

bool Simulation::request_local_player_binoculars_toggle() {
	if (world_ == nullptr) return false;
	// The raise's aim-displacement angle samples the process RNG, only on a raise.
	const auto unit_random = []() -> float {
		return static_cast<float>(
				(static_cast<double>(std::rand()) + 0.5) /
				(static_cast<double>(RAND_MAX) + 1.0));
	};
	return opennova::world::local_player_binoculars_toggle(
			*world_, local_weapon_, player_view_, view_tracker_, unit_random);
}

bool Simulation::request_local_player_nvg_toggle() {
	if (world_ == nullptr) return false;
	return opennova::world::local_player_nvg_toggle(
			*world_, local_weapon_, player_view_,
			[this]() { return request_local_player_scope_toggle(); });
}

int Simulation::request_local_player_nvg_gain(int p_delta) {
	return opennova::world::player_view_adjust_nvg_gain(player_view_, p_delta);
}

void Simulation::set_local_player_third_person_selected(bool p_selected) {
	// The preference re-resolves the mode at once (retail: the next frame's
	// arbiter; see world/player_view.h).
	opennova::world::player_view_set_third_person_selected(player_view_, p_selected);
	refresh_local_player_view_effects();
}

void Simulation::set_local_player_debug_third_person(bool p_enabled) {
	player_view_.debug_third_person_on_foot = p_enabled;
	opennova::world::player_view_resolve_mode(player_view_);
	refresh_local_player_view_effects();
}

bool Simulation::local_death_screen_active() const {
	// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
	// role's view folds (the listen host's own loopback included)
	// (retail: g_death_screen_active, NapiNPClientMsg_0x00A @0x42ff88..0x43002b, see netsim/client_state.h).
	return runtime_ != nullptr && runtime_->state().death_screen_active;
}

opennova::world::LocalViewSessionInputs Simulation::local_view_session_inputs() const {
	// What the arbiter reads from the session: the net layer sits above the
	// world group, so its client state crosses as plain values.
	opennova::world::LocalViewSessionInputs s;
	s.in_session = runtime_ != nullptr;
	s.joiner = joiner_;
	s.death_screen_active = local_death_screen_active();
	s.death_screen_submode = runtime_ != nullptr ? runtime_->state().death_screen_submode : 0;
	s.end_round_known = runtime_ != nullptr && runtime_->state().end_round.known;
	s.local_dead = local_player_dead();
	s.death_camera_target_known = runtime_ != nullptr;
	if (runtime_ != nullptr) {
		const opennova::netsim::ClientDeathCameraTarget &t = runtime_->state().death_camera;
		s.death_camera_target[0] = t.x;
		s.death_camera_target[1] = t.y;
		s.death_camera_target[2] = t.z;
	}
	return s;
}

// One 62.5 Hz tick of the view state, before the weapon pump (the order the
// world tick keeps: retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call).
void Simulation::tick_local_player_view() {
	opennova::world::local_player_view_tick(
			world_.get(), local_weapon_, player_view_, view_tracker_, local_view_session_inputs());
}

void Simulation::set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid) {
	// Godot (x, y, z) -> mission (x, -z, y), the get_local_player_position inverse.
	const float eye[3] = {p_eye_godot.x, -p_eye_godot.z, p_eye_godot.y};
	opennova::world::local_player_set_eye(world_.get(), local_weapon_, eye, p_valid);
}

void Simulation::set_local_player_eye_offset(const Vector3 &p_offset_godot, bool p_valid) {
	const float offset[3] = {p_offset_godot.x, -p_offset_godot.z, p_offset_godot.y};
	opennova::world::local_player_set_eye_offset(world_.get(), offset, p_valid);
}

Dictionary Simulation::get_local_player_view() const {
	opennova::world::LocalPlayerViewFrame f;
	opennova::world::local_player_view_frame(world_.get(), local_weapon_, player_view_, view_tracker_, f);
	Dictionary out;
	out["scope_engaged"] = f.scope_engaged;
	out["binoculars_requested"] = f.binoculars_requested;
	out["binoculars_raised"] = f.binoculars_raised;
	out["binoculars_view_active"] = f.binoculars_view_active;
	out["binocular_yaw_offset_deg"] = f.binocular_yaw_offset_deg;
	out["binocular_pitch_offset_deg"] = f.binocular_pitch_offset_deg;
	out["nvg_active"] = f.nvg_active;
	out["nvg_visible"] = f.nvg_visible;
	out["nvg_gain"] = f.nvg_gain;
	out["mounted"] = f.mounted;
	out["third_person"] = f.third_person;
	out["third_person_selected"] = f.third_person_selected;
	out["camera_mode"] = f.camera_mode;
	out["camera_mounted"] = f.camera_mounted;
	out["vehicle_attack_context"] = f.vehicle_attack_context;
	out["scope_fraction"] = f.scope_fraction;
	out["suppress_view_bias"] = f.suppress_view_bias;
	out["scope_card_active"] = f.scope_card_active;
	out["fov_h_deg"] = f.fov_h_deg;
	// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position map.
	out["tp_anchor"] = Vector3(f.tp_anchor[0], f.tp_anchor[2], -f.tp_anchor[1]);
	out["tp_anchor_valid"] = f.tp_anchor_valid;
	if (f.fp_terms_valid) {
		out["fp_pitch_recoil_deg"] = f.fp_pitch_recoil_deg;
		out["fp_roll_deg"] = f.fp_roll_deg;
	}
	if (f.camera_pose_valid) {
		out["camera_pose_valid"] = true;
		out["camera_eye"] = Vector3(f.camera.eye[0], f.camera.eye[2], -f.camera.eye[1]);
		out["camera_yaw_deg"] = f.camera.yaw_deg;
		out["camera_pitch_deg"] = f.camera.pitch_deg;
		out["camera_roll_deg"] = f.camera.roll_deg;
	}
	return out;
}

// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=forward,
// Y=left, Z=up); the rig maps view axes onto its camera frame and parents the
// node (world/player_view.h, S8).
Vector3 Simulation::local_player_viewmodel_bias_view_units(
		const Vector3 &p_pos_raw_units, const Vector3 &p_tpos_raw_units,
		int p_viewport_w, int p_viewport_h) {
	const float pos[3] = {p_pos_raw_units.x, p_pos_raw_units.y, p_pos_raw_units.z};
	const float tpos[3] = {p_tpos_raw_units.x, p_tpos_raw_units.y, p_tpos_raw_units.z};
	float out[3];
	opennova::world::local_player_viewmodel_bias(world_.get(), local_weapon_, player_view_,
			view_tracker_, pos, tpos, p_viewport_w, p_viewport_h, out);
	return Vector3(out[0], out[1], out[2]);
}

float Simulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect) {
	return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, p_aspect);
}

Vector3 Simulation::presentation_forward(float p_yaw_deg, float p_pitch_deg) {
	float f[3];
	opennova::world::presentation_forward_from_angles(p_yaw_deg, p_pitch_deg, f);
	return Vector3(f[0], f[1], f[2]);
}

Vector3 Simulation::aim_ray_endpoint(const Vector3 &p_eye, float p_yaw_deg, float p_pitch_deg) {
	const float eye[3] = {p_eye.x, p_eye.y, p_eye.z};
	float out[3];
	opennova::world::aim_ray_endpoint(eye, p_yaw_deg, p_pitch_deg,
			opennova::world::kAimProjectRange, out);
	return Vector3(out[0], out[1], out[2]);
}

int Simulation::rangefinder_units(const Vector3 &p_position, const Vector3 &p_endpoint) {
	const float pos[3] = {p_position.x, p_position.y, p_position.z};
	const float end[3] = {p_endpoint.x, p_endpoint.y, p_endpoint.z};
	return opennova::world::rangefinder_units(pos, end);
}

Vector3 Simulation::viewmodel_camera_local_from_view(const Vector3 &p_view_units) {
	const float view[3] = {p_view_units.x, p_view_units.y, p_view_units.z};
	float out[3];
	opennova::simassets::viewmodel_camera_local_from_view(view, out);
	return Vector3(out[0], out[1], out[2]);
}

Vector3 Simulation::viewmodel_bias_euler_rad(const Vector3 &p_rot_bias_deg) {
	const float bias[3] = {p_rot_bias_deg.x, p_rot_bias_deg.y, p_rot_bias_deg.z};
	float out[3];
	opennova::simassets::viewmodel_bias_euler_rad(bias, out);
	return Vector3(out[0], out[1], out[2]);
}

float Simulation::viewmodel_rig_yaw_deg() {
	return opennova::simassets::kViewmodelRigYawDeg;
}

float Simulation::weapon_render_fov_h_deg_default() {
	return opennova::simassets::kWeaponRenderFovHDegDefault;
}

int Simulation::viewmodel_team_byte(int p_team) {
	return opennova::simassets::viewmodel_team_byte(p_team);
}
