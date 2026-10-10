#include <runtime/world/local_player.h>

#include <base/io/bam.h>
#include <base/io/log.h>
#include <runtime/audio/oneshot_play.h> // listener view flags
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_present.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

using namespace opennova::def;

namespace opennova::world {

namespace w = opennova::world;

bool LocalPlayer::has_local_player() const {
	const World &world = world_;
	return world.cached.local_player.valid() && world.registry.get(world.cached.local_player) != nullptr;
}

const w::Entity *LocalPlayer::player() const {
	const World &world = world_;
	return world.cached.local_player.valid() ? world.registry.get(world.cached.local_player) : nullptr;
}

w::Entity *LocalPlayer::player() {
	World &world = world_;
	return world.cached.local_player.valid() ? world.registry.get(world.cached.local_player) : nullptr;
}

w::AiEntity *LocalPlayer::player_ai() {
	World &world = world_;
	return world.cached.local_player.valid() ? world.ai.for_handle(world.cached.local_player) : nullptr;
}

w::Vec3 LocalPlayer::player_position() const {
	const w::Entity *e = player();
	return e != nullptr ? e->position : w::Vec3{};
}

std::string LocalPlayer::player_anim_key() const {
	const World &world = world_;
	const w::AiEntity *e =
			world.cached.local_player.valid() ? world.ai.for_handle(world.cached.local_player) : nullptr;
	if (e == nullptr || !e->inf.active) return std::string();
	return w::infantry_anim_key(e->inf.anim_state);
}

int32_t LocalPlayer::player_health() const {
	const w::Entity *e = player();
	return e != nullptr ? e->health : 0;
}

void LocalPlayer::set_movement_keys(bool forward, bool back, bool left,
		bool right, bool lean_left, bool lean_right, bool jump) {
	World &world = world_;
	input.forward = forward;
	input.back = back;
	input.left = left;
	input.right = right;
	// The held keys; the pre-tick folds them into the input-flag word (lean
	// 0x2000/0x4000, jump 0x1000) and the pack maps that word onto MoveOrder
	// bits 6/7 and 5 [orig: cases 148/147/153 @0x4e10f1/@0x4e10c8/@0x4e0d66;
	// packer @0x4df6fa-0x4df741].
	input.lean_left = lean_left;
	input.lean_right = lean_right;
	input.jump = jump;
	// Stance comes from the sim-owned SELECT latches (latch_stance — the
	// C2S 0x1D apply semantics [orig: @0x501c60]).
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	// The binocular suppression reads the input word as this frame's fold will
	// leave it, so the raised pose drops on the frame a direction key goes
	// down. The movement-held latch and the unscope-on-move are the pack's
	// (apply_player_input_pre_tick). [orig: Player_UpdatePerFrame
	//  `test byte ptr g_InputFlags, 1Eh` @0x4de3ae, ahead of the pack]
	view.movement_input =
			(input_flags.folded(w::player_input_flags(input, false)) & w::kInputFlagDirectionMask) != 0;
	w::local_player_view_refresh(&world, view);
}

// [orig: Player_AdjustWeaponZoomLevel @0x4dbcc0 -- the CanFire, equipped-slot
//  and zero-table gates @0x4dbcc3..0x4dbcf7, then the clamp (the -1 floor
//  outside a session, `cmp g_NapiNPCtx.is_in_session` @0x4dbd0c), the click,
//  the pitch delta and the yaw term]
bool LocalPlayer::request_scope_zero(int delta) {
    if (!weapon.active || !local_player_can_fire()) return false;
    WeaponSlotState &slot = *active_local_weapon_slot(world_, weapon);
    const int16_t next = weapon_scope_zero_adjust(weapon.def.scope_zero, slot.scope_zero,
        delta, world_.rules.mp_session, world_.rules.auto_scope_zero);
    if (next == slot.scope_zero) return false;
    // A changed zero clicks the GF_SCOPE_ZERO interface set (player_present.h
    // carries the witnesses); the shell plays Interface script sounds 2D
    // [orig: @0x4dbd47..0x4dbd50 -> Sound_PlayInterfaceTriggerSet @0x527be0].
    ScriptSoundEvent click;
    click.name = kScopeZeroSoundset;
    click.kind = ScriptSoundEvent::Kind::Interface;
    world_.out.script_sounds.push_back(std::move(click));
    slot.scope_zero = next;
    if (!weapon.usegun_slot_active && inventory_valid) {
        if (WeaponInventorySlot *entry = inventory.slot(inventory.equipped_combo)) entry->scope_zero = next;
    }
    const int32_t offset = weapon_scope_zero_pitch(weapon.def.scope_zero, next);
    input.look_pitch = io::bam_add(input.look_pitch, io::bam_sub(offset, slot.zero_pitch));
    slot.zero_pitch = offset;
    // The zero-yaw partner (MountSlot+8), negated here for a negative parallax
    // [orig: @0x4dbd91..0x4dbde3 -- the negate @0x4dbddf..0x4dbde3].
    slot.zero_yaw = weapon_scope_zero_yaw(weapon.def.scope_zero, next);
    if (weapon.def.scope_zero.paralax_distance_q16 < 0) slot.zero_yaw = -slot.zero_yaw;
    return true;
}

void LocalPlayer::set_view_keys(bool free_look, bool up, bool down, bool left, bool right) {
    input.free_look = free_look;
    input.look_up = up;
    input.look_down = down;
    input.turn_left = left;
    input.turn_right = right;
}

bool LocalPlayer::stance_request_allowed() const {
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172
	// gate Entity_CheckWeaponSeatFlags(equipped, 0x40000) @0x4e0d8a].
	if (weapon.active && weapon.force_crouch) return false;
	// So does the UseGun seat: a mounted gunner never sends the C2S 0x1D
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 `parentEntity &&
	// parentSlot == 3` @0x4e0da0..0x4e0db5].
	if (const w::Entity *p = player();
			p != nullptr && p->mounted && p->mount_type == w::SeatType::Gunner)
		return false;
	return true;
}

