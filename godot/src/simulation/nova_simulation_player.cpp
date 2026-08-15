// Simulation — the LOCAL PLAYER core: input and spawn, pose/state getters.
// The view-effects, equipped-weapon FSM, and loadout clusters live in the
// sibling nova_simulation_player_{view,weapon,loadout}.cpp TUs.
#include "simulation/nova_simulation_internal.h"

#include <world/music_vars.h>
#include <world/player_view.h>

#include <def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*

using namespace novasim;

void Simulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return;
	refresh_local_player_view_effects();
	opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
	// g_weaponScopeActive is the post-ease promotion, not raw scope intent.
	// This one value feeds body pose, per-shot recoil scaling, and CanFire.
	// [orig: promoter @0x4DE4F7; local body mirror @0x4B5D95]
	const bool scope_promoted = local_weapon_.active && player_view_.scope_engaged &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	// The local-player weapon-channel inputs, refreshed before the body updater runs —
	// the Flags-bit refresh (Flags|0x10 from g_weaponScopeActive; the binoculars bit
	// stays false until a shell binoculars input exists). This is the ONLY part of the
	// weapon channel the original gates on locality [orig: @ 0x4b5d7f]; the hold kind is
	// NOT mirrored here any more — infantry_weapon_channel re-reads it from the ADM
	// table by the entity's own equipped index every selection pass, exactly as the
	// original does [orig: @ 0x4b5dba], which is the same path a remote player's pose
	// resolves through. Keeping a local-only scalar beside that table read would put two
	// sources behind one value and let the two disagree across a weapon switch.
	p->inf.aimed_shot_available = false;
	if (p->inf.active) {
		if (local_weapon_.active) {
			opennova::world::infantry_weapon_switch_stamp(
					p->inf, local_weapon_.anim_map_serial);
		}
		p->inf.scope_raised = scope_promoted;
		p->inf.binoculars_raised = player_view_.binoculars_raised;
		// The run-gait class + ForceCrouch mirror, same per-tick re-read pattern as the
		// hold kind [orig: the selection reads AdmDefs[+0x2B0]+0xAC each pass @ 0x4b72cf;
		// the ForceCrouch checks read the equipped def flags @ 0x4b7245/@ 0x4e0d8a].
		p->inf.wpn_run_anim = local_weapon_.active ? local_weapon_.run_anim : 0;
		p->inf.wpn_force_crouch = local_weapon_.active && local_weapon_.force_crouch;

		// The body updater and HUD share retail's Player_CanFireWeapon verdict.
		// A promoted Scoped weapon (Flags bit 0) is rejected while moving and
		// submerged. The second predicate, misleadingly named as a vehicle-gunner
		// helper in the IDB, is actually promoted Sighted (bit 1, except SWITCHFROM)
		// with no mount gate; it bypasses those two ordinary checks. Both still lose
		// to InAir. ForceScoped is the final first-person override, but never bypasses
		// the earlier card-switch reload or seat rejection.
		// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80;
		// Sighted helper @0x4dcd30; HUD row select @0x592b87]
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		bool mount_allows_aimed_shot = local != nullptr;
		if (local != nullptr && local->mounted) {
			mount_allows_aimed_shot =
					local->mount_type == opennova::world::SeatType::Passenger ||
					(local->mount_type == opennova::world::SeatType::Gunner &&
							local_weapon_.usegun_slot_active);
		}
		const opennova::world::WeaponSlotState &active_slot =
				*active_local_weapon_slot();
		const uint32_t weapon_flags =
				static_cast<uint32_t>(local_weapon_.def.flags);
		const uint32_t entity_flags = local != nullptr
				? local->flags | local->engine_flags
				: 0;
		const bool card_switch_reload =
				active_slot.current == opennova::world::weapon_action::kReload &&
				(weapon_flags & DEF_WEAPON_FLAG_NOCARDSWITCH) == 0;
		const bool scoped_aimed_shot = scope_promoted &&
				(weapon_flags & DEF_WEAPON_FLAG_SCOPED) != 0;
		const bool sighted_aimed_shot = scope_promoted &&
				(weapon_flags & DEF_WEAPON_FLAG_SIGHTED) != 0 &&
				active_slot.current != opennova::world::weapon_action::kSwitchFrom;
		const bool in_air = p->inf.airborne ||
				(entity_flags & opennova::world::kEntityFlagInAir) != 0;
		// The shared witnessed eye-projection classifier (world/round_sim.h
		// entity_eye_below_water) plus the drowning flag.
		const bool submerged =
				(entity_flags & opennova::world::kEntityFlagDrowning) != 0 ||
				opennova::world::entity_eye_below_water(*world_, p->pos[2],
						local != nullptr ? local->eye_offset_z : 0);
		const bool ordinary_aimed_shot = !in_air &&
				(sighted_aimed_shot ||
						(scoped_aimed_shot && !p->inf.player_moving)) &&
				(sighted_aimed_shot || !submerged);
		const bool force_scoped =
				(weapon_flags & DEF_WEAPON_FLAG_FORCESCOPED) != 0;
		p->inf.aimed_shot_available = local != nullptr && local->alive &&
				local->health > 0 && local_weapon_.active && mount_allows_aimed_shot &&
				!card_switch_reload && !player_view_.third_person &&
				!player_view_.binoculars_view_active &&
				(force_scoped || ordinary_aimed_shot);
	}
	if (opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player)) {
		uint32_t view_flags = 0;
		if (player_view_.nvg_active) view_flags |= 0x4u;
		if (player_view_.binoculars_raised) view_flags |= 0x8u;
		if (scope_promoted) view_flags |= 0x10u;
		entity->flags = (entity->flags & ~0x1cu) | view_flags;
	}
}

