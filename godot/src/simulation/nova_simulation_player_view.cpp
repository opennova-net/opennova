// Simulation — the LOCAL PLAYER view cluster: view effects (NVG /
// binoculars / scope), camera mode, and the composed view read.
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*
#include <npwire/ingame_message_id.h> // c2s:: mounted-weapon slot select on scope toggle
#include <world/vehicle_motor.h> // carrier_pose_fixed — the mounted camera's carrier read

#include <cmath>

using namespace novasim;

void Simulation::reset_local_player_view_effects() {
	player_view_.binoculars_requested = false;
	player_view_.binoculars_raised = false;
	player_view_.binoculars_view_active = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	player_view_.nvg_gain = opennova::world::kNvgGainMin;
	player_view_.nvg_active = world_ != nullptr &&
			(world_->mission_attrib_flags &
					static_cast<uint32_t>(
							opennova::bms::AttribFlags::StartWithNVGOn)) != 0;
	local_weapon_.nvg_scope_restore = false;
	refresh_local_player_view_effects();
}

void Simulation::refresh_local_player_view_effects() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	const bool alive = local != nullptr && local->alive && local->health > 0;
	const bool round_ended = world_ != nullptr && world_->round_end.ended;
	opennova::world::player_view_update_effective_modes(
			player_view_, alive, round_ended);
}

