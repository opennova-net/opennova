// Simulation — the LOCAL PLAYER view cluster's device face: Godot <-> mission
// frame conversion, the wire routing of a mount-slot selection, the session
// inputs the arbiter reads, and the Dictionary read. The orchestration and
// every witnessed gate live in <runtime/world/local_player_view.h>.
#include "simulation/simulation_internal.h"
#include "simulation/player_local_view.h"
#include "util/axes.h"

#include <net/npwire/ingame_message_id.h> // c2s:: mounted-weapon slot select on scope toggle
#include <runtime/world/local_player_view.h>
#include <runtime/renderer/aspect_ratio.h>
#include <runtime/renderer/fp_viewmodel_spec.h>
#include <runtime/world/presentation_frame.h>

#include <cstdlib>

using namespace sim_internal;

void Simulation::reset_local_player_view_effects() {
	opennova::world::local_player_view_reset(&kernel_->world, kernel_->local.weapon, kernel_->local.view, kernel_->local.view_tracker);
}

void Simulation::refresh_local_player_view_effects() {
	opennova::world::local_player_view_refresh(&kernel_->world, kernel_->local.view);
}

void Simulation::set_local_player_aspect_mode(int p_mode) {
    if (kernel_) kernel_->local.aspect_mode = p_mode;
}

int Simulation::get_local_player_aspect_mode() const {
    return kernel_ ? kernel_->local.aspect_mode : -1;
}

bool Simulation::request_local_player_scope_zero(int p_delta) {
    return kernel_ != nullptr && kernel_->local.request_scope_zero(p_delta);
}

bool Simulation::request_local_player_scope_toggle() {
	if (!kernel_->local.weapon.active || kernel_ == nullptr) return false;
	// Action 6 first toggles the selected MountSlot on a designated-G carried
	// EWeap; the engine validates the route, this leg only carries it: the joiner
	// queues it toward the authority, a serving host sends it to its own loopback
	// client, a standalone/tool world applies the same validated transition.
	opennova::world::MountSlotSelectRequest req;
	if (opennova::world::local_player_mount_slot_select(kernel_->world, kernel_->local.weapon, req)) {
		if (is_joiner() && runtime_)
			return runtime_->queue_mounted_weapon_slot_selection(req.use_parent_slot);
		if (opennova::inmatch::ListenHostState *host = host_state();
				host != nullptr && host->host_owner.serve_and_play) {
			opennova::MountedWeaponSlotSelection selection;
			selection.use_parent_slot = req.use_parent_slot;
			host->host_loop.client_send(
					opennova::c2s::MOUNTED_WEAPON_SLOT_SELECT,
					opennova::encode_mounted_weapon_slot_selection(selection));
			return true;
		}
		opennova::world::local_player_apply_mount_slot_select(kernel_->world, kernel_->local.weapon, req, kernel_->local.view);
		return true;
	}
	opennova::world::WeaponSlotState *active_slot = active_local_weapon_slot();
	if (active_slot == nullptr) return false;
	return opennova::world::local_player_scope_toggle(kernel_->world, kernel_->local.weapon, kernel_->local.view, *active_slot);
}

bool Simulation::request_local_player_binoculars_toggle() {
	if (kernel_ == nullptr) return false;
	// The raise's aim-displacement angle is no longer sampled here: retail seeds
	// it once per ACTIVATION from the render frame, off the mission PRNG the
	// engine owns, so the engine's view tick draws it.
	return opennova::world::local_player_binoculars_toggle(
			kernel_->world, kernel_->local.weapon, kernel_->local.view, kernel_->local.view_tracker);
}

bool Simulation::request_local_player_nvg_toggle() {
	if (kernel_ == nullptr) return false;
	return opennova::world::local_player_nvg_toggle(
			kernel_->world, kernel_->local.weapon, kernel_->local.view,
			[this]() { return request_local_player_scope_toggle(); });
}

int Simulation::request_local_player_nvg_gain(int p_delta) {
	return opennova::world::player_view_adjust_nvg_gain(kernel_->local.view, p_delta);
}