void LocalPlayer::latch_stance(uint8_t bits) {
	// [orig: NapiNPClientMsg_0x00A @0x430549..0x43058f -- prone latch = bit 8,
	//  crouch latch = bit 9 of (tail byte << 8), and MoveOrder's 0x300 replaced
	//  from the same word; NapiNPServerMsg_HandleStanceChange @0x501d0d..0x501d2d
	//  latches the same two bits of the MoveOrder it just wrote; the body tests
	//  prone ahead of crouch @0x4b59ce]
	const bool prone = (bits & 0x01u) != 0;
	const bool crouch = (bits & 0x02u) != 0;
	stance_latch_ = prone ? 2 : (crouch ? 1 : 0);
	input.prone = prone;
	input.crouch = crouch;
	move_order.stance = prone ? w::InfantryState::Stance::kProne
			: (crouch ? w::InfantryState::Stance::kCrouch : w::InfantryState::Stance::kStand);
	if (w::AiEntity *p = player_ai()) p->inf.stance = move_order.stance;
}

bool LocalPlayer::request_stance(int stance) {
	if (stance < 0 || stance > 2) return false;
	if (!stance_request_allowed()) return false;
	if (stance_latch_ == stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and
	// clears the other [orig: NapiNPServerMsg_HandleStanceChange @0x501c60:
	// 169 -> crouch, 170 -> prone, 172 -> clear both].
	stance_latch_ = stance;
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	return true;
}

void LocalPlayer::clear_stance_latches() {
	// [orig: `g_PlayerStanceProneLatch = 0; g_PlayerStanceCrouchLatch = 0`
	//  @0x435c54/@0x435c59 and @0x43561e/@0x435624, beside the entity's
	//  `MoveOrder &= ~0x300` @0x435c42 / @0x43560c]
	stance_latch_ = 0;
	input.crouch = false;
	input.prone = false;
	move_order.stance = w::InfantryState::Stance::kStand;
	if (w::AiEntity *p = player_ai()) p->inf.stance = w::InfantryState::Stance::kStand;
}

// Mouse pixels onto the look angles through the witnessed integer pipeline
// [orig: Input_ProcessMouseAxisBindings @0x499680]. The float accumulator is
// the device-input fold over retail's integer remainder pump [orig:
// Game_ProcessMainFrame @0x526481..0x5264a9, dword_24E0E78].
void LocalPlayer::look(float dx_px, float dy_px) {
	World &world = world_;
	int32_t scoped_zoom = 0;
	if (!view.binoculars_view_active && weapon.active && view.scope_engaged)
		scoped_zoom = w::local_player_scope_zoom(weapon, *w::active_local_weapon_slot(world, weapon));
	const bool prone = stance_latch_ == 2;
	look_accum_x_ += dx_px;
	look_accum_y_ += dy_px;
	const int32_t dx = static_cast<int32_t>(look_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_accum_y_);
	look_accum_x_ -= static_cast<float>(dx);
	look_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
    if (w::local_weapon_seat_flag(weapon, w::player_view_scope_settled(view),
                DEF_WEAPON_FLAG_ABSORBPITCH)) {
        // The mouse-Y binding (action 164) SUBTRACTS its scaled value from
        // dword_B79008 where the ordinary arm adds the same value to Pitch,
        // then clamps the offset to 0..(max - min). Body pitch is left alone.
        // [orig: Input_HandleActionBinding_0 case 164 -- seat-flag query
        //  @0x4E0F9D, `sub dword_B79008, ecx` @0x4E0FE2, clamp
        //  @0x4E0F1A..0x4E0F3A; the ordinary arm `add [eax+14h], edx` @0x4E0D39]
        const auto delta = player_look_delta(look_settings, dx, dy, scoped_zoom);
        input.look_heading = io::bam_add(input.look_heading, delta.yaw);
        weapon.pitch_offset_bam = std::max(0, std::min(
            io::bam_sub(weapon.pitch_offset_bam, delta.pitch),
            io::bam_sub(weapon.pitch_max_bam, weapon.pitch_min_bam)));
    } else {
        w::player_look_apply(input.look_heading, input.look_pitch, look_settings, dx, dy, scoped_zoom, prone);
    }
}

void LocalPlayer::look_chase_orbit(float dx_px, float dy_px) {
	// The death screen raises no optical view, so no scoped reduction
	// [orig: Input_ProcessMouseAxisBindings @0x499706..0x499714].
	look_accum_x_ += dx_px;
	look_accum_y_ += dy_px;
	const int32_t dx = static_cast<int32_t>(look_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_accum_y_);
	look_accum_x_ -= static_cast<float>(dx);
	look_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	const auto delta = w::player_look_delta(look_settings, dx, dy, 0);
	w::player_view_chase_orbit_look(view, delta.yaw, delta.pitch);
}

void LocalPlayer::place_on_composed_view() {
	World &world = world_;
	w::Entity *e = player();
	w::AiEntity *p = player_ai();
	if (e == nullptr || p == nullptr) return;
	w::PlayerCameraPose pose;
	bool mounted_camera = false;
	if (!w::local_player_camera_compose(world, view, view_tracker, pose, mounted_camera)) return;
	// [orig: @0x52b087..0x52b0b7 — g_ViewPos less CameraOffset (+0x6C..+0x74)]
	const int32_t offset[3] = {e->eye_offset_x, e->eye_offset_y, e->eye_offset_z};
	for (int axis = 0; axis < 3; ++axis)
		p->pos[axis] = io::bam_sub(w::to_fixed(pose.eye[axis]), offset[axis]);
	// [orig: @0x52b0ba..0x52b0ee — Yaw and g_LocalPlayerLookYaw = g_ViewRotYaw,
	//  Pitch, Roll]. The port's view words are the composed mission angles.
	const int32_t heading = w::bam_heading_from_mission_yaw_deg(pose.yaw_deg);
	const int32_t pitch = static_cast<int32_t>(
			std::lround(static_cast<double>(pose.pitch_deg) / w::kDegreesPerBam));
	const int32_t roll = static_cast<int32_t>(
			std::lround(static_cast<double>(pose.roll_deg) / w::kDegreesPerBam));
	p->heading = heading;
	p->pitch = pitch;
	p->roll = roll;
	p->inf.target_heading = heading;
	p->inf.look_pitch = pitch;
	input.look_heading = heading;
	input.look_pitch = pitch;
	e->position = {static_cast<float>(w::from_fixed(p->pos[0])),
			static_cast<float>(w::from_fixed(p->pos[1])),
			static_cast<float>(w::from_fixed(p->pos[2]))};
	e->yaw = static_cast<int16_t>(std::lround(
			w::normalize_mission_yaw_deg(w::mission_yaw_deg_from_bam_heading(heading))));
}

void LocalPlayer::aim_at(const w::Vec3 &eye, const w::Vec3 &target) {
	const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	input.look_heading = io::bam_from_radians(std::atan2(dy, dx));
	input.look_pitch = io::bam_from_radians(std::atan2(dz, horizontal));
	if (w::AiEntity *p = player_ai()) {
		p->inf.target_heading = input.look_heading;
		p->inf.look_pitch = input.look_pitch;
	}
}

void LocalPlayer::teleport_local_player(const w::Vec3 &mission_pos, double yaw_deg, double pitch_deg) {
	w::Entity *e = player();
	w::AiEntity *p = player_ai();
	if (e == nullptr || p == nullptr) return;
	e->position = mission_pos;
	p->pos[0] = w::to_fixed(mission_pos.x);
	p->pos[1] = w::to_fixed(mission_pos.y);
	p->pos[2] = w::to_fixed(mission_pos.z);
	p->heading = w::bam_heading_from_mission_yaw_deg(yaw_deg);
	p->pitch = static_cast<int32_t>(pitch_deg / w::kDegreesPerBam);
	// The input-owned view mirrors, or the next pre-tick snaps the view back.
	p->inf.target_heading = p->heading;
	p->inf.look_pitch = p->pitch;
	input.look_heading = p->heading;
	input.look_pitch = p->pitch;
	e->flags &= ~w::kEntityFlagLadderContact;
	e->engine_flags &= ~w::kEntityFlagLadderContact;
	p->inf.pitch_restore_active = false;
	p->inf.pitch_restore_target = 0;
	p->inf.pitch_restore_prev = 0;
	p->collide_state = {};
}

bool LocalPlayer::apply_pose(const int32_t (&position_q16)[3], int32_t heading_bam,
		int32_t pitch_bam) {
	w::Entity *e = player();
	w::AiEntity *p = player_ai();
	if (e == nullptr || p == nullptr) return false;
	// The bridge's person writes: Position, Rotation.X/Y, AirVelocity
	// (onhook/src/debug/debug_bridge.c ApplyPose; GamePerson +0x04 / +0x10 /
	// +0x98, the organic's InfantryState::vel triple).
	for (int axis = 0; axis < 3; ++axis) p->pos[axis] = position_q16[axis];
	p->heading = heading_bam;
	p->pitch = pitch_bam;
	p->inf.vel[0] = p->inf.vel[1] = p->inf.vel[2] = 0;
	e->position = {static_cast<float>(w::from_fixed(p->pos[0])),
			static_cast<float>(w::from_fixed(p->pos[1])),
			static_cast<float>(w::from_fixed(p->pos[2]))};
	p->inf.target_heading = heading_bam;
	p->inf.look_pitch = pitch_bam;
	input.look_heading = heading_bam;
	input.look_pitch = pitch_bam;
	return true;
}

void LocalPlayer::set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed) {
	w::local_weapon_set_input(weapon, view, fire_held, fire_pressed, reload_pressed);
}

