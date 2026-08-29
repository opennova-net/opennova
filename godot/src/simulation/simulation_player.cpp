// Simulation — the LOCAL PLAYER core: input and spawn, pose/state getters.
// The view-effects, equipped-weapon FSM, and loadout clusters live in the
// sibling simulation_player_{view,weapon,loadout}.cpp TUs.
#include "simulation/simulation_internal.h"

#include <formats/mission/bms.h>
#include <net/npwire/game_type.h>
#include <runtime/world/music_vars.h>
#include <runtime/world/player_view.h>

#include <formats/def/def.h> // DEF_WEAPON_FLAG_* / DEF_WEAPON_FLAG2_*

using namespace sim_internal;

bool Simulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!world_installed_ || !kernel_->world.ai) return false;
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
	return kernel_->spawn_local_player(spawn);
}

uint32_t Simulation::mission_game_type() const {
	// The engine's one derivation (game_type::for_mission_attribs) over the
	// world's retained mission attribs.
	return opennova::game_type::for_mission_attribs(
			kernel_ ? kernel_->world.mission_attrib_flags : 0u);
}

int Simulation::spawn_local_player_at_start() {
	if (!world_installed_) return -1;
	// The npruntime listen server auto-spawns the host's own player at
	// bring-up via the SAME retail spawn-pose operation
	// (Server_BuildPlayerInfoAndAdd), so when a player already exists this is
	// a no-op success. The bare path runs the kernel's marker-select spawn
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 ->
	// Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c].
	if (has_local_player()) return 1;
	const int status = kernel_->spawn_local_player_at_start(mission_game_type());
	reset_local_player_view_effects();
	return status;
}

bool Simulation::has_local_player() const {
	return kernel_ && kernel_->world.cached.local_player.valid();
}

int Simulation::get_local_player_wire_handle() const {
	// The handle the wire stream knows the local player by. On a JOINER that is H (the
	// host-assigned wire identity), NOT the local sim handle L — L lives in the joiner's
	// own pool and collides with a host-side slot (e.g. the host player), so excluding L
	// from the wire present would wrongly hide a remote entity. On the host, the local
	// player's own pool-0 handle IS its wire handle.
	if (joiner_) return static_cast<int>(joiner_bridge_.self_wire_handle());
	return (kernel_ && kernel_->world.cached.local_player.valid())
			? static_cast<int>(kernel_->world.cached.local_player.packed) : 0;
}

// One frame of movement keys: the kernel folds the sim-owned stance latch in
// and runs the witnessed movement-held unscope [orig:
// Player_PackInputStateToEntity @ 0x4df450]; this binding only converts the
// device booleans.
void Simulation::set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
                                      bool p_lean_left, bool p_lean_right, bool p_jump) {
	kernel_->set_movement_keys(p_forward, p_back, p_left, p_right,
			p_lean_left, p_lean_right, p_jump);
}

// One frame of mouse pixels onto the kernel's look accumulator
// (the center-lock accumulator, the scoped sensitivity reduction, the prone
// up-limit) [orig: Input_ProcessMouseAxisBindings @ 0x499680].
void Simulation::add_local_player_look(float p_dx_px, float p_dy_px) {
	kernel_->look(p_dx_px, p_dy_px);
}

void Simulation::set_local_player_mouse(int p_sensitivity, bool p_invert_y) {
	// The mousescale clamp [orig: @ 0x49b19b-0x49b1b9: >= 0x200 -> 0x1FF, <= 0 -> 1].
	int s = p_sensitivity;
	if (s < opennova::world::kMouseSensitivityMin) s = opennova::world::kMouseSensitivityMin;
	if (s > opennova::world::kMouseSensitivityMax) s = opennova::world::kMouseSensitivityMax;
	kernel_->look_settings.sensitivity = s;
	kernel_->look_settings.invert_y = p_invert_y;
}

