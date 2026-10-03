// The death screen's free-fly motor over the world's own entity update: the
// org2 divert of the local player, the follow arm's pose copy (a registry
// target read as it stands, a joiner row's carried pose), the free arm's
// speeds, directions, lean climb and look keys, and the chase orbit/zoom
// state the sub-mode drives.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b40f8..0x4b411a;
//  Camera_UpdateFreeFly @0x4b2980; Input_HandleActionBinding cases 405..410
//  @0x49c10c..0x49c249; ThirdPersonCamera_Update @0x437c1b..0x437d02;
//  Camera_SetTrackedEntity @0x4391d0; sub_52AD50 @0x52ae4f..0x52ae99]

#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_view.h>
#include <runtime/world/spectator_motor.h>
#include <runtime/world/world.h>

#include <cstdio>

namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

w::PlayerSpawn spawn_at(float x, float y, float z, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = {x, y, z};
	s.net_id = net_id;
	return s;
}

struct Rig {
	w::World world;
	w::EntityHandle local;
	w::EntityHandle remote;
	Rig() {
		world.registry.configure_pool(0, 8);
		local = w::spawn_player(world, spawn_at(10, 20, 30, 0xFFF0));
		remote = w::spawn_remote_player(world, spawn_at(50, 60, 70, 0xFFF1));
	}
	w::AiEntity &body() { return *world.ai.for_handle(local); }
	w::Entity &entity() { return *world.registry.get(local); }
	// The MoveOrder word as the pack leaves it: the movement byte, the stance
	// bits and the look keys, on the registry entity and the body alike.
	void move_order(int dir, bool moving, uint8_t stance_bits = 0, uint16_t view_keys = 0,
			bool lean_left = false, bool lean_right = false) {
		w::AiEntity &b = body();
		b.inf.player_move_dir_index = dir;
		b.inf.player_moving = moving;
		b.inf.lean_left = lean_left;
		b.inf.lean_right = lean_right;
		b.inf.view_input_bits = view_keys;
		b.inf.stance = stance_bits == 1 ? w::InfantryState::Stance::kProne
				: stance_bits == 2 ? w::InfantryState::Stance::kCrouch
								   : w::InfantryState::Stance::kStand;
		w::Entity &e = entity();
		e.net_move_input = static_cast<uint8_t>((dir & 7) | (moving ? 8 : 0) |
				(lean_left ? 0x40 : 0) | (lean_right ? 0x80 : 0));
		e.net_stance_bits = stance_bits;
		e.local_view_input = view_keys;
	}
	void tick() { world.run_logic_tick(/*is_authority=*/true, w::TickPhase::Gameplay); }
};

// The free sub-mode on a level heading: one tick's displacement for a MoveOrder.
struct Step {
	int32_t dx, dy, dz, dyaw, dpitch;
};
Step free_step(int dir, bool moving, uint8_t stance_bits, bool su, int32_t pitch,
		uint16_t view_keys = 0, bool lean_left = false, bool lean_right = false) {
	Rig rig;
	rig.world.spectator.death_screen = true;
	rig.world.spectator.submode = 0;
	rig.world.spectator.su_flag = su;
	w::AiEntity &b = rig.body();
	b.heading = 0;
	b.inf.target_heading = 0;
	b.pitch = pitch;
	b.inf.look_pitch = pitch;
	rig.move_order(dir, moving, stance_bits, view_keys, lean_left, lean_right);
	const int32_t x0 = b.pos[0], y0 = b.pos[1], z0 = b.pos[2];
	rig.tick();
	return {b.pos[0] - x0, b.pos[1] - y0, b.pos[2] - z0, b.heading, b.pitch - pitch};
}

} // namespace