void LocalPlayer::queue_to_special() {
	if (to_special_queued < 32) ++to_special_queued; // [orig: @0x4993E8]
}

void LocalPlayer::dispatch_to_special(bool keys_held) {
	if (!inventory_valid) {
		to_special_queued = 0;
		return;
	}
	while (to_special_queued > 0) {
		if (w::local_player_to_special(world_, weapon, inventory, view, keys_held) ==
				w::ToSpecialResult::kDeferred)
			return; // re-dispatched next frame [orig: Input_FlushDeferredEvents @0x497AC0]
		--to_special_queued;
	}
}

bool LocalPlayer::toggle_mount() {
	World &world = world_;
	const w::Entity *toggle_player = player();
	if (toggle_player == nullptr || !toggle_player->alive || toggle_player->health <= 0) return false;
	w::sync_local_usegun_weapon_transition(world, weapon, view);
	const w::WeaponSlotState *active_slot = w::active_local_weapon_slot(world, weapon);
	if (active_slot != nullptr &&
			!w::weapon_state_allows_mount_toggle(active_slot->current, active_slot->next))
		return false;
	// The null-EquippedSlot rejection belongs to UseGun itself, not the
	// top-level USE action: an unarmed local player still enters an ordinary
	// passenger/control seat, and it is an out-of-session-only player gate
	// (force/script and NAPI authority paths bypass it); single player is
	// outside the session [orig: Entity_AttachToUseGunSlot @0x546b80, reject
	// `!is_in_session && Flags&0x100 && !EquippedSlot` -- `cmp
	// g_NapiNPCtx.is_in_session` @0x546BF6, the slot test @0x546c07].
	if (!world.rules.mp_session && !weapon.active) {
		w::VehicleSeatSelection hit;
		if (world.vehicles.find_mount_toggle_candidate(*toggle_player, hit) && hit.type == w::SeatType::Gunner)
			return false;
	}
	const bool changed = world.vehicles.player_toggle_mount(world.cached.local_player);
	if (changed) {
		view.binoculars_requested = false;
		w::local_player_view_refresh(&world, view);
		sync_local_mounted_input_heading();
		w::sync_local_usegun_weapon_transition(world, weapon, view);
	}
	return changed;
}