bool Simulation::request_local_player_stance(int p_stance) {
	// The SELECT gates and the mutual-exclusion latch are the kernel's
	// [orig: NapiNPServerMsg_HandleStanceChange @ 0x501c60].
	if (!kernel_->request_stance(p_stance)) return false;
	// A joiner also SENDS the select — the witnessed key handlers emit one C2S
	// 0x1D with the action id immediately; without it a retail host (and every
	// other client) never sees this player crouch or go prone.
	// [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/@0x4e0e3e]
	if (joiner_ && runtime_) {
		static constexpr uint16_t kStanceActionIds[3] = {0xAC, 0xA9, 0xAA};
		ship_to_host(runtime_->send_stance_change(
				kStanceActionIds[static_cast<size_t>(p_stance)]));
	}
	return true;
}

Vector3 Simulation::get_local_player_position() const {
	if (!kernel_->world.cached.local_player.valid()) return Vector3();
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (!e) return Vector3();
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(e->position.x, e->position.z, -e->position.y);
}

int64_t Simulation::get_local_player_heading_bam() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0;
	const AiEntity *player = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
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
	// The joiner reads its recipient-specific 0x0A health tail, the authority
	// its entity flags — the one local_player_dead() seam.
	if (local_player_dead()) hud_map_control_.on_local_player_dead();
}

bool Simulation::get_hud_map_flip_180() const {
	// AttribFlags::RotateMap180; the map-side witness is
	// HudMinimapInput::flip_180 (hud/hud_minimap.h).
	return kernel_ && (kernel_->world.mission_attrib_flags & 0x20u) != 0;
}

float Simulation::get_local_player_yaw_deg() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0.0f;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	if (!p) return 0.0f;
	// Engine heading (BAM32) -> mission yaw degrees, the (90 - heading) convention.
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(p->heading));
}

float Simulation::get_local_player_pitch_deg() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0.0f;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	if (!p) return 0.0f;
	return static_cast<float>(static_cast<double>(p->pitch) * opennova::world::kDegreesPerBam);
}

int Simulation::get_local_player_body_anim_slot() const {
	if (!kernel_->world.cached.local_player.valid()) return -1;
	// The same Entity.body_anim_slot the present pass reads for NPC models (written by the
	// infantry motor mirror, infantry.cpp). The avatar is shell-managed and not in the present
	// registry, so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	return e ? e->body_anim_slot : -1;
}

String Simulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_body_anim_slot), this carries
	// stance + jump (anim_idle_crouch / anim_walk_prone_forward / anim_jump_loop / ...), so
	// main_game drives the 3rd-person avatar via play_body_clip(key) for full stance fidelity.
	// [orig: off_8135F0 names ARE the .adm keys without the "anim_" prefix]
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return String();
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	if (!p) return String();
	return infantry_anim_key(p->inf.anim_state);
}

int Simulation::get_local_player_stance() const {
	// [orig: HUD_BuildEntityInfo @0x4b860c — entity+300 flags 0x200=crouch -> 1,
	// 0x100=prone -> 2]; InfantryState::Stance already carries the icon order
	// (kStand 0 / kCrouch 1 / kProne 2).
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	if (!p) return 0;
	return static_cast<int>(p->inf.stance);
}

int Simulation::get_local_player_anim_phase_ticks() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	return p ? p->inf.clip_phase : 0;
}

String Simulation::get_local_player_anim_source_key() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return String();
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	if (!p || !p->inf.body_blend_active()) return String();
	return infantry_anim_key(p->inf.anim_prev);
}

int Simulation::get_local_player_anim_source_phase_ticks() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 0;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	return p ? p->inf.anim_prev_clip_phase : 0;
}

float Simulation::get_local_player_anim_blend_weight() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 1.0f;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
	return p ? p->inf.anim_blend_weight : 1.0f;
}


