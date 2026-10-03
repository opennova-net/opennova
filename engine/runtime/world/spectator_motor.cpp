#include <runtime/world/spectator_motor.h>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <cmath>

namespace opennova::world {

namespace {

// The free-fly constants as the image stores them: the 16-bit angle to
// radians (dbl_7C9BC0, 2*pi/65536 to the double's rounding), the per-tick
// scale (flt_7C9BB8, 1/62 as a float) and the eighth-turn per direction index
// (dbl_7C9BB0). [orig: @0x4b29a8, @0x4b2a73, @0x4b2a8a]
constexpr double kAngleToRadians = 9.587371826171875e-05;
constexpr float kPerTick = 0.016129031777381897f;
constexpr double kEighthTurn = 0.7853975;

// The MoveOrder word, entity+0x12C, as the port stores its halves: the
// uplink's movement byte, the stance bits above it and the local look keys.
// [orig: Player_PackInputStateToEntity @0x4df68f..0x4df790]
uint32_t move_order_of(const Entity &e) {
	return static_cast<uint32_t>(e.net_move_input) |
			(static_cast<uint32_t>(e.net_stance_bits) << 8) | e.local_view_input;
}

} // namespace

void spectator_free_fly(World &world, AiEntity &local) {
	const SpectatorMotorState &s = world.spectator;
	// The trig angles from the high words: Yaw >> 16 (arithmetic) and the
	// signed high word of Pitch [orig: @0x4b298a..0x4b29bc].
	const double yaw = static_cast<double>(local.heading >> 16) * kAngleToRadians;
	const double pitch =
			static_cast<double>(static_cast<int16_t>(static_cast<uint32_t>(local.pitch) >> 16)) *
			kAngleToRadians;
	if (s.submode != 0) {
		// The follow arm: the target's Position, Yaw, Pitch and Roll onto the
		// local entity; no target, nothing [orig: @0x4b29c0..0x4b2a18].
		if (!s.has_target) return;
		int32_t pos[3] = {s.target_pos[0], s.target_pos[1], s.target_pos[2]};
		int32_t yaw_bam = s.target_yaw, pitch_bam = s.target_pitch, roll_bam = s.target_roll;
		if (s.target_entity.valid()) {
			// A registry target (a player: the walk admits nothing else) is
			// read as its body stands at this point of the pool-0 walk, a
			// lower slot having already moved this tick.
			const AiEntity *body = world.ai.for_handle(s.target_entity);
			if (body == nullptr) return;
			for (int i = 0; i < 3; ++i) pos[i] = body->pos[i];
			yaw_bam = body->heading;
			pitch_bam = body->pitch;
			roll_bam = body->roll;
		}
		for (int i = 0; i < 3; ++i) local.pos[i] = pos[i];
		local.heading = yaw_bam;
		local.pitch = pitch_bam;
		local.roll = roll_bam;
		local.inf.target_heading = yaw_bam;
		local.inf.look_pitch = pitch_bam;
		return;
	}
	const Entity *entity = world.registry.get(local.handle);
	if (entity == nullptr) return;
	const uint32_t move_order = move_order_of(*entity);
	const int32_t dir = static_cast<int32_t>(move_order & 7u);
	int32_t speed = kSpectatorFlySpeed;
	// The fast-flight gate lifts the speed unless prone: 80, or 40 crouched
	// [orig: @0x4b2a35..0x4b2a6b].
	if (s.su_flag && (move_order & 0x100u) == 0)
		speed = (move_order & 0x200u) != 0 ? kSpectatorFlySpeedFastCrouch : kSpectatorFlySpeedFast;
	// The per-tick step, truncated [orig: fild/fmul flt_7C9BB8 @0x4b2a6f, the
	// _ftol2_sse @0x4b2a79].
	int32_t step = io::retail_ftol_sse2(static_cast<double>(speed) * static_cast<double>(kPerTick));
	if ((move_order & 8u) != 0) {
		// The moving bit: along the heading turned by the direction index's
		// eighth turns, the horizontal legs scaled by cos(pitch)
		// [orig: @0x4b2a86..0x4b2ab9].
		const double heading = yaw + static_cast<double>(dir) * kEighthTurn;
		const double cos_pitch = std::cos(pitch);
		local.pos[0] = io::bam_add(local.pos[0],
				io::retail_ftol_sse2(std::cos(heading) * static_cast<double>(step) * cos_pitch));
		local.pos[1] = io::bam_add(local.pos[1],
				io::retail_ftol_sse2(static_cast<double>(step) * std::sin(heading) * cos_pitch));
		// The climb follows the pitch forward, reverses backward (3, 4, 5) and
		// is zero strafing (2, 6) [orig: @0x4b2abc..0x4b2add].
		if (dir >= 3 && dir <= 5) step = -step;
		else if (dir == 2 || dir == 6) step = 0;
		local.pos[2] = io::bam_add(local.pos[2],
				io::retail_ftol_sse2(std::sin(pitch) * static_cast<double>(step)));
	}
	// The lean pair rides three quarters of that step down / up — the climb's
	// sign included [orig: @0x4b2afd..0x4b2b0f].
	const int32_t vertical = (step * 3) >> 2;
	if ((move_order & 0x40u) != 0) local.pos[2] = io::bam_sub(local.pos[2], vertical);
	if ((move_order & 0x80u) != 0) local.pos[2] = io::bam_add(local.pos[2], vertical);
	// The turn keys step Yaw and the local look yaw alike, the look keys Pitch
	// [orig: @0x4b2b13..0x4b2b70].
	if ((move_order & 0x1000u) != 0) {
		local.heading = io::bam_add(local.heading, kSpectatorTurnStep);
		local.inf.target_heading = io::bam_add(local.inf.target_heading, kSpectatorTurnStep);
	}
	if ((move_order & 0x2000u) != 0) {
		local.heading = io::bam_sub(local.heading, kSpectatorTurnStep);
		local.inf.target_heading = io::bam_sub(local.inf.target_heading, kSpectatorTurnStep);
	}
	if ((move_order & 0x4000u) != 0) {
		local.pitch = io::bam_add(local.pitch, kSpectatorTurnStep);
		local.inf.look_pitch = local.pitch;
	}
	if ((move_order & 0x8000u) != 0) {
		local.pitch = io::bam_sub(local.pitch, kSpectatorTurnStep);
		local.inf.look_pitch = local.pitch;
	}
}

void spectator_seat_eye(World &world) {
	AiEntity *body = world.ai.for_handle(world.cached.local_player);
	Entity *entity = world.registry.get(world.cached.local_player);
	if (body == nullptr || entity == nullptr) return;
	body->inf.eye_offset_x = 0;
	body->inf.eye_offset_y = 0;
	body->inf.eye_offset_z = kSpectatorEyeZ;
	entity->eye_offset_x = 0;
	entity->eye_offset_y = 0;
	entity->eye_offset_z = kSpectatorEyeZ;
}

} // namespace opennova::world