void Simulation::sync_local_mounted_input_heading() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	const AiEntity *body = world_->ai->for_handle(world_->cached.local_player);
	if (player == nullptr || body == nullptr || !body->inf.is_local_player)
		return;

	// Retail has ONE input-owned view: the mouse accumulators ARE the entity
	// yaw/pitch (g_LocalPlayerLookYaw / g_LadderPitchRestorePrev ride along
	// with every sim-side write). Our split keeps the accumulator in this host
	// input record, so any view value the core wrote during the tick must be
	// mirrored back before the next pre-tick input write can undo it. The
	// sim-side writers (all engine-side, cited at their port sites): the
	// mount-attach yaw snap, and the ladder legs — the alignment chase, the
	// ±120° view clamp, the post-ladder pitch restore
	// (docs/world/world-wac-ai-re.md §30). A post-tick difference from the
	// pre-tick input copy is exactly "the sim wrote the view this tick".
	if (body->inf.target_heading != player_input_.look_heading)
		player_input_.look_heading = body->inf.target_heading;
	if (body->inf.look_pitch != player_input_.look_pitch)
		player_input_.look_pitch = body->inf.look_pitch;
}

bool Simulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!world_installed_ || !world_ || !world_->ai) return false;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up (the faithful §5.0
	// mode-3 path), so an explicit spawn is a no-op success there. The legacy LAN host + any non-listen
	// caller (no auto-spawn) still spawn at the requested pose below.
	if (has_local_player()) return true;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x, -z, y): the inverse of the present (x,y,z) -> (x, z, -y) remap.
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return false;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return true;
}