// HUD health/team. The original rebuilds these into its per-frame HUD info struct every frame
// (health ratio at +92 = currentHealth/maxHealth, team byte at +374). We surface the raw values
// and let the HUD compute the ratio. [orig: HUD_BuildEntityInfo @0x4b8440]
int Simulation::get_local_player_health() const {
	if (!kernel_->world.cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	return e ? e->health : 0;
}

int Simulation::get_local_player_max_health() const {
	if (!kernel_->world.ai || !kernel_->world.cached.local_player.valid()) return 100;
	const AiEntity *p = kernel_->world.ai->for_handle(kernel_->world.cached.local_player);
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
	if (!kernel_->world.cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
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
	if (kernel_ && kernel_->world.cached.local_player.valid()) {
		const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
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
	if (!kernel_->world.cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	return e ? static_cast<int>(e->player_class) : 0;
}

String Simulation::get_local_player_weapon_name() const {
	if (!kernel_->world.cached.local_player.valid()) return String();
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (!e) return String();
	const opennova::world::WeaponTableEntry *weapon =
			kernel_->world.weapons.by_index(e->equipped_adm_index);
	return weapon ? String(weapon->name.c_str()) : String();
}

// The MCP/probe mirror of the attach-command seat selection: the engine's own
// rules (world/vehicle_attach.h predict_seat_selection) over a flat seat list,
// presented as the tooling card {command, seat_index, seat, candidates}.
Dictionary Simulation::predict_mount_seat(const Array &p_seats, int p_command_id) {
	using opennova::world::SeatCandidate;
	using opennova::world::SeatSelectionMode;
	using opennova::world::SeatType;
	using opennova::world::SeatVerdict;
	SeatSelectionMode mode = SeatSelectionMode::Any;
	const bool mount_command =
			opennova::world::seat_selection_mode_for_command(p_command_id, mode);
	std::vector<SeatCandidate> candidates;
	candidates.reserve(static_cast<size_t>(p_seats.size()));
	for (int64_t i = 0; i < p_seats.size(); ++i) {
		const Dictionary seat = p_seats[i];
		SeatCandidate c;
		c.type = static_cast<SeatType>(
				static_cast<int>(seat.get("type", static_cast<int>(SeatType::None))));
		c.occupied = bool(seat.get("occupied", false));
		candidates.push_back(c);
	}
	std::vector<SeatVerdict> verdicts;
	const int best = opennova::world::predict_seat_selection(
			candidates, mount_command ? &mode : nullptr, verdicts);
	const auto type_label = [](SeatType t) -> String {
		switch (t) {
			case SeatType::Passenger: return "passenger";
			case SeatType::Controller: return "controller";
			case SeatType::Gunner: return "gunner";
			case SeatType::Driver: return "driver";
			default: return "none";
		}
	};
	Array rows;
	for (int64_t i = 0; i < p_seats.size(); ++i) {
		const Dictionary seat = p_seats[i];
		Dictionary row = seat.duplicate(true);
		const SeatCandidate &c = candidates[static_cast<size_t>(i)];
		const SeatVerdict v = verdicts[static_cast<size_t>(i)];
		row["index"] = static_cast<int>(i);
		row["type_label"] = type_label(c.type);
		row["eligible"] = v == SeatVerdict::kEligible || v == SeatVerdict::kSelected;
		// A None type never weighs in; the card shows it as the never-picked sentinel.
		row["weight"] = c.type == SeatType::None
				? 0x7fffffff
				: opennova::world::seat_priority_weight(c.type, true);
		row["status"] = v == SeatVerdict::kSelected ? "selected"
				: (v == SeatVerdict::kEligible ? "eligible" : "skipped");
		row["skip_reason"] = v == SeatVerdict::kSkippedOccupied ? "occupied"
				: (v == SeatVerdict::kSkippedCommand ? "command_filter" : "");
		rows.push_back(row);
	}
	Dictionary command;
	command["id"] = p_command_id;
	if (!mount_command) {
		command["mode"] = "not_mount_command";
		command["description"] = "not an attach-to-seat command";
	} else if (mode == SeatSelectionMode::PassengerOnly) {
		command["mode"] = "passenger_only";
		command["description"] = "command 123 accepts sitex/passenger seats only";
	} else if (mode == SeatSelectionMode::RejectController) {
		command["mode"] = "no_controller";
		command["description"] = "command 124 rejects ctrlx/controller seats";
	} else {
		command["mode"] = "any_seat";
		command["description"] =
				"command 125 accepts passenger, controller, driver, and gunner seats";
	}
	Dictionary out;
	out["command"] = command;
	out["seat_index"] = best;
	out["seat"] = best >= 0 ? Dictionary(rows[best]) : Dictionary();
	out["candidates"] = rows;
	return out;
}