// [orig: Entity_FindAvailableSeat @0x436798..0x4367C8]
bool LocalPlayer::find_numbered_seat(int index, VehicleSeatSelection &out,
		const VehicleOccupancySource *source) {
	out = {};
	const w::Entity *local = player();
	if (local == nullptr || !local->alive || local->health <= 0) return false;
	w::sync_local_usegun_weapon_transition(world_, weapon, view);
	const w::WeaponSlotState *slot = w::active_local_weapon_slot(world_, weapon);
	// This is currentAction == 11, not the USE toggle's pending-action gate.
	if (weapon.active && slot != nullptr && slot->current >= 2 && slot->current != w::weapon_action::kOverheated)
		return false;
	return w::find_numbered_vehicle_seat(world_, *local, index, out, source);
}

// [orig: Entity_FindAvailableSeat @0x4368B0 -> Entity_RequestVehicleAttach @0x4365DD]
bool LocalPlayer::select_numbered_seat(int index) {
	w::VehicleSeatSelection selected;
	if (!find_numbered_seat(index, selected)) return false;
	// The same out-of-session unarmed UseGun rejection toggle_mount carries
	// [orig: Entity_AttachToUseGunSlot @0x546c07].
	if (!world_.rules.mp_session && !weapon.active && selected.type == w::SeatType::Gunner)
		return false;
	const w::Entity *carrier = world_.registry.get(selected.vehicle);
	const bool changed = carrier != nullptr && world_.vehicles.process_attach(
			world_.cached.local_player, selected.vehicle,
			carrier->seats[static_cast<size_t>(selected.seat_index)].bone_index);
	if (changed) {
		view.binoculars_requested = false;
		w::local_player_view_refresh(&world_, view);
		sync_local_mounted_input_heading();
		w::sync_local_usegun_weapon_transition(world_, weapon, view);
	}
	return changed;
}

namespace {

// The main scene's camera zero, after camera composition. Input, body aim
// and the unadjusted targeting query retain their own frames.
// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0,
// Sighted @0x5ca452..0x5ca465; Scoped @0x5ca494..0x5ca4a0]
void apply_main_scene_optics(World &world, LocalPlayerWeapon &weapon,
		w::LocalPlayerViewFrame &f) {
	const auto *slot = w::active_local_weapon_slot(world, weapon);
	if (f.camera_pose_valid && f.binoculars_view_active) {
		// Already converted from BAM to mission-coordinate deltas.
		// [orig: Render_ProcessMainSceneFrame @0x5ca403..0x5ca407]
		f.camera.yaw_deg += f.binocular_yaw_offset_deg;
		f.camera.pitch_deg += f.binocular_pitch_offset_deg;
	} else if (f.camera_pose_valid && slot != nullptr && f.scope_camera_zero_active) {
		constexpr double degrees_per_bam = 360.0 / 4294967296.0;
		// Mission yaw is 90 - BAM heading.
		f.camera.yaw_deg -= static_cast<float>(slot->zero_yaw * degrees_per_bam);
		f.camera.pitch_deg -= static_cast<float>(slot->zero_pitch * degrees_per_bam);
	}
}

} // namespace

w::LocalPlayerViewFrame LocalPlayer::present_view_frame() {
	World &world = world_;
	// The rendered view owns this latch; target-lock queries and fixed ticks
	// must not advance the shared mission PRNG. [orig:
	// Render_ProcessMainSceneFrame @ 0x5ca0f0, latch @0x5ca3e1..0x5ca3f3]
	w::local_player_binocular_sway_latch(world, view, view_tracker);
	w::LocalPlayerViewFrame f;
	w::local_player_view_frame(&world, weapon, view, view_tracker, f);
	apply_main_scene_optics(world, weapon, f);
	return f;
}

w::LocalPlayerViewFrame LocalPlayer::view_frame() {
	World &world = world_;
	// Observation composes nothing: the camera is the view the last compose
	// left, and neither the shake filters, the chase look-ahead nor the
	// binocular latch move.
	w::LocalPlayerViewFrame f;
	w::local_player_view_observe(&world, weapon, view, view_tracker, f);
	apply_main_scene_optics(world, weapon, f);
	return f;
}

bool LocalPlayer::local_player_can_fire() {
	return w::local_player_scope_view_visible(world_, weapon, view);
}

void LocalPlayer::collect_attach_labels(std::vector<w::AttachLabel> &out, const VehicleOccupancySource *source) {
	World &world = world_;
	out.clear();
	const w::Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return;
	// [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4]
	const bool armory_mode = (player->flags & w::kEntityFlagArmoryZone) != 0;
	world.vehicles.collect_attach_labels(*player, armory_mode, local_player_can_fire(), out, nullptr, source);
}

bool LocalPlayer::local_player_dead() const {
	const World &world = world_;
	if (!world.cached.local_player.valid()) return false;
	const w::Entity *e = world.registry.get(world.cached.local_player);
	return e != nullptr && ((e->flags | e->engine_flags) & w::kEntityFlagDead) != 0;
}

void LocalPlayer::stamp_medic_request() {
	medic_request_cooldown_ticks = kMedicRequestCooldownTicks;
	++medic_request_serial;
}

