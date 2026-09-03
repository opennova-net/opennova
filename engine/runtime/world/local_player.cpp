#include <runtime/world/local_player.h>

#include <base/io/bam.h>
#include <base/io/log.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

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
		if (w::player_view_set_engaged(view, false,
					(weapon.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
			w::weapon_fsm_queue_scope_down(*w::active_local_weapon_slot(world, weapon));
	}
	w::local_player_view_refresh(&world, view);
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
	if (!view.binoculars_view_active && weapon.active && view.scope_engaged && weapon.scope_max_mag > 1.0f)
		scoped_zoom = static_cast<int32_t>(weapon.scope_max_mag);
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

w::LocalPlayerViewFrame LocalPlayer::view_frame() {
	World &world = world_;
	w::LocalPlayerViewFrame f;
	w::local_player_view_frame(&world, weapon, view, view_tracker, f);
	return f;
}

bool LocalPlayer::local_player_can_fire(const w::AiEntity *body) const {
	const World &world = world_;
	// The Player_CanFireWeapon verdict the body updater and the HUD share
	// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80; Sighted helper
	// @0x4dcd30].
	const w::Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr || body == nullptr || !weapon.active) return false;
	bool mount_allows = true;
	if (local->mounted)
		mount_allows = local->mount_type == w::SeatType::Passenger ||
				(local->mount_type == w::SeatType::Gunner && weapon.usegun_slot_active);
	if (!mount_allows || view.third_person || view.binoculars_view_active) return false;
	const w::WeaponSlotState *slot = w::active_local_weapon_slot(world, weapon);
	if (slot == nullptr) return false;
	const uint32_t flags = static_cast<uint32_t>(weapon.def.flags);
	if (slot->current == w::weapon_action::kReload && (flags & DEF_WEAPON_FLAG_NOCARDSWITCH) == 0)
		return false;
	const bool scope_promoted = body->inf.scope_raised;
	const bool scoped = scope_promoted && (flags & DEF_WEAPON_FLAG_SCOPED) != 0;
	const bool sighted = scope_promoted && (flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
			slot->current != w::weapon_action::kSwitchFrom;
	const uint32_t entity_flags = local->flags | local->engine_flags;
	const bool in_air = body->inf.airborne || (entity_flags & w::kEntityFlagInAir) != 0;
	const bool submerged = (entity_flags & w::kEntityFlagDrowning) != 0 ||
			w::entity_eye_below_water(world, body->pos[2], local->eye_offset_z);
	// Dead (Flags & 0x2) and airborne (0x2000) share ONE can_fire=0 group
	// that the FORCESCOPED override reverses, so a dead body holding a
	// ForceScoped weapon still reads can_fire [orig: `Flags & 0x2002` @0x5cf7fb;
	// the override @0x5cf845].
	const bool dead = !local->alive || local->health <= 0;
	const bool ordinary = !dead && !in_air && (sighted || (scoped && !body->inf.player_moving)) &&
			(sighted || !submerged);
	return (flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0 || ordinary;
}

void LocalPlayer::collect_attach_labels(std::vector<w::AttachLabel> &out) {
	World &world = world_;
	out.clear();
	const w::Entity *player = world.registry.get(world.cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return;
	// [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4]
	const bool armory_mode = (player->flags & w::kEntityFlagArmoryZone) != 0;
	const w::AiEntity *body =
			world.ai.for_handle(world.cached.local_player);
	world.vehicles.collect_attach_labels(*player, armory_mode, local_player_can_fire(body), out);
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
	// the view this tick" (the mount-attach yaw snap, the ladder legs).
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
	w::apply_player_body_input(*p, w::pack_player_body_input(input));
	const bool scope_promoted = weapon.active && view.scope_engaged &&
			!w::player_view_scope_ease_active(view);
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (weapon.active) w::infantry_weapon_switch_stamp(p->inf, weapon.anim_map_serial);
		p->inf.scope_raised = scope_promoted;
		p->inf.binoculars_raised = view.binoculars_raised;
		p->inf.wpn_run_anim = weapon.active ? weapon.run_anim : 0;
		p->inf.wpn_force_crouch = weapon.active && weapon.force_crouch;
		p->inf.aimed_shot_available = local_player_can_fire(p);
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
