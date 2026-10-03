#include <runtime/inmatch/spectator_session.h>

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/player_view.h>
#include <runtime/world/spectator_motor.h>

namespace opennova::inmatch {

void stamp_spectator_motor(mission::MissionKernel &kernel, const ClientRuntime *runtime,
		bool joiner) {
	world::World &world = kernel.world;
	world::SpectatorMotorState &s = world.spectator;
	s.death_screen = false;
	s.submode = 0;
	s.has_target = false;
	s.target_entity = world::EntityHandle{};
	s.su_flag = false;
	if (runtime == nullptr) return;
	const replication::ClientState &cs = runtime->state();
	s.death_screen = cs.death_screen_active;
	s.submode = cs.death_screen_submode;
	s.su_flag = cs.scoreboard_status_suffix != 0;
	if (cs.spectate_target != 0xFFFF) {
		if (!joiner) {
			// The authority's replica rows carry its own world's handles.
			s.has_target = true;
			s.target_entity = world::EntityHandle{cs.spectate_target};
		} else if (const replication::ClientEntityState *row = cs.find(cs.spectate_target)) {
			// A remote person lives in the replica only: its row's pose, the
			// rows having folded this frame's records already.
			s.has_target = true;
			s.target_pos[0] = row->x;
			s.target_pos[1] = row->y;
			s.target_pos[2] = row->z;
			s.target_yaw = row->heading_bam;
			s.target_pitch = row->pitch_bam;
			s.target_roll = row->roll_bam;
		}
	}
	// The rising edge's eye, once the local entity exists to take it (the
	// port's joiner spawns its entity after the session starts; retail's is
	// there from the mission start).
	if (cs.death_screen_opens != s.death_screen_opens_seen && cs.death_screen_active &&
			world.cached.local_player.valid()) {
		s.death_screen_opens_seen = cs.death_screen_opens;
		world::spectator_seat_eye(world);
	}
}

void apply_death_screen_input(mission::MissionKernel &kernel, ClientRuntime &runtime,
		bool joiner, const InputPacket &input) {
	world::LocalPlayer &lp = kernel.local;
	world::World &world = kernel.world;
	const world::SpectatorMotorState &s = world.spectator;
	world::PlayerInput keys = input.movement;
	float look_x = input.look_delta_x;
	float look_y = input.look_delta_y;
	// The fire (row 43), reload (row 47) and medic (row 64) rows are mode-1
	// rows; jump (row 8) is too.
	lp.set_weapon_input(false, false, false);
	keys.jump = false;
	const bool dead = joiner ? runtime.local_player_dead() : lp.local_player_dead();
	if (dead || runtime.state().deploy_overlay_active) {
		// Every movement and look row carries the action flag bits 0 and
		// 0x8000000 the dispatcher refuses on a dead local player and while
		// the deploy screen is up — which a join-time spectator in a mission
		// with spawn zones holds for good (its deploy-hold bit, 0x0A flags1
		// bit 1) [orig: @0x49ad7e / @0x49ada0 (dead), @0x49add7
		// (g_DeployScreenActive); catalog rows 0..16 flags 0x0C..05].
		keys = world::PlayerInput{};
		look_x = look_y = 0.0f;
	} else if (s.follows_target()) {
		if (s.submode == 1) {
			// The chase: the mouse onto the orbit (the axis pass runs ahead of
			// the queued keys), then the keys onto the orbit/zoom actions in
			// catalog row order [orig: Input_ProcessFrame @0x49d58b ahead of
			// the dispatch @0x49d5d8; sub_52AD50 @0x52ada2..0x52ae99 — 0x98
			// forward -> 409, 0x97 back -> 410, 0x9C / 0x9E strafe / turn left
			// -> 405, 0x9D / 0x9F right -> 406, 0x9A look down -> 408, 0x9B
			// look up -> 407].
			if (look_x != 0.0f || look_y != 0.0f) lp.look_chase_orbit(look_x, look_y);
			uint32_t *bits = &world.script.input_action_bits;
			if (keys.forward) world::player_view_chase_action(lp.view, 409, bits);
			if (keys.back) world::player_view_chase_action(lp.view, 410, bits);
			if (keys.left) world::player_view_chase_action(lp.view, 405, bits);
			if (keys.right) world::player_view_chase_action(lp.view, 406, bits);
			if (keys.look_down) world::player_view_chase_action(lp.view, 408, bits);
			if (keys.look_up) world::player_view_chase_action(lp.view, 407, bits);
			if (keys.turn_left) world::player_view_chase_action(lp.view, 405, bits);
			if (keys.turn_right) world::player_view_chase_action(lp.view, 406, bits);
		}
		// Both target sub-modes consume the movement and look rows
		// [orig: sub_52AD50 @0x52aeb4 (first person)].
		keys.forward = keys.back = keys.left = keys.right = false;
		keys.look_up = keys.look_down = keys.turn_left = keys.turn_right = false;
		look_x = look_y = 0.0f;
	}
	// A chase or first-person sub-mode without a target walks the target list
	// in sub_52AD50 (@0x52ad75..0x52ad79), but every writer that leaves either
	// sub-mode leaves a target (docs/interface/hud-re.md, the spectate state),
	// so that leg is unreachable and the keys pass as in the free sub-mode.
	lp.set_movement_keys(keys.forward, keys.back, keys.left, keys.right, keys.lean_left,
			keys.lean_right, keys.jump);
	lp.set_view_keys(keys.free_look, keys.look_up, keys.look_down, keys.turn_left,
			keys.turn_right);
	if (look_x != 0.0f || look_y != 0.0f) lp.look(look_x, look_y);
}

void spectate_action(mission::MissionKernel &kernel, ClientRuntime &runtime, int code) {
	if (runtime.view().spectate_action(code)) kernel.local.place_on_composed_view();
}

} // namespace opennova::inmatch