int main() {
	// --- the follow arm: a registry target copied as it stands -------------
	{
		Rig rig;
		w::AiEntity *target = rig.world.ai.for_handle(rig.remote);
		CHECK(target != nullptr);
		target->heading = 0x12345678;
		target->pitch = 0x01000000;
		target->roll = -0x00200000;
		const int32_t tx = target->pos[0], ty = target->pos[1], tz = target->pos[2];
		rig.world.spectator.death_screen = true;
		rig.world.spectator.submode = 1;
		rig.world.spectator.has_target = true;
		rig.world.spectator.target_entity = rig.remote;
		rig.tick();
		// The local slot (0) runs before the target's (1): the pre-tick pose.
		CHECK(rig.body().pos[0] == tx && rig.body().pos[1] == ty && rig.body().pos[2] == tz);
		CHECK(rig.body().heading == 0x12345678);
		CHECK(rig.body().pitch == 0x01000000);
		CHECK(rig.body().roll == -0x00200000);
		// The look staging carries the copied angles past the next input mirror.
		CHECK(rig.body().inf.target_heading == 0x12345678);
		CHECK(rig.body().inf.look_pitch == 0x01000000);
		// The registry mirror (the camera, the HUD, the uplink read it).
		CHECK(w::to_fixed(rig.entity().position.x) == tx);
		CHECK(w::to_fixed(rig.entity().position.y) == ty);
	}
	// --- the follow arm: a joiner row's carried pose -----------------------
	{
		Rig rig;
		w::SpectatorMotorState &s = rig.world.spectator;
		s.death_screen = true;
		s.submode = 2;
		s.has_target = true;
		s.target_pos[0] = 0x100000;
		s.target_pos[1] = -0x200000;
		s.target_pos[2] = 0x30000;
		s.target_yaw = 0x40000000;
		s.target_pitch = -0x01000000;
		s.target_roll = 0x00100000;
		rig.tick();
		CHECK(rig.body().pos[0] == 0x100000 && rig.body().pos[1] == -0x200000 &&
				rig.body().pos[2] == 0x30000);
		CHECK(rig.body().heading == 0x40000000 && rig.body().pitch == -0x01000000 &&
				rig.body().roll == 0x00100000);
	}
	// --- a target sub-mode without a target holds the entity still ----------
	{
		Rig rig;
		const int32_t x0 = rig.body().pos[0], z0 = rig.body().pos[2];
		rig.world.spectator.death_screen = true;
		rig.world.spectator.submode = 1;
		rig.move_order(0, true);
		rig.tick();
		CHECK(rig.body().pos[0] == x0 && rig.body().pos[2] == z0);
	}
	// --- the free arm: the speeds ------------------------------------------
	{
		// Stock 10.0 u/s: 0xA0000 * (float)1/62 truncated = 10570 per tick.
		const Step stock = free_step(0, true, 0, false, 0);
		CHECK(stock.dx == 10570 && stock.dy == 0 && stock.dz == 0);
		// The SU gate: 80.0, 40.0 crouched, the stock speed prone.
		CHECK(free_step(0, true, 0, true, 0).dx == 84562);
		CHECK(free_step(0, true, 2, true, 0).dx == 42281);
		CHECK(free_step(0, true, 1, true, 0).dx == 10570);
		// Without the moving bit nothing moves.
		const Step idle = free_step(0, false, 0, false, 0);
		CHECK(idle.dx == 0 && idle.dy == 0 && idle.dz == 0);
	}
	// --- the free arm: directions, the pitch climb and the lean pair -------
	{
		// Backward: four of the image's eighth-turns (0.7853975) fall short of
		// pi, so the cosine truncates one unit shy.
		const Step back = free_step(4, true, 0, false, 0);
		CHECK(back.dx == -10569);
		// Strafing (direction 6) moves along -Y and never climbs.
		const Step strafe = free_step(6, true, 0, false, 0x08000000);
		CHECK(strafe.dx == 0 && strafe.dz == 0);
		CHECK(strafe.dy < -10000);
		// Forward with the nose up climbs; backward with it up descends.
		const int32_t up = 0x08000000; // 11.25 deg
		const Step fwd_up = free_step(0, true, 0, false, up);
		const Step back_up = free_step(4, true, 0, false, up);
		CHECK(fwd_up.dz > 2000 && back_up.dz < -2000);
		// The lean pair rides three quarters of the step, its sign included:
		// standing still (7927 = 10570 * 3 >> 2), backward the pair inverts
		// (-31710 >> 2 = -7928, the arithmetic shift rounding down), and
		// strafing zeroes it.
		CHECK(free_step(0, false, 0, false, 0, 0, true, false).dz == -7927);
		CHECK(free_step(0, false, 0, false, 0, 0, false, true).dz == 7927);
		CHECK(free_step(4, true, 0, false, 0, 0, true, false).dz == 7928);
		CHECK(free_step(2, true, 0, false, 0, 0, true, false).dz == 0);
	}
	// --- the free arm: the look keys ---------------------------------------
	{
		const Step left = free_step(0, false, 0, false, 0, 0x1000);
		CHECK(left.dyaw == 0x1FFFFFF);
		const Step right = free_step(0, false, 0, false, 0, 0x2000);
		CHECK(right.dyaw == -0x1FFFFFF);
		CHECK(free_step(0, false, 0, false, 0, 0x4000).dpitch == 0x1FFFFFF);
		CHECK(free_step(0, false, 0, false, 0, 0x8000).dpitch == -0x1FFFFFF);
	}
	// --- off the death screen the body is the ordinary org2 motor ----------
	{
		Rig rig;
		rig.move_order(0, false, 0, 0x1000);
		const int32_t heading0 = rig.body().heading;
		rig.tick();
		CHECK(rig.body().heading == heading0); // the free-fly turn step is not taken
	}
	// --- the death screen's eye ---------------------------------------------
	{
		Rig rig;
		w::spectator_seat_eye(rig.world);
		CHECK(rig.entity().eye_offset_x == 0 && rig.entity().eye_offset_y == 0 &&
				rig.entity().eye_offset_z == 0xD000);
		CHECK(rig.body().inf.eye_offset_z == 0xD000);
	}
	// --- the chase orbit/zoom actions ---------------------------------------
	{
		w::PlayerViewState v;
		uint32_t bits = 0;
		CHECK(v.chase_distance_q16 == 0x10000 && v.chase_orbit_pitch == 0);
		w::player_view_chase_action(v, 409, &bits); // max(0x10000 >> 6, 0x800)
		CHECK(v.chase_distance_q16 == 0x10000 - 0x800 && bits == 0x80u);
		for (int i = 0; i < 64; ++i) w::player_view_chase_action(v, 409, &bits);
		CHECK(v.chase_distance_q16 == 0x8000); // the 0.5 floor
		v.chase_distance_q16 = 0x1FF0000;
		w::player_view_chase_action(v, 410, &bits);
		CHECK(v.chase_distance_q16 == 0x2000000 && (bits & 0x200u) != 0); // the 512 cap
		for (int i = 0; i < 130; ++i) w::player_view_chase_action(v, 408, &bits);
		CHECK(v.chase_orbit_pitch == 0x40000000 && (bits & 0x4u) != 0);
		v.chase_orbit_pitch = 0;
		for (int i = 0; i < 100; ++i) w::player_view_chase_action(v, 407, &bits);
		CHECK(v.chase_orbit_pitch == static_cast<int32_t>(0xD0000000u) && (bits & 0x100u) != 0);
		bits = 0;
		w::player_view_chase_action(v, 405, &bits);
		w::player_view_chase_action(v, 406, &bits);
		CHECK(bits == 0x50u);
		// The per-tick yaw from those bits.
		w::player_view_chase_tick(v, 0x10u, false);
		CHECK(v.chase_orbit_yaw == -0x1000000);
		w::player_view_chase_tick(v, 0x40u, false);
		CHECK(v.chase_orbit_yaw == 0);
		// The mouse orbit: the yaw free, the pitch clamped to +-80 deg.
		w::player_view_chase_orbit_look(v, 0x00100000, 0x7F000000);
		CHECK(v.chase_orbit_yaw == 0x00100000 && v.chase_orbit_pitch == 0x38E38E00);
	}
	// --- the tracked entity: the reseed, the dead start and the reel -------
	{
		w::PlayerViewState v;
		w::player_view_track_entity(v, 0, false, false);
		CHECK(v.chase_distance_q16 == 0x10000); // the local player, unchanged
		w::player_view_track_entity(v, w::kCameraTrackedTargetKey | 3, true, false);
		CHECK(v.chase_orbit_pitch == 0x4000000 && v.chase_distance_q16 == 0x30000);
		w::player_view_track_entity(v, 0, true, true);
		CHECK(v.camera_tracked == 0 && v.chase_distance_q16 == 0xA0000);
		w::player_view_chase_tick(v, 0, true);
		CHECK(v.chase_distance_q16 == 0x90000);
		for (int i = 0; i < 3; ++i) w::player_view_chase_tick(v, 0, true);
		CHECK(v.chase_distance_q16 == 0x70000 - 0x4000); // 7.0 then a sixteenth of 4.0
		for (int i = 0; i < 400; ++i) w::player_view_chase_tick(v, 0, true);
		CHECK(v.chase_distance_q16 == 0x30000);
	}
	if (failures != 0) {
		std::printf("spectator_motor: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("spectator_motor: ok\n");
	return 0;
}