void LocalPlayer::tick_medic_cooldown(bool local_dead) {
	if (local_dead && !medic_dead_edge_seen_) medic_request_cooldown_ticks = 0;
	medic_dead_edge_seen_ = local_dead;
	if (medic_request_cooldown_ticks > 0) --medic_request_cooldown_ticks;
}

void LocalPlayer::sync_local_mounted_input_heading() {
	World &world = world_;
	if (!world.cached.local_player.valid()) return;
	const w::Entity *player_entity = world.registry.get(world.cached.local_player);
	const w::AiEntity *body = world.ai.for_handle(world.cached.local_player);
	if (player_entity == nullptr || body == nullptr || !body->inf.is_local_player) return;
	// A post-tick difference from the pre-tick input copy is "the sim wrote
	// the view this tick" (the mount-attach yaw snap, the ladder legs, the
	// emplaced gun's gunner-yaw tether and window write-back — retail's
	// g_LocalPlayerLookYaw stores @0x440aa8 / @0x441277 and the occupant
	// Pitch store @0x4412b3, folded here into the input-owned look).
	if (body->inf.target_heading != input.look_heading) input.look_heading = body->inf.target_heading;
	if (body->inf.look_pitch != input.look_pitch) input.look_pitch = body->inf.look_pitch;
}

// [orig: Entity_UpdateInfantryPlayerBody @ 0x4B40E0, block @0x4B5966..0x4B5C97]
void LocalPlayer::apply_scoped_aim_drift(AiEntity &body, uint32_t logic_tick) {
	World &world = world_;
	if (body.handle != world.cached.local_player) return;
	const Entity *entity = world.registry.get(body.handle);
	if (entity == nullptr) return;
	const bool binoculars = view.binoculars_view_active;
	const bool gunner = entity->mount_type == SeatType::Gunner;
	if ((!player_view_scope_settled(view) || gunner) && !binoculars) return;

	const uint16_t stance_bits = body.inf.stance == InfantryState::Stance::kProne ? 0x100 :
			body.inf.stance == InfantryState::Stance::kCrouch ? 0x200 : 0;
	const int stance = (stance_bits & 0x100) != 0 || binoculars ? 0 :
			(stance_bits & 0x200) != 0 ? 1 : 2;
	const WeaponTableEntry *held = world.tables.weapons.by_index(entity->equipped_adm_index);
	const int32_t stability = held != nullptr ? held->stability_fp16[stance] : 0x10000;
	// imul/add/adc/shrd keeps the low 32 bits after the rounded Q16 product,
	// including negative/custom values. Pitch intentionally multiplies twice.
	const auto multiply = [](int32_t value, int32_t factor) {
		const int64_t product = static_cast<int64_t>(value) * factor + 0x8000;
		return static_cast<int32_t>(static_cast<uint64_t>(product) >> 16);
	};
	const auto refresh = [&](ScopedAimAxis &axis, bool pitch) {
		if (axis.stance_bits != stance_bits) axis.drift = 0;
		const int32_t random = world.next_prng16();
		const int32_t amplitude = stance == 0 ? random % 8000 + 3000 :
				stance == 1 ? random % 15000 + 5000 : random % 20000 + 10000;
		axis.limit = multiply(amplitude, stability);
		axis.step = io::bam_sar(axis.limit, 6);
		if (pitch) axis.step = multiply(axis.step, stability);
		axis.decreasing = !axis.decreasing;
		axis.stance_bits = stance_bits;
	};
	// Raw, unstaggered world ticks; enabling the view does not restart either
	// period. Draw yaw before pitch on their common boundary.
	if (logic_tick % 186u == 0) refresh(scope_yaw_, false);
	if (logic_tick % 62u == 0) refresh(scope_pitch_, true);
	const auto advance = [](ScopedAimAxis &axis) {
		if (axis.decreasing) {
			if (axis.drift > io::bam_sub(0, axis.limit))
				axis.drift = io::bam_sub(axis.drift, axis.step);
		} else if (axis.drift < axis.limit) {
			axis.drift = io::bam_add(axis.drift, axis.step);
		}
	};
	advance(scope_pitch_);
	advance(scope_yaw_);
	body.pitch = io::bam_add(body.pitch, scope_pitch_.drift);
	body.heading = io::bam_add(body.heading, scope_yaw_.drift);
	// The native motor stages input separately from the entity. Keep both
	// copies current so its later aim assignment and post-tick input fold
	// preserve retail's entity Pitch/Yaw and g_LocalPlayerLookYaw additions.
	body.inf.look_pitch = io::bam_add(body.inf.look_pitch, scope_pitch_.drift);
	body.inf.target_heading = io::bam_add(body.inf.target_heading, scope_yaw_.drift);
}

void LocalPlayer::carry_process_globals_from(const LocalPlayer &previous) {
	scope_yaw_ = previous.scope_yaw_;
	scope_pitch_ = previous.scope_pitch_;
	analog_pack_ = previous.analog_pack_;
}