int Simulation::spawn_local_player_at_start() {
	if (!world_installed_ || !world_ || !world_->ai) return -1;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up via the SAME
	// select_player_spawn start-marker scan (Server_BuildPlayerInfoAndAdd), so when a player already
	// exists this is a no-op success (the player is at its start, input seeded by bringup_host_runtime).
	if (has_local_player()) return 1;
	// Pick the player-start marker the original would — scan the 60xx start-marker family (SP/DM,
	// coop, team), FARTHEST from the enemy set — instead of the first NPC's position. Finds the
	// authored start whatever the mission mode (e.g. a 6001-only SP training mission like 00TRa).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c]
	const opennova::world::SpawnPointResult sel = opennova::world::select_player_spawn(*world_);
	opennova::world::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No player-start marker authored: spawn at the mission origin (the terrain clamp grounds
		// it). NEVER fall back to an NPC's position — that is the bug this replaces.
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	// SP keeps the player's own team; the marker's team is not copied [orig: §5.2c]. Team 1 mirrors
	// the prior placeholder until the MP team path lands.
	spawn.team = 1;
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return -1;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return sel.found ? 1 : 0;
}

bool Simulation::has_local_player() const {
	return world_ && world_->cached.local_player.valid();
}

int Simulation::get_local_player_wire_handle() const {
	// The handle the wire stream knows the local player by. On a JOINER that is H (the
	// host-assigned wire identity), NOT the local sim handle L — L lives in the joiner's
	// own pool and collides with a host-side slot (e.g. the host player), so excluding L
	// from the wire present would wrongly hide a remote entity. On the host, the local
	// player's own pool-0 handle IS its wire handle.
	if (joiner_) return static_cast<int>(joiner_bridge_.self_wire_handle());
	return (world_ && world_->cached.local_player.valid())
			? static_cast<int>(world_->cached.local_player.packed) : 0;
}

void Simulation::set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
                                      bool p_lean_left, bool p_lean_right, bool p_jump) {
	player_input_.forward = p_forward;
	player_input_.back = p_back;
	player_input_.left = p_left;
	player_input_.right = p_right;
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @ 0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once grounded.
	player_input_.lean_left = p_lean_left;
	player_input_.lean_right = p_lean_right;
	player_input_.jump = p_jump;
	// Stance comes from the sim-owned SELECT latches (request_local_player_stance —
	// the C2S 0x1D apply semantics [orig: @ 0x501c60]).
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @ 0x4df450 — any of the four direction keys
	// sets byte_B7653B (blocks scope-UP on Scoped weapons @ 0x4df29c) and, while
	// SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @ 0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@ 0x4df12d) keeps pinned sights raised].
	const bool move_held = p_forward || p_back || p_left || p_right;
	if (opennova::world::player_view_move_input(player_view_, move_held,
			local_weapon_.active ? local_weapon_.def.flags : 0) &&
			(local_weapon_.def.flags & DEF_WEAPON_FLAG_FORCESCOPED) == 0) {
		if (opennova::world::player_view_set_engaged(player_view_, false,
				(local_weapon_.def.flags2 & DEF_WEAPON_FLAG2_INSET) != 0))
			opennova::world::weapon_fsm_queue_scope_down(
					*active_local_weapon_slot());
	}
	refresh_local_player_view_effects();
}

void Simulation::add_local_player_look(float p_dx_px, float p_dy_px) {
	// The scoped sensitivity reduction divides by the CURRENT zoom magnification —
	// the slot zoom seeded from the def's scope_max_mag [orig: sens /
	// Player_GetClampedWeaponElevation() @ 0x499714, applied while scoped and the
	// binocular view is down; no binoculars input exists yet]. Engaged-at-scope is
	// the sim's own bit; the zoom-adjust keys are an unported tail, so the seed
	// (scope_max_mag) IS the current zoom.
	int32_t scoped_zoom = 0;
	if (!player_view_.binoculars_view_active && local_weapon_.active &&
			player_view_.scope_engaged && local_weapon_.scope_max_mag > 1.0f)
		scoped_zoom = static_cast<int32_t>(local_weapon_.scope_max_mag);
	const bool prone = (stance_latch_ == 2); // [orig: MoveOrder & 0x100 @ 0x4e0ff7]
	// Godot supplies float relative motion; the original consumes whole center-lock
	// pixels. Accumulate the fraction so slow motion is not truncated away.
	look_px_accum_x_ += p_dx_px;
	look_px_accum_y_ += p_dy_px;
	const int32_t dx = static_cast<int32_t>(look_px_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_px_accum_y_);
	look_px_accum_x_ -= static_cast<float>(dx);
	look_px_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	opennova::world::player_look_apply(player_input_.look_heading, player_input_.look_pitch,
	                                   look_settings_, dx, dy, scoped_zoom, prone);
}

void Simulation::set_local_player_mouse(int p_sensitivity, bool p_invert_y) {
	// The mousescale clamp [orig: @ 0x49b19b-0x49b1b9: >= 0x200 -> 0x1FF, <= 0 -> 1].
	int s = p_sensitivity;
	if (s < opennova::world::kMouseSensitivityMin) s = opennova::world::kMouseSensitivityMin;
	if (s > opennova::world::kMouseSensitivityMax) s = opennova::world::kMouseSensitivityMax;
	look_settings_.sensitivity = s;
	look_settings_.invert_y = p_invert_y;
}

