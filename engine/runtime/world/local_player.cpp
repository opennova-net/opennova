#include <runtime/world/local_player.h>

#include <base/io/bam.h>
#include <base/io/log.h>
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

namespace {

int32_t bam_from_radians(double radians) {
	return static_cast<int32_t>(
			static_cast<int64_t>(std::llround(radians * io::kBamPerRadian)));
}

} // namespace

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
	const World &world = world_;
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
	const World &world = world_;
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
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once
	// grounded.
	input.lean_left = lean_left;
	input.lean_right = lean_right;
	input.jump = jump;
	// Stance comes from the sim-owned SELECT latches (request_stance — the
	// C2S 0x1D apply semantics [orig: @0x501c60]).
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @0x4df450 — any of the four direction keys
	// sets g_movementKeyHeld (blocks scope-UP on Scoped weapons @0x4df29c)
	// and, while SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@0x4df12d) keeps pinned sights raised].
	const bool move_held = forward || back || left || right;
	if (w::player_view_move_input(view, move_held,
				weapon.active ? weapon.def.flags : 0) &&
			(weapon.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0) {
		if (w::local_player_set_scope(world, weapon, view,
					*w::active_local_weapon_slot(world, weapon), false))
			w::weapon_fsm_queue_scope_down(*w::active_local_weapon_slot(world, weapon));
	}
	w::local_player_view_refresh(&world, view);
}