bool Simulation::request_local_player_scope_toggle() {
	// Action 6 first toggles the selected MountSlot on a designated-G carried
	// EWeap. This branch precedes ordinary scope FSM gates and waits for the
	// authoritative compact seat_type 1/2 echo before mutating local route state.
	// [orig: Input_HandleActionBinding_0 @0x4e0420, case 6 @0x4e0492]
	if (!local_weapon_.active) return false;
	opennova::world::Entity *player = world_ != nullptr
			? world_->registry.get(world_->cached.local_player) : nullptr;
	opennova::world::Entity *mount = player != nullptr && player->mounted &&
			player->use_gun_slot_swapped && player->mount_target.valid()
			? world_->registry.get(player->mount_target) : nullptr;
	if (mount != nullptr && mount->has_item_def && mount->item_type != 1u &&
			(mount->item_attrib & opennova::world::kItemAttribEweap) != 0u &&
			(mount->emplacement_attachment_flags & 0x02u) != 0u &&
			opennova::world::vehicle_prepare_weapon_slot(*world_, *mount)) {
		const bool use_parent_slot =
				!mount->primary_weapon_slot.redirect_to_parent_slot;
		opennova::world::Entity *parent = nullptr;
		bool route_valid = !use_parent_slot;
		if (use_parent_slot && mount->ground_target.valid() &&
				mount->emplacement_parent == mount->ground_target &&
				mount->emplacement_parent_spawn_id != 0) {
			parent = world_->registry.get(mount->ground_target);
			route_valid = parent != nullptr &&
					parent->registry_spawn_id ==
							mount->emplacement_parent_spawn_id &&
					parent->has_item_def && parent->item_type == 1u &&
					(parent->item_attrib &
							opennova::world::kItemAttribEweap) != 0u &&
					opennova::world::vehicle_prepare_weapon_slot(
							*world_, *parent);
		}
		if (route_valid) {
			opennova::MountedWeaponSlotSelection selection;
			selection.use_parent_slot = use_parent_slot;
			if (joiner_ && runtime_)
				return runtime_->queue_mounted_weapon_slot_selection(
						use_parent_slot);
			if (host_owner_.serve_and_play) {
				host_loop_.client_send(
						opennova::c2s::MOUNTED_WEAPON_SLOT_SELECT,
						opennova::encode_mounted_weapon_slot_selection(selection));
				return true;
			}
			// Standalone/tool worlds have no wire authority loop. Apply the same
			// validated transition directly.
			mount->primary_weapon_slot.redirect_to_parent_slot =
					use_parent_slot;
			player->equipped_adm_index = use_parent_slot
					? parent->primary_weapon_slot_adm
					: mount->primary_weapon_slot_adm;
			sync_local_usegun_weapon_transition();
			return true;
		}
	}

	// Ordinary scope: currentAction not in {RELOAD, SWITCHFROM}, then the
	// Player_ToggleWeaponScope view/definition gates.
	// [orig: Player_ToggleWeaponScope @0x4df0c0]
	opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	if (!opennova::world::weapon_fsm_scope_toggle_allowed(
			local_weapon_.def, *active_slot)) return false;
	// Scope-UP is refused while a movement key is held on a Scoped weapon
	// [orig: g_movementKeyHeld && (flags & 1) -> return @ 0x4df29c].
	if (!player_view_.scope_engaged &&
			opennova::world::player_view_scope_up_blocked(player_view_, local_weapon_.def.flags))
		return false;
	// Inset optics cannot be raised under NVG. Non-Inset sights retain the
	// original independent behavior.
	if (!player_view_.scope_engaged && player_view_.nvg_active &&
			(local_weapon_.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0)
		return false;
	// ForceScoped pins the raised sight: un-scoping is refused once settled
	// [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @ 0x4df12d].
	if (player_view_.scope_engaged && (local_weapon_.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0 &&
			!opennova::world::player_view_scope_ease_active(player_view_))
		return false;
	// The toggle latches this ease's step count (7 for Inset weapons, else 15;
	// 1 on the hipfire-return leg) and REFUSES while the previous ease runs
	// [orig: Player_ToggleWeaponScope @ 0x4df177 !activeFlag; Setup @ 0x4df1b3..0x4df36e].
	if (!opennova::world::player_view_set_engaged(player_view_, !player_view_.scope_engaged,
			(local_weapon_.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
		return false;
	if (player_view_.scope_engaged)
		opennova::world::weapon_fsm_queue_scope_up(*active_slot);
	else
		opennova::world::weapon_fsm_queue_scope_down(*active_slot);
	return true;
}

bool Simulation::request_local_player_binoculars_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;
	// Retail refuses binoculars while a PowerThrow charge is live. Allowing the
	// view to rise would suppress held weapon input and turn the charge into an
	// unintended release [orig: g_fireChargeStartTick @ 0xB76800; action 26 gate].
	if (local_weapon_.power_throw_start_tick != 0) return false;
	// An active scope also blocks binoculars in a gunner parent slot.
	if (player_view_.scope_engaged && local->mounted &&
			local->mount_type == opennova::world::SeatType::Gunner)
		return false;

	const bool requested =
			opennova::world::player_view_toggle_binoculars(player_view_);
	if (requested) {
		// The fixed-radius random aim displacement lives in the engine
		// (world/player_view.h kBinocularAimOffsetDeg + the sway helper);
		// this leg only samples the shell's RNG.
		const float unit = static_cast<float>(
				(static_cast<double>(std::rand()) + 0.5) /
				(static_cast<double>(RAND_MAX) + 1.0));
		opennova::world::player_view_binocular_sway_offset(unit,
				binocular_yaw_offset_deg_, binocular_pitch_offset_deg_);
	} else {
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
	}
	refresh_local_player_view_effects();
	return requested;
}

bool Simulation::request_local_player_nvg_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;

	if (!player_view_.nvg_active) {
		local_weapon_.nvg_scope_restore = false;
		if (local_weapon_.active && player_view_.scope_engaged &&
				(local_weapon_.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0 &&
				!opennova::world::player_view_scope_ease_active(player_view_)) {
			local_weapon_.nvg_scope_restore = request_local_player_scope_toggle();
		}
		return opennova::world::player_view_toggle_nvg(player_view_);
	}

	// Clear NVG before the normal scope-up request so the Inset refusal no
	// longer applies, then consume the one-shot restore latch.
	opennova::world::player_view_toggle_nvg(player_view_);
	const bool restore_scope = local_weapon_.nvg_scope_restore;
	local_weapon_.nvg_scope_restore = false;
	if (restore_scope && !player_view_.scope_engaged)
		request_local_player_scope_toggle();
	return false;
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

// One 62.5 Hz tick of the view state, before the weapon pump: the ADS ease and the
// third-person anchor chase run at the WORLD cadence, so camera lag is identical at
// any render rate. Retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call, so this tick's settle promoter is visible to
// action routing while an action's unscope/rescope begins easing on the next tick
// [orig: call sites @ 0x42c18e / @ 0x526786; promoter @ 0x4de4f7].
void Simulation::tick_local_player_view() {
	if (!world_ || !world_->cached.local_player.valid()) {
		// No seat without a player: the arbiter resolves to first person (or
		// the debug override) before the effective modes read the mode.
		player_view_.mount = opennova::world::MountedCameraInput();
		opennova::world::player_view_resolve_mode(player_view_);
		opennova::world::player_view_update_effective_modes(
				player_view_, false, world_ != nullptr && world_->round_end.ended);
		player_view_.tp_anchor_valid = false;
		return;
	}
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return;
	// The mounted camera's carrier read, refreshed every tick: only a CONTROL
	// seat (the retail parentSlot 2/5 test) takes the mounted leg, and the
	// carrier's pose/radius/class feed the chase target, the back-off and the
	// watercraft eye drop (retail: Camera_ComputeThirdPersonView @0x437D10 —
	// the +0x168 seat test, parentEntity +0x16C, boundRadius +0, the unitType
	// +0x196 in {3,4} test @0x43861D..0x43864C; see world/player_view.h). The
	// same seat test is the arbiter's (retail: Render_ProcessMainSceneFrame
	// @0x5ca1e2..0x5ca1f2), so the read precedes the mode resolve and the
	// effective-mode refresh below.
	opennova::world::MountedCameraInput mount;
	const opennova::world::Entity *carrier = e->mounted
			? world_->registry.get(e->mount_target)
			: nullptr;
	if (carrier != nullptr &&
			opennova::world::is_vehicle_control_seat(e->mount_type)) {
		int32_t pitch_bam = 0, roll_bam = 0;
		opennova::world::carrier_pose_fixed(*carrier, mount.carrier_pos_q16,
				mount.carrier_yaw_bam, pitch_bam, roll_bam);
		mount.control_seat = true;
		// The carrier's unit forward for the 6 u look-ahead. Retail takes the
		// chassis matrix's first column (parentMatrix +0xB4 x (6,0,0)); this seam
		// reads the carrier through carrier_pose_fixed, whose yaw is the one
		// attitude term every mover family stamps, so the forward is the
		// yaw-only form (sin yaw, cos yaw, 0) in mission space — the pitch/roll
		// fold of the full chassis matrix is not composed here (retail:
		// Camera_ComputeThirdPersonView @0x438811..0x4388b5, see
		// docs/world/world-wac-ai-re.md §14.6).
		const double forward_yaw_rad =
				opennova::world::mission_yaw_deg_from_bam_heading(
						mount.carrier_yaw_bam) * (3.14159265358979323846 / 180.0);
		mount.carrier_forward[0] = static_cast<float>(std::sin(forward_yaw_rad));
		mount.carrier_forward[1] = static_cast<float>(std::cos(forward_yaw_rad));
		mount.carrier_forward[2] = 0.0f;
		mount.bound_radius = carrier->bound_radius;
		mount.watercraft = carrier->item_unit_type == 3 ||
				carrier->item_unit_type == 4;
		mount.water_z = static_cast<float>(world_->env.water_z) / 65536.0f;
	}
	player_view_.mount = mount;
	opennova::world::player_view_resolve_mode(player_view_);
	refresh_local_player_view_effects();
	// The per-tick movement delta the FP motion lead samples per render frame
	// (retail: the (position - entity+0x80 prev-position) << 8 samples
	// @ 0x437bb2/0x437b92/0x437ba2 — world/player_view.h carries the witness).
	if (local_tick_prev_valid_) {
		local_tick_delta_[0] = e->position.x - local_tick_prev_pos_[0];
		local_tick_delta_[1] = e->position.y - local_tick_prev_pos_[1];
		local_tick_delta_[2] = e->position.z - local_tick_prev_pos_[2];
	}
	local_tick_prev_pos_[0] = e->position.x;
	local_tick_prev_pos_[1] = e->position.y;
	local_tick_prev_pos_[2] = e->position.z;
	local_tick_prev_valid_ = true;
	// The anchor-chase target is Position + CameraOffset — the posed head-bone eye
	// [orig: ThirdPersonCamera_Update @ 0x437b70..76], fed by the host's per-frame
	// skeleton sample (see local_weapon_.eye_mission). Without a sample: Position + 1.0,
	// the witnessed NON-person bump [orig: @ 0x437e8f].
	float eye[3] = {
		local_weapon_.eye_valid ? local_weapon_.eye_mission[0] : e->position.x,
		local_weapon_.eye_valid ? local_weapon_.eye_mission[1] : e->position.y,
		local_weapon_.eye_valid ? local_weapon_.eye_mission[2] : e->position.z + 1.0f,
	};
	// The chase target inherits the CameraOffset terrain floor: retail's
	// producer floors the head-bone eye before the store the chase reads
	// (D-INF-18; the witnessed walk lives in world::player_view_floor_eye_to_terrain).
	if (local_weapon_.eye_valid) {
		opennova::world::player_view_floor_eye_to_terrain(
				world_->ai != nullptr ? world_->ai->terrain : nullptr,
				(e->flags & opennova::world::kEntityFlagIndoors) != 0, eye);
	}
	opennova::world::player_view_tick(player_view_, eye);
}

void Simulation::set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid) {
	// Godot (x, y, z) -> mission (x, -z, y), the get_local_player_position inverse.
	local_weapon_.eye_mission[0] = p_eye_godot.x;
	local_weapon_.eye_mission[1] = -p_eye_godot.z;
	local_weapon_.eye_mission[2] = p_eye_godot.y;
	local_weapon_.eye_valid = p_valid;
	// Mirror into the world so the infantry body tick can restamp the local
	// eye-offset triple from the exact posed head (the D-HUD-20 local leg).
	if (world_) {
		world_->cached.local_head = opennova::world::Vec3{
				local_weapon_.eye_mission[0], local_weapon_.eye_mission[1],
				local_weapon_.eye_mission[2]};
		world_->cached.local_head_valid = p_valid;
	}
}

Dictionary Simulation::get_local_player_view() const {
	Dictionary out;
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	out["scope_engaged"] = player_view_.scope_engaged;
	out["binoculars_requested"] = player_view_.binoculars_requested;
	out["binoculars_raised"] = player_view_.binoculars_raised;
	out["binoculars_view_active"] = player_view_.binoculars_view_active;
	out["binocular_yaw_offset_deg"] = binocular_yaw_offset_deg_;
	out["binocular_pitch_offset_deg"] = binocular_pitch_offset_deg_;
	out["nvg_active"] = player_view_.nvg_active;
	out["nvg_visible"] =
			opennova::world::player_view_nvg_visible(player_view_);
	out["nvg_gain"] = player_view_.nvg_gain;
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	out["mounted"] = local != nullptr && local->mounted;
	// The RESOLVED camera mode and the chase preference behind it
	// [orig: g_camera_mode @ 0xA890C8; g_camera_third_person_selected @ 0xA860DF].
	out["third_person"] = player_view_.third_person;
	out["third_person_selected"] = player_view_.third_person_selected;
	// The camera's mounted leg is engaged: a control seat with a live carrier
	// (the per-tick carrier read in tick_local_player_view) AND the resolved
	// third person — the compose fork's own gate.
	out["camera_mounted"] = player_view_.mount.control_seat && player_view_.third_person;
	// Structural proxy for Player_IsVehicleHasAttackCapability until mounted
	// weapon inventory is modeled: these seat classes replace the on-foot
	// upper-body weapon channel; passenger seats do not.
	out["vehicle_attack_context"] = local != nullptr && mount_blocks_weapon_channel(*local);
	out["scope_fraction"] = opennova::world::player_view_scope_fraction(player_view_);
	// The NoCardSwitch reload rule: while the equipped slot is mid-RELOAD on a
	// weapon WITHOUT NoCardSwitch (flags 0x2000000), the FP camera drops the ADS
	// view bias for the frame — the shell reads the eased fraction as 0.
	// [orig: Player_UpdateFirstPersonCamera @ 0x4dd439/@ 0x4dd4cc; the same
	//  predicate is Player_IsReloadingCardSwitchWeapon @ 0x4dcdd0 (ex kong
	//  "Player_IsDriverInVehicle"), whose one caller refuses fire @ 0x5cf7be]
	out["suppress_view_bias"] = local_weapon_.active &&
			active_slot->current == opennova::world::weapon_action::kReload &&
			(local_weapon_.def.flags & opennova::world::weapon_flag::kNoCardSwitch) == 0;
	// On the supported on-foot first-person path, the standard SIGHTS card replaces
	// the FP viewmodel once ADS settles. Scoped and Sighted are asymmetric selectors;
	// NoCardSwitch clears both unless ForceScoped overrides it. The frame draws the
	// card or the FP viewmodel, never both. [orig: Render_ProcessMainSceneFrame
	// @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15; suppression @0x4dcce0]
	out["scope_card_active"] = local_weapon_.active &&
			opennova::world::weapon_sights_card_eligible(
					local_weapon_.def, *active_slot) &&
			player_view_.scope_engaged && !player_view_.third_person &&
			!player_view_.binoculars_view_active &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	out["fov_h_deg"] = opennova::world::player_view_fov_h_deg(player_view_,
			local_weapon_.active ? local_weapon_.def.flags : 0,
			local_weapon_.active ? local_weapon_.scope_max_mag : 0.0f);
	// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position map.
	out["tp_anchor"] = Vector3(player_view_.tp_anchor[0], player_view_.tp_anchor[2],
			-player_view_.tp_anchor[1]);
	out["tp_anchor_valid"] = player_view_.tp_anchor_valid;
	// The composed camera pose + its FP components — one native composition
	// (world/player_view.h, S8): the shell converts frames and stamps the
	// Camera3D node. The recoil doubling and torso+lean/4 roll stay exported
	// separately for diagnostics/probes; authoritative look pitch never
	// inherits the camera-only doubling.
	// [orig: Camera_ComputeThirdPersonView @0x437d10 — the on-foot person leg
	//  @0x437f9c..0x438031, the TP leg @0x438100..0x4383e2, recoil @0x437fc7,
	//  roll @0x437fe6]
	if (world_ && world_->ai && world_->cached.local_player.valid()) {
		if (const AiEntity *p =
					world_->ai->for_handle(world_->cached.local_player)) {
			const opennova::world::Entity *e =
					world_->registry.get(world_->cached.local_player);
			out["fp_pitch_recoil_deg"] =
					opennova::world::player_view_fp_pitch_recoil_deg(
							p->inf.recoil_pitch);
			out["fp_roll_deg"] = opennova::world::player_view_fp_roll_deg(
					p->inf.torso_roll, p->inf.lean_angle);
			if (e != nullptr) {
				// The aim angles the camera composes over: the presented look
				// getters' values, plus the binocular wander while its optical
				// view is up (the shell's former _aim_angles_deg).
				float aim_yaw = static_cast<float>(
						opennova::world::mission_yaw_deg_from_bam_heading(
								p->heading));
				float aim_pitch = static_cast<float>(
						static_cast<double>(p->pitch) *
						opennova::world::kDegreesPerBam);
				if (player_view_.binoculars_view_active) {
					aim_yaw += static_cast<float>(binocular_yaw_offset_deg_);
					aim_pitch += static_cast<float>(binocular_pitch_offset_deg_);
				}
				const float position[3] = {e->position.x, e->position.y,
						e->position.z};
				opennova::world::PlayerCameraPose pose;
				opennova::world::player_view_compose_camera(player_view_,
						position, local_weapon_.eye_mission,
						local_weapon_.eye_valid,
						world_->ai != nullptr ? world_->ai->terrain : nullptr,
						(e->flags & opennova::world::kEntityFlagIndoors) != 0,
						aim_yaw, aim_pitch,
						p->inf.recoil_pitch, p->inf.torso_roll,
						p->inf.lean_angle, pose);
				out["camera_pose_valid"] = true;
				// mission (x,y,z) -> Godot (x, z, -y).
				out["camera_eye"] = Vector3(pose.eye[0], pose.eye[2],
						-pose.eye[1]);
				out["camera_yaw_deg"] = pose.yaw_deg;
				out["camera_pitch_deg"] = pose.pitch_deg;
				out["camera_roll_deg"] = pose.roll_deg;
			}
		}
	}
	return out;
}

// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=forward,
// Y=left, Z=up): the raw weapon.def `pos`/`tpos` blend over the /256 scale
// with the NoCardSwitch reload suppression applied — the rig maps view axes
// onto its camera frame and parents the node (world/player_view.h, S8).
// [orig: Player_UpdateFirstPersonCamera @ 0x4dd380]
Vector3 Simulation::local_player_viewmodel_bias_view_units(
		const Vector3 &p_pos_raw_units, const Vector3 &p_tpos_raw_units,
		int p_viewport_w, int p_viewport_h) {
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	const bool suppress = local_weapon_.active &&
			active_slot->current == opennova::world::weapon_action::kReload &&
			(local_weapon_.def.flags &
					opennova::world::weapon_flag::kNoCardSwitch) == 0;
	const float pos[3] = {p_pos_raw_units.x, p_pos_raw_units.y,
			p_pos_raw_units.z};
	const float tpos[3] = {p_tpos_raw_units.x, p_tpos_raw_units.y,
			p_tpos_raw_units.z};
	float out[3];
	opennova::world::player_view_bias_view_units(player_view_, suppress, pos,
			tpos, out);
	// The per-frame motion lead: the witnessed pre-rotation add takes the
	// world-delta components RAW onto the view-frame lanes (no frame
	// conversion) (retail: @ 0x4dd549..0x4dd56c — see world/player_view.h).
	int32_t lead[3];
	opennova::world::player_view_motion_lead_update(fp_motion_lead_,
			local_tick_delta_, lead);
	for (int i = 0; i < 3; ++i)
		out[i] += static_cast<float>(lead[i]) / 65536.0f;
	// The 4:3 framing drop — the 3w<=4h rule is the engine's own
	// (retail: @ 0x4dd571..0x4dd578 — world/player_view.h
	// player_view_narrow_aspect); the caller only samples the viewport.
	if (opennova::world::player_view_narrow_aspect(p_viewport_w, p_viewport_h))
		out[2] -= static_cast<float>(
				opennova::world::kFpNarrowAspectDropQ16) / 65536.0f;
	return Vector3(out[0], out[1], out[2]);
}

float Simulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect) {
	return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, p_aspect);
}