void LocalPlayer::pack_analog_axes(w::Entity &entity, bool moving) {
	AnalogPackState &a = analog_pack_;
	// The axis words, shifted to bytes. The keyboard's only writers are the
	// forward and back handlers on the X word (-1023 / +1023); back's binding
	// row follows forward's in the analog dispatch list, so it lands last when
	// both are held. Y and Z have joystick writers only.
	// [orig: Input_HandleActionBinding_0 case 151 `word_B3B750 = 1023`
	//  @0x4e0c75, case 152 `= -1023` @0x4e0cc5; rows 2/3 of the binding table
	//  @0x8159A8 enter g_InputAnalogBindingIndices in row order
	//  (Input_InitBindingSystem @0x499b6a..); the shifts @0x4df79a..0x4df7cd]
	const int16_t word_x = input.back ? int16_t(1023) : (input.forward ? int16_t(-1023) : int16_t(0));
	const int8_t x = static_cast<int8_t>(word_x >> 3);
	const int8_t y = 0;
	const int8_t z = 0;
	// The throttle byte, zeroed inside its 0x14 deadzone [orig: @0x4df7c2..0x4df7d5].
	int8_t throttle = input.analog_throttle;
	if (std::abs(static_cast<int32_t>(throttle)) < 0x14) throttle = 0;
	// A change of more than 32 from the last stored value latches each group
	// on; a moving pack latches both off. [orig: @0x4df7d7..0x4df84e]
	if (std::abs(int32_t(x) - a.last_x) > 32 || std::abs(int32_t(y) - a.last_y) > 32 ||
			std::abs(int32_t(z) - a.last_z) > 32)
		a.axes_active = true;
	if (std::abs(int32_t(throttle) - a.last_throttle) > 32) a.throttle_active = true;
	if (moving) {
		a.axes_active = false;
		a.throttle_active = false;
	}
	// A lean key in the word drops the throttle latch; only a latched pack
	// stores the throttle (and remembers it), else 0. [orig: `test g_InputFlags,
	//  6000h` @0x4df855; @0x4df861..0x4df86e; @0x4df8bd..0x4df8d1]
	if ((input_flags.flags & (w::kInputFlagLeanLeft | w::kInputFlagLeanRight)) != 0)
		a.throttle_active = false;
	if (a.throttle_active) {
		entity.analog_throttle = throttle;
		a.last_throttle = throttle;
	} else {
		entity.analog_throttle = 0;
	}
	// Only a latched pack stores the three axes (and remembers them), else 0.
	// [orig: @0x4df875..0x4df8b5; @0x4df8d9..0x4df8f8]
	if (a.axes_active) {
		entity.net_analog_x = x;
		entity.net_analog_y = y;
		entity.net_analog_z = z;
		a.last_x = x;
		a.last_y = y;
		a.last_z = z;
	} else {
		entity.net_analog_x = 0;
		entity.net_analog_y = 0;
		entity.net_analog_z = 0;
	}
}