// [orig: Player_AdjustWeaponZoomLevel @0x4dbcc0 -- the CanFire, equipped-slot
//  and zero-table gates @0x4dbcc3..0x4dbcf7, then the clamp, the click, the
//  pitch delta and the yaw term]
bool LocalPlayer::request_scope_zero(int delta) {
    if (!weapon.active || !local_player_can_fire()) return false;
    WeaponSlotState &slot = *active_local_weapon_slot(world_, weapon);
    const int16_t next = weapon_scope_zero_adjust(weapon.def.scope_zero, slot.scope_zero,
        delta, world_.rules.session_open, world_.rules.auto_scope_zero);
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

bool LocalPlayer::request_stance(int stance) {
	World &world = world_;
	if (stance < 0 || stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172
	// gate Entity_CheckWeaponSeatFlags(equipped, 0x40000) @0x4e0d8a].
	if (weapon.active && weapon.force_crouch) return false;
	// So does the UseGun seat: a mounted gunner never sends the C2S 0x1D
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 `parentEntity &&
	// parentSlot == 3` @0x4e0da0..0x4e0db5].
	if (const w::Entity *p = player();
			p != nullptr && p->mounted && p->mount_type == w::SeatType::Gunner)
		return false;
	if (stance_latch_ == stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and
	// clears the other [orig: NapiNPServerMsg_HandleStanceChange @0x501c60:
	// 169 -> crouch, 170 -> prone, 172 -> clear both].
	stance_latch_ = stance;
	input.crouch = stance_latch_ == 1;
	input.prone = stance_latch_ == 2;
	return true;
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
	w::player_look_apply(input.look_heading, input.look_pitch, look_settings, dx, dy, scoped_zoom, prone);
}

void LocalPlayer::aim_at(const w::Vec3 &eye, const w::Vec3 &target) {
	World &world = world_;
	const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	input.look_heading = bam_from_radians(std::atan2(dy, dx));
	input.look_pitch = bam_from_radians(std::atan2(dz, horizontal));
	if (w::AiEntity *p = player_ai()) {
		p->inf.target_heading = input.look_heading;
		p->inf.look_pitch = input.look_pitch;
	}
}

void LocalPlayer::teleport_local_player(const w::Vec3 &mission_pos, double yaw_deg, double pitch_deg) {
	World &world = world_;
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

void LocalPlayer::set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed) {
	World &world = world_;
	w::local_weapon_set_input(weapon, view, fire_held, fire_pressed, reload_pressed);
}

bool LocalPlayer::toggle_mount() {
	World &world = world_;
	const w::Entity *toggle_player = player();
	if (toggle_player == nullptr || !toggle_player->alive || toggle_player->health <= 0) return false;
	w::sync_local_usegun_weapon_transition(world, weapon);
	const w::WeaponSlotState *active_slot = w::active_local_weapon_slot(world, weapon);
	if (active_slot != nullptr &&
			!w::weapon_state_allows_mount_toggle(active_slot->current, active_slot->next))
		return false;
	// The null-EquippedSlot rejection belongs to UseGun itself, not the
	// top-level USE action: an unarmed local player still enters an ordinary
	// passenger/control seat, and it is an out-of-session-only player gate
	// (force/script and NAPI authority paths bypass it) [orig:
	// Entity_AttachToUseGunSlot @0x546b80, reject `!is_in_session &&
	// Flags&0x100 && !EquippedSlot` @0x546c07].
	if (!world.rules.session_open && !weapon.active) {
		w::VehicleSeatSelection hit;
		if (world.vehicles.find_mount_toggle_candidate(*toggle_player, hit) && hit.type == w::SeatType::Gunner)
			return false;
	}
	const bool changed = world.vehicles.player_toggle_mount(world.cached.local_player);
	if (changed) {
		view.binoculars_requested = false;
		view_tracker.binocular_yaw_offset_deg = 0.0f;
		view_tracker.binocular_pitch_offset_deg = 0.0f;
		w::local_player_view_refresh(&world, view);
		sync_local_mounted_input_heading();
		w::sync_local_usegun_weapon_transition(world, weapon);
	}
	return changed;
}

// [orig: Entity_FindAvailableSeat @0x436798..0x4367C8]
bool LocalPlayer::find_numbered_seat(int index, VehicleSeatSelection &out,
		const VehicleOccupancySource *source) {
	out = {};
	const w::Entity *local = player();
	if (local == nullptr || !local->alive || local->health <= 0) return false;
	w::sync_local_usegun_weapon_transition(world_, weapon);
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
	if (!world_.rules.session_open && !weapon.active && selected.type == w::SeatType::Gunner)
		return false;
	const w::Entity *carrier = world_.registry.get(selected.vehicle);
	const bool changed = carrier != nullptr && world_.vehicles.process_attach(
			world_.cached.local_player, selected.vehicle,
			carrier->seats[static_cast<size_t>(selected.seat_index)].bone_index);
	if (changed) {
		view.binoculars_requested = false;
		view_tracker.binocular_yaw_offset_deg = 0.0f;
		view_tracker.binocular_pitch_offset_deg = 0.0f;
		w::local_player_view_refresh(&world_, view);
		sync_local_mounted_input_heading();
		w::sync_local_usegun_weapon_transition(world_, weapon);
	}
	return changed;
}

w::LocalPlayerViewFrame LocalPlayer::view_frame() {
	World &world = world_;
	w::LocalPlayerViewFrame f;
	w::local_player_view_frame(&world, weapon, view, view_tracker, f);
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
	World &world = world_;
	medic_request_cooldown_ticks = kMedicRequestCooldownTicks;
	++medic_request_serial;
}

void LocalPlayer::tick_medic_cooldown(bool local_dead) {
	World &world = world_;
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

void LocalPlayer::apply_player_input_pre_tick() {
	World &world = world_;
	if (!world.cached.local_player.valid()) return;
	// The per-tick shake decay, ahead of the entity update's arms and the
	// weather tick's quake hard-set: retail decays in Player_UpdatePerFrame
	// from the client network frame that precedes both, once per quantum and
	// gated on the player entity alone [orig: @ 0x4DE590; Game_ProcessMainFrame
	// @ 0x52674b / @ 0x526774].
	w::camera_shake_decay(view.shake);
	w::AiEntity *p = world.ai.for_handle(world.cached.local_player);
	if (p == nullptr) return;
	w::local_player_view_refresh(&world, view);
    // [orig: Entity_ApplyFreeLookRotation @0x4ae090, called by the local
    // body before aim/camera updates]. Both pitch limits follow body slope.
    if ((world.logic_tick & 1u) != 0)
        player_look_keys(input.look_heading, input.look_pitch, input.turn_left,
        input.turn_right, input.look_up, input.look_down, input.prone, p->body_pitch);
	w::apply_player_body_input(*p, w::pack_player_body_input(input));
	if (w::Entity *entity = world.registry.get(p->handle))
		entity->analog_throttle = input.analog_throttle;
	const bool scope_promoted = weapon.active && w::player_view_scope_settled(view);
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (weapon.active) w::infantry_weapon_switch_stamp(p->inf, weapon.anim_map_serial);
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
		// [orig: the g_NVGActive / g_binocularsRaised / g_weaponScopeActive
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

void LocalPlayer::run_local_player_post_tick() {
	World &world = world_;
	// Retail promotes the per-frame view before weapon actions; the sim-wrote-
	// the-view fold runs first so the pumps read the settled look.
	sync_local_mounted_input_heading();
	tick_view();
	w::LocalWeaponPumpIO io;
	io.view = &view;
	io.inventory = inventory_valid ? &inventory : nullptr;
	io.is_authority = true;
	w::local_weapon_pump_tick(world, weapon, io);
	// The wire-facing outcomes for the embedder's relay legs (the local reload
	// producer the listen drain consumes; a joiner's fired-round uplink).
	last_fired = io.fired;
	last_reload = io.reload;
}

void LocalPlayer::tick_view() {
	World &world = world_;
	w::local_player_view_tick(&world, weapon, view, view_tracker, view_session_inputs);
    update_aim_target();
}

void LocalPlayer::reset_for_new_round() {
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
    world_.weather.core.hit_dim.intensity = 0;
    world_.weather.core.hit_dim.fade_rate = 0;
    view.camera_mode = 0;
    view.third_person = false;
    view.tp_anchor_valid = true;
    for (int axis = 0; axis < 3; ++axis) {
        const float pos = axis == 0 ? local->position.x :
                axis == 1 ? local->position.y : local->position.z;
        view.tp_anchor[axis] = pos;
        view.tp_anchor_q16[axis] = to_fixed(pos);
        view.lookahead_q16[axis] = 0;
    }
    world_.cached.sound_listener_view_flags = 2;
    world_.script.waypoints.reset_selection(world_.registry, *local, world_.match.rules().game_type);
    input.look_heading = bam_heading_from_mission_yaw_deg(local->yaw);
    // Dialog and HUD buffers belong to the presenting device; one ordered
    // effect carries the reset without discarding unrelated mission events.
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
	World &world = world_;
	input = w::PlayerInput{};
	stance_latch_ = 0;
	look_accum_x_ = look_accum_y_ = 0.0f;
	input.look_heading = look_heading_bam;
}

} // namespace opennova::world