bool Simulation::request_local_player_stance(int p_stance) {
	if (p_stance < 0 || p_stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172 gate
	// Entity_CheckWeaponSeatFlags(equipped, 0x40000) @ 0x4e0d8a; the seat-kind-3
	// mount refusal rides the unported mounting slice].
	if (local_weapon_.active && local_weapon_.force_crouch) return false;
	if (stance_latch_ == p_stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and clears
	// the other [orig: NapiNPServerMsg_HandleStanceChange @ 0x501c60: 169 -> crouch,
	// 170 -> prone, 172 -> clear both].
	stance_latch_ = p_stance;
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	// A joiner also SENDS the select — the witnessed key handlers emit one C2S 0x1D
	// with the action id immediately; without it a retail host (and every other
	// client) never sees this player crouch or go prone.
	// [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/@0x4e0e3e]
	if (joiner_ && runtime_) {
		static constexpr uint16_t kStanceActionIds[3] = {0xAC, 0xA9, 0xAA};
		ship_to_host(runtime_->send_stance_change(
				kStanceActionIds[static_cast<size_t>(p_stance)]));
	}
	return true;
}

Vector3 Simulation::get_local_player_position() const {
	if (!world_ || !world_->cached.local_player.valid()) return Vector3();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return Vector3();
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(e->position.x, e->position.z, -e->position.y);
}

int64_t Simulation::get_local_player_heading_bam() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *player = world_->ai->for_handle(world_->cached.local_player);
	return player != nullptr ? static_cast<int64_t>(player->heading) : 0;
}

int Simulation::request_hud_radar_zoom(int p_direction) {
	// The engine control routes the step to the big-map pair while a mode
	// is up (witness at hud::HudMapControl::zoom_step).
	const int step = p_direction < 0 ? -1 : (p_direction > 0 ? 1 : 0);
	return hud_map_control_.zoom_step(step);
}

int Simulation::get_hud_radar_zoom_q16() const {
	return hud_map_control_.zoom_q16;
}

int Simulation::request_hud_map_cycle() {
	// The map_toggle action's three-state cycle (witness at
	// hud::HudMapControl::cycle).
	return hud_map_control_.cycle();
}

int Simulation::get_hud_map_mode() const {
	return hud_map_control_.mode;
}

int Simulation::get_hud_big_zoom_q16() const {
	return hud_map_control_.big_zoom_q16;
}

void Simulation::tick_hud_map_death_gate() {
	// The render gate zeroes the mode whenever the local player is dead;
	// respawn re-opens nothing — only the M key does (witness at
	// hud::HudMapControl::on_local_player_dead).
	if (hud_map_control_.mode == 0) return;
	bool dead = false;
	if (joiner_) {
		// The recipient-specific 0x0A tail is the joiner's authoritative
		// local health channel (the same read run_frame's death edge uses).
		dead = runtime_ != nullptr && runtime_->state().local_health <= 0;
	} else if (world_ && world_->cached.local_player.valid()) {
		const opennova::world::Entity *e =
				world_->registry.get(world_->cached.local_player);
		dead = e != nullptr &&
				((e->flags | e->engine_flags) &
						opennova::world::kEntityFlagDead) != 0;
	}
	if (dead) hud_map_control_.on_local_player_dead();
}

bool Simulation::get_hud_map_flip_180() const {
	// AttribFlags::RotateMap180; the map-side witness is
	// HudMinimapInput::flip_180 (hud/hud_minimap.h).
	return world_ && (world_->mission_attrib_flags & 0x20u) != 0;
}

float Simulation::get_local_player_yaw_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	// Engine heading (BAM32) -> mission yaw degrees, the (90 - heading) convention.
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(p->heading));
}

float Simulation::get_local_player_pitch_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	return static_cast<float>(static_cast<double>(p->pitch) * opennova::world::kDegreesPerBam);
}

int Simulation::get_local_player_body_anim_slot() const {
	if (!world_ || !world_->cached.local_player.valid()) return -1;
	// The same Entity.body_anim_slot the present pass reads for NPC models (written by the
	// infantry motor mirror, infantry.cpp). The avatar is shell-managed and not in the present
	// registry, so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->body_anim_slot : -1;
}

String Simulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_body_anim_slot), this carries
	// stance + jump (anim_idle_crouch / anim_walk_prone_forward / anim_jump_loop / ...), so
	// main_game drives the 3rd-person avatar via play_body_clip(key) for full stance fidelity.
	// [orig: off_8135F0 names ARE the .adm keys without the "anim_" prefix]
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return String();
	return infantry_anim_key(p->inf.anim_state);
}

int Simulation::get_local_player_stance() const {
	// [orig: HUD_BuildEntityInfo @0x4b860c — entity+300 flags 0x200=crouch -> 1,
	// 0x100=prone -> 2]; InfantryState::Stance already carries the icon order
	// (kStand 0 / kCrouch 1 / kProne 2).
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0;
	return static_cast<int>(p->inf.stance);
}