void Simulation::set_local_player_third_person_selected(bool p_selected) {
	// The preference re-resolves the mode at once [orig: the next frame's
	// arbiter; see world/player_view.h].
	opennova::world::player_view_set_third_person_selected(kernel_->local.view, p_selected);
	// The BMS input-action word's view bits (world::ScriptState::input_action_bits,
	// the Input_HandleActionBinding producers the cat-7 player triggers read):
	// viewchase (402) |= 0x8000000, view1st (400) |= 0x4000000. The authority's
	// own player only: the evaluator runs the host's chains and a joiner never
	// opens the .bms. This seam cannot tell viewwithgun (401, |= 0x10000000) or
	// the 412 toggle apart from the plain selection; they read as the chase /
	// first-person selection they resolve to.
	if (!is_joiner()) {
		kernel_->world.script.input_action_bits |= p_selected ? 0x8000000u : 0x4000000u;
	}
	refresh_local_player_view_effects();
}

int Simulation::debug_input_action_bits() const {
	return kernel_ ? static_cast<int>(kernel_->world.script.input_action_bits) : 0;
}

void Simulation::set_local_player_debug_third_person(bool p_enabled) {
	kernel_->local.view.debug_third_person_on_foot = p_enabled;
	opennova::world::player_view_resolve_mode(kernel_->local.view);
	refresh_local_player_view_effects();
}

bool Simulation::local_death_screen_active() const {
	// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
	// role's view folds (the listen host's own loopback included)
	// [orig: g_death_screen_active, NapiNPClientMsg_0x00A @0x42ff88..0x43002b, see netsim/client_state.h].
	return runtime_ != nullptr && runtime_->state().death_screen_active;
}

Ref<PlayerLocalView> Simulation::get_local_player_view() const {
	Ref<PlayerLocalView> out;
	out.instantiate();
	out->assign(kernel_->local.view_frame());
	return out;
}

Vector3 Simulation::local_player_viewmodel_rotation_bias_deg() const {
	int32_t bias[3];
	opennova::world::local_player_viewmodel_rotation_bias(&kernel_->world,
			kernel_->local.weapon, kernel_->local.view, bias);
	constexpr double degrees_per_bam = 360.0 / 4294967296.0;
	return Vector3(bias[0] * degrees_per_bam, bias[1] * degrees_per_bam,
			bias[2] * degrees_per_bam);
}

// The FP viewmodel view-offset in VIEW-FRAME world units (X=forward, Y=left,
// Z=up); the rig maps view axes onto its camera frame and parents the node
// (world/player_view.h, S8). The ADS endpoint is the bound pose in the sim,
// so `p_tpos_raw_units` is not consumed; it stays on the binding only until
// the Simulation seam drops it.
Vector3 Simulation::local_player_viewmodel_bias_view_units(
		const Vector3 &p_pos_raw_units, const Vector3 &,
		int p_viewport_w, int p_viewport_h) {
	const float pos[3] = {p_pos_raw_units.x, p_pos_raw_units.y, p_pos_raw_units.z};
	float out[3];
	opennova::world::local_player_viewmodel_bias(&kernel_->world, kernel_->local.weapon, kernel_->local.view,
			kernel_->local.view_tracker, pos, p_viewport_w, p_viewport_h, out);
	return Vector3(out[0], out[1], out[2]);
}

float Simulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect, int p_mode) {
    const float selected = opennova::renderer::aspect_height_over_width(p_mode, p_aspect, 1.0f);
    return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, 1.0f / selected);
}

int Simulation::fresh_profile_aspect_mode(int p_width, int p_height) {
	return opennova::renderer::fresh_profile_aspect_mode(p_width, p_height);
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
	opennova::renderer::viewmodel_camera_local_from_view(view, out);
	return Vector3(out[0], out[1], out[2]);
}

Vector3 Simulation::viewmodel_bias_euler_rad(const Vector3 &p_rot_bias_deg) {
	const float bias[3] = {p_rot_bias_deg.x, p_rot_bias_deg.y, p_rot_bias_deg.z};
	float out[3];
	opennova::renderer::viewmodel_bias_euler_rad(bias, out);
	return Vector3(out[0], out[1], out[2]);
}

float Simulation::viewmodel_rig_yaw_deg() {
	return opennova::renderer::kViewmodelRigYawDeg;
}

float Simulation::weapon_render_fov_h_deg_default() {
	return opennova::renderer::kWeaponRenderFovHDegDefault;
}

int Simulation::viewmodel_team_byte(int p_team) {
	return opennova::renderer::viewmodel_team_byte(p_team);
}