void LocalPlayer::apply_player_input_pre_tick(bool pack_input) {
	World &world = world_;
	if (!world.cached.local_player.valid()) return;
	// The per-tick shake decay, ahead of the entity update's arms and the
	// weather tick's quake hard-set: retail decays in Player_UpdatePerFrame
	// from the client network frame that precedes both, once per quantum and
	// gated on the player entity alone [orig: @ 0x4DE590; Game_ProcessMainFrame
	// @ 0x52674b / @ 0x526774].
	w::camera_shake_decay(view.shake);
	// The three fullscreen damage-feedback words decay in the SAME instruction
	// run, immediately after the shake and in the order white / red / revive
	// [orig: Player_UpdatePerFrame @0x4DE5A7..0x4DE5F7].
	w::screen_flash_decay(view.flash);
	w::AiEntity *p = world.ai.for_handle(world.cached.local_player);
	if (p == nullptr) return;
	// Look-up/down key bindings are refused while the AbsorbPitch seat
	// flag answers (an OnlyScoped weapon only once promoted).
	// [orig: cases 154/155 -- Entity_CheckWeaponSeatFlags @0x4E0EA5 / @0x4E0F73]
	const bool absorb = w::local_weapon_seat_flag(weapon, w::player_view_scope_settled(view),
			DEF_WEAPON_FLAG_ABSORBPITCH);
	// The frame's held keys fold into the input-flag word: the bits the last
	// pack reported clear first, then each held key ORs its bit back in. A
	// bit the last pack did not report stays set until the next pack.
	// [orig: Input_ProcessFrame @0x49d541, then the handler dispatch
	//  Input_HandleActionBinding_0 @0x4e0420]
	input_flags.fold(w::player_input_flags(input, absorb));
	// The per-frame binocular refresh reads that folded word, ahead of the
	// pack [orig: Client_ProcessNetworkFrame -> Player_UpdatePerFrame @0x42c18e,
	// `test byte ptr g_InputFlags, 1Eh` @0x4de3ae; the pack follows @0x42c3e9].
	view.movement_input = (input_flags.flags & w::kInputFlagDirectionMask) != 0;
	w::local_player_view_refresh(&world, view);
	if (pack_input) {
		// The pack: the accumulated word onto MoveOrder, then the word is
		// saved as the previous pack's and cleared. The analog throttle is
		// the pack's too [orig: Player_PackInputStateToEntity @0x4df450 --
		// MoveOrder @0x4df68f..0x4df790, analogThrottle @0x4df86e/@0x4df8cb,
		// the clear @0x4df904/@0x4df909].
		// A NoMove weapon strips the direction and lean bits from the word
		// first, the saved copy included; jump, the look keys and free look
		// survive. An OnlyScoped weapon answers only once promoted.
		// [orig: Entity_CheckWeaponSeatFlags(EquippedSlot, 0x20000) @0x4df46c;
		//  `g_InputFlags &= 0xFFFF9FE1` @0x4df482]
		if (w::local_weapon_seat_flag(weapon, w::player_view_scope_settled(view),
					DEF_WEAPON_FLAG_NOMOVE))
			input_flags.flags &= 0xFFFF9FE1u;
		move_order = w::pack_player_body_input(input_flags.flags, input);
		// The packed word's direction bits drive the movement legs: the
		// movement-held latch (it refuses scope-UP on a Scoped weapon
		// @0x4df29c), the drop of the raw binocular toggle (moving lowers the
		// binoculars for good), and, while SETTLED at scope on a Scoped
		// (flags 1) weapon, the full unscope through the toggle -- the
		// ForceScoped pin (@0x4df12d) keeps pinned sights raised -- else the
		// hip-fire camera legs. A joiner runs them once per send boundary.
		// [orig: Player_PackInputStateToEntity @0x4df4b2..0x4df63b --
		//  g_MovementKeyHeld @0x4df4bb / @0x4df4f9, `mov g_BinocularsToggle, 0`
		//  @0x4df4c2, Player_ToggleWeaponScope @0x4df4c9..0x4df4ec, the
		//  entitySlotPtr legs @0x4df500..0x4df63b]
		const bool direction = move_order.direction_bits != 0;
		if (direction) view.binoculars_requested = false;
		if (w::player_view_move_input(view, direction, weapon.active ? weapon.def.flags : 0) &&
				(weapon.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0) {
			if (w::local_player_set_scope(world, weapon, view,
						*w::active_local_weapon_slot(world, weapon), false))
				w::weapon_fsm_queue_scope_down(*w::active_local_weapon_slot(world, weapon));
		}
		if (w::Entity *entity = world.registry.get(p->handle))
			pack_analog_axes(*entity, move_order.moving);
		input_flags.clear_after_pack();
	}
    // The keyboard turn/look rotation reads the MoveOrder bits the last pack
    // wrote; the prone halving reads its stance bit. Both pitch limits follow
    // body slope. [orig: Entity_ApplyFreeLookRotation @0x4ae090 -- MoveOrder
    // 0x1000/0x2000 @0x4ae0bc/@0x4ae0df, 0x4000/0x8000 @0x4ae0fe/@0x4ae109,
    // 0x100 @0x4ae09e; called by the local body before aim/camera updates]
    // The death screen's free-fly motor replaces that body and turns by the
    // same keys itself [orig: Entity_UpdateInfantryPlayerBody @0x4b40f8 ->
    // Camera_UpdateFreeFly @0x4b2b13..0x4b2b70].
    if ((world.logic_tick & 1u) != 0 && !world.spectator.death_screen) {
        int32_t pitch = input.look_pitch;
        player_look_keys(input.look_heading, pitch, move_order.turn_left,
            move_order.turn_right, move_order.look_up, move_order.look_down,
            move_order.stance == InfantryState::Stance::kProne, p->body_pitch);
        if (!absorb) input.look_pitch = pitch;
    }
	// MoveOrder persists between packs, so the body sees the last pack's word
	// every tick; the look heading/pitch are the handlers' direct per-frame
	// entity writes, not the pack's [orig: Input_ProcessMouseAxisBindings
	// @0x499680; Input_HandleActionBinding_0 cases 164..167 @0x4e0fa5..0x4e110f].
	w::PlayerBodyInput body = move_order;
	body.look_heading = input.look_heading;
	body.look_pitch = input.look_pitch;
	w::apply_player_body_input(*p, body);
	// Input precedes the pool-1 vehicle callbacks. Publish current look and
	// MoveOrder now, before the later pool-0 body pose; otherwise a driver's
	// key press, release and mouse steering arrive one motor tick late.
	// This mirror copies retained animation/eye values without advancing them.
	// [orig: Player_PackInputStateToEntity @0x4DF450;
	// Input_ProcessMouseAxisBindings @0x499680; pool-1 walk @0x4C2158]
	p->heading = p->inf.target_heading;
	p->pitch = p->inf.look_pitch;
	world.ai.mirror_wire_anim(*p, world);
	stamp_body_view();
}

void LocalPlayer::stamp_body_view() {
	World &world = world_;
	if (!world.cached.local_player.valid()) return;
	w::AiEntity *p = world.ai.for_handle(world.cached.local_player);
	if (p == nullptr) return;
	// [orig: Player_PackInputStateToEntity @0x4DF450 -- the body's view and weapon facts]
	const bool scope_promoted = weapon.active && w::player_view_scope_settled(view);
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (weapon.active) w::infantry_weapon_switch_stamp(p->inf, weapon.category_serial);
		p->inf.scope_raised = scope_promoted;
		p->inf.binoculars_raised = view.binoculars_raised;
		p->inf.wpn_run_anim = weapon.active ? weapon.run_anim : 0;
		p->inf.wpn_force_crouch = weapon.active && weapon.force_crouch;
		// entity+0x37C mirror: the kit weight the last 0x5A/accept stamped.
		p->inf.loadout_weight_fp16 = loadout.weight_fp16;
		p->inf.aimed_shot_available = local_player_can_fire();
	}
	if (w::Entity *entity = world.registry.get(world.cached.local_player)) {
		// The per-frame view-flag restamp onto the body's Flags word
		// [orig: the g_NVGActive / g_BinocularsRaised / g_WeaponScopeActive
		// refresh in Player_PackInputStateToEntity @0x4df450].
		uint32_t view_flags = 0;
		if (view.nvg_active) view_flags |= w::kEntityFlagNVGWorn;
		if (view.binoculars_raised) view_flags |= w::kEntityFlagBinoculars;
		if (scope_promoted) view_flags |= w::kEntityFlagScopeRaised;
		constexpr uint32_t kViewFlagMask =
				w::kEntityFlagNVGWorn | w::kEntityFlagBinoculars | w::kEntityFlagScopeRaised;
		entity->flags = (entity->flags & ~kViewFlagMask) | view_flags;
	}
}

void LocalPlayer::run_local_view_tick() {
	// Retail promotes the per-frame view before weapon actions; the sim-wrote-
	// the-view fold runs first so the pumps read the settled look.
	sync_local_mounted_input_heading();
	tick_view();
}

void LocalPlayer::pump_local_weapon() {
	World &world = world_;
	w::LocalWeaponPumpIO io;
	io.view = &view;
	io.inventory = inventory_valid ? &inventory : nullptr;
	io.is_authority = true;
	io.authority_fire_admitted = authority_fire_admitted;
	w::local_weapon_pump_tick(world, weapon, io);
	hud_map_control.weapon_command(io.map_command);
	// The wire-facing outcome for the embedder's relay leg (the local reload
	// producer the listen drain consumes).
	last_reload = io.reload;
}