Dictionary Simulation::get_local_player_body_debug() const {
	// Read-only F3 card for the local player's water/eye classification — the
	// exact terms the recoil/spread/aimed-shot gates consume (the shared
	// world::entity_eye_below_water witness), so the panel explains the
	// verdict instead of re-deriving one.
	Dictionary d;
	if (!world_ || !world_->cached.local_player.valid()) return d;
	const opennova::world::Entity *local =
			world_->registry.get(world_->cached.local_player);
	if (local == nullptr) return d;
	const AiEntity *body =
			world_->ai != nullptr
					? world_->ai->for_handle(world_->cached.local_player)
					: nullptr;
	const int32_t body_z = body != nullptr
			? body->pos[2]
			: opennova::world::to_fixed(local->position.z);
	const uint32_t flags = local->flags | local->engine_flags;
	const bool drowning =
			(flags & opennova::world::kEntityFlagDrowning) != 0;
	const bool eye_below = opennova::world::entity_eye_below_water(
			*world_, body_z, local->eye_offset_z);
	constexpr double kQ16 = 65536.0;
	d["body_z"] = static_cast<double>(body_z) / kQ16;
	d["eye_height"] = static_cast<double>(local->eye_offset_z) / kQ16;
	d["eye_z"] =
			static_cast<double>(body_z + local->eye_offset_z) / kQ16;
	d["water_authored"] = world_->env.water_z != 0;
	d["water_z"] = static_cast<double>(world_->env.water_z) / kQ16;
	d["drowning"] = drowning;
	d["eye_below_water"] = eye_below;
	d["submerged"] = drowning || eye_below;
	d["in_air"] = (body != nullptr && body->inf.airborne) ||
			(flags & opennova::world::kEntityFlagInAir) != 0;
	d["stance"] = get_local_player_stance();
	return d;
}

int Simulation::get_local_player_anim_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.clip_phase : 0;
}

String Simulation::get_local_player_anim_source_key() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || !p->inf.body_blend_active()) return String();
	return infantry_anim_key(p->inf.anim_prev);
}

int Simulation::get_local_player_anim_source_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.anim_prev_clip_phase : 0;
}

float Simulation::get_local_player_anim_blend_weight() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 1.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.anim_blend_weight : 1.0f;
}


// HUD health/team. The original rebuilds these into its per-frame HUD info struct every frame
// (health ratio at +92 = currentHealth/maxHealth, team byte at +374). We surface the raw values
// and let the HUD compute the ratio. [orig: HUD_BuildEntityInfo @0x4b8440]
int Simulation::get_local_player_health() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->health : 0;
}

int Simulation::get_local_player_max_health() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 100;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || p->inf.max_health <= 0) return 100;
	return p->inf.max_health;
}

double Simulation::player_eye_min_above_position() {
	return opennova::world::kEyeMinAbovePosition;
}
double Simulation::player_non_person_eye_bump() {
	return opennova::world::kNonPersonEyeBump;
}
int Simulation::player_head_bone_index() {
	return opennova::world::kHeadBoneIndex;
}
double Simulation::player_aim_project_range() {
	return opennova::world::kAimProjectRange;
}

int Simulation::get_local_player_health_percent() const {
	return opennova::world::music_health_percent(get_local_player_health(),
			get_local_player_max_health());
}

int Simulation::get_local_player_team() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->team) : 0;
}

int Simulation::get_local_player_character_id() const {
	// The packed character id the AUTHORITY stamped on the local player
	// (entity+0x15C): the host's own spawn from its installed per-side profile
	// vars, a joiner's from its named 0x0C record — the one word every observer
	// keys the composed head/body (and the local first-person arms) on, so the
	// shell never re-derives side-by-team itself (the stamp's witness lives at
	// server_spawn.cpp / joiner_world_bridge.cpp; the reader side is
	// docs/playerinfo/avatars-re.md, TEX_CAMO section).
	if (world_ && world_->cached.local_player.valid()) {
		const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
		if (e) return static_cast<int>(e->minimap_net_id);
	}
	// A joiner that has learned its record but not yet materialized L (the
	// deploy-screen hold, the challenge prewarm) reads the record itself.
	if (joiner_ && runtime_ && runtime_->has_self_handle()) {
		return static_cast<int>(runtime_->spawn_pose().net_id);
	}
	return 0;
}

int Simulation::get_local_player_class() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->player_class) : 0;
}

String Simulation::get_local_player_weapon_name() const {
	if (!world_ || !world_->cached.local_player.valid()) return String();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return String();
	const opennova::world::WeaponTableEntry *weapon =
			world_->weapons.by_index(e->equipped_adm_index);
	return weapon ? String(weapon->name.c_str()) : String();
}