void LocalPlayer::tick_view() {
	World &world = world_;
	w::local_player_view_tick(&world, view, view_tracker, view_session_inputs, &weapon);
	// Retail acquires the aim inside the entity update, BEFORE the quantum's
	// camera compose, so its camera leg reads the view the previous compose
	// left [orig: Game_ProcessMainFrame @0x5263F0 -- the Entity_UpdateAllEntities
	//  call @0x52674B precedes the Camera_ComputeThirdPersonView call @0x526781].
	update_aim_target();
	// The quantum's own compose: it advances the shake filters and the chase
	// look-ahead once per logic tick and leaves g_view_pos / g_view_rot for
	// the readers until the next compose [orig: @0x526781].
	w::PlayerCameraPose composed;
	bool mounted_camera = false;
	w::local_player_camera_compose(world, view, view_tracker, composed, mounted_camera);
}

void LocalPlayer::reset_for_new_round() {
    // Round init clears the world's dialog table, its first statement and again
    // after the overlay reset; the second finds it empty
    // [orig: Game_InitNewRound @0x422741 / @0x4227ac -> Dialog_ResetAll @0x44dc90].
    world_.script.dialog.reset();
    // The overlay-buffer reset is unconditional in retail's round init; the
    // local-player gate below covers only the view state
    // [orig: Game_InitNewRound @0x4227a7 -> HUD_ResetAllOverlayBuffers @0x59dd40].
    w::radar_reset(radar);
    Entity *local = player();
    if (local == nullptr) return;
    // [orig: Game_InitNewRound @0x422740; Camera_ResetToLocalPlayer @0x4a3d30]
    view.binoculars_requested = false;
    view.binoculars_raised = false;
    view.binoculars_view_active = false;
    w::player_view_scope_reset(view);
    hud_map_control.mode = 0;
    stance_latch_ = 0;
    input.crouch = input.prone = false;
    weapon.power_throw_start_tick = 0;
    view.shake.counter = 0;
    // The three fullscreen damage-feedback words
    // [orig: Game_InitNewRound @0x422778 / @0x422784 / @0x422790]. The revive
    // tint holds at its 0xC4 decay floor forever otherwise, so this clear (and
    // the mission-start one) is the only thing that ever puts it out.
    w::screen_flash_clear(view.flash);
    world_.weather.core.hit_dim.intensity = 0;
    world_.weather.core.hit_dim.fade_rate = 0;
    view.camera_mode = 0;
    view.third_person = false;
    // The chase back on the local player at 1.0 with the orbit zeroed
    // [orig: Camera_ResetToLocalPlayer @0x4a3d42..0x4a3d5b].
    view.camera_tracked = 0;
    view.chase_distance_q16 = w::kTpDistanceQ16;
    view.chase_orbit_yaw = 0;
    view.chase_orbit_pitch = 0;
    view.tp_anchor_valid = true;
    for (int axis = 0; axis < 3; ++axis) {
        const float pos = axis == 0 ? local->position.x :
                axis == 1 ? local->position.y : local->position.z;
        view.tp_anchor[axis] = pos;
        view.tp_anchor_q16[axis] = to_fixed(pos);
        view.lookahead_q16[axis] = 0;
    }
    world_.cached.sound_listener_view_flags = audio::kListenerViewFirstPerson;
    world_.script.waypoints.reset_selection(world_.registry, *local,
            world_.waypoint_context().game_type);
    // The look yaw takes the player's +0x10 word, the heading the deploy just
    // placed, never its whole-degree mirror (D-NET-376)
    // [orig: Game_InitNewRound @0x4227DA..0x4227E3].
    const w::AiEntity *motor = player_ai();
    input.look_heading = motor != nullptr ? motor->heading
                                          : bam_heading_from_mission_yaw_deg(local->yaw);
    // The HUD buffers belong to the presenting device; one ordered effect
    // carries the reset without discarding unrelated mission events.
    Effect reset;
    reset.kind = "local_round_reset";
    world_.out.effects.push(std::move(reset));
    ++round_reset_revision;
}

void LocalPlayer::reset_local_player_input_to_player_facing() {
	World &world = world_;
	int32_t heading = 0;
	if (world.cached.local_player.valid())
		if (const w::AiEntity *pe = world.ai.for_handle(world.cached.local_player))
			heading = pe->heading;
	reset_local_player_input(heading);
}

void LocalPlayer::reset_local_player_input(int32_t look_heading_bam) {
	input = w::PlayerInput{};
	stance_latch_ = 0;
	// The spawn clears MoveOrder's stance bits with the latches; the
	// movement bits persist until the next pack, as retail's do
	// [orig: PlayerClass_InitEntity @0x4b1069 `MoveOrder &= ~0x300`].
	move_order.stance = w::InfantryState::Stance::kStand;
	look_accum_x_ = look_accum_y_ = 0.0f;
	input.look_heading = look_heading_bam;
}


void LocalPlayer::latch_attack_defend_role(uint32_t game_type) {
	// [orig: sub_524110 @0x524110 — cleared first @0x52411A]
	attack_defend_role = 0;
	if (game_type != 0x10002u) return; // [orig: `cmp g_GameType, 10002h` @0x524110]
	const w::Entity *local = player();
	if (local == nullptr) return;
	const World &world = world_;
	for (const int pool : {2, 1}) {
		for (size_t i = 0; i < world.registry.pool_capacity(pool); ++i) {
			const w::Entity *e = world.registry.get(w::EntityHandle::make(pool, static_cast<int>(i)));
			if (e == nullptr || !e->has_item_def || (e->item_attrib & 0x8000u) == 0u) continue;
			attack_defend_role = e->team != local->team ? 2u : 1u;
			return;
		}
	}
}

} // namespace opennova::world
