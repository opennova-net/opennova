// Seat/mount pose predicates + the aim-overlay input builder. Moved verbatim
// from the shell adapter's simulation internals (ADR 0028): every input is
// world/anim state, and the legacy render/collision paths and the engine-side
// pose provider must select the same channels (ADR 0016 one-impl).
#ifndef OPENNOVA_SIMASSETS_POSE_INPUTS_H
#define OPENNOVA_SIMASSETS_POSE_INPUTS_H

#include <anim/aim_overlay.h>
#include <world/ai.h>
#include <world/entity.h>
#include <world/infantry.h>

namespace opennova::simassets {

inline bool seat_type_blocks_weapon_channel(world::SeatType type) {
	switch (type) {
		case world::SeatType::Controller:
		case world::SeatType::Gunner:
		case world::SeatType::Driver:
			return true;
		default:
			return false; // passenger seats retain the on-foot upper-body channel
	}
}

inline bool mount_blocks_weapon_channel(const world::Entity &entity) {
	return entity.mounted && seat_type_blocks_weapon_channel(entity.mount_type);
}

inline bool mount_collapses_right_hand_row(const world::Entity &entity) {
	// This terminal skeletal row is stricter than the secondary-channel gate:
	// retail requires a controller/gunner/driver parent slot AND no Flags 0x100.
	// In the port, engine_flags is the authoritative entity+0x24 Flags mirror.
	return entity.mounted &&
			seat_type_blocks_weapon_channel(entity.mount_type) &&
			(entity.engine_flags & world::kEntityFlagPlayer) == 0;
}

inline anim::MountMode mount_mode_for_seat_type(world::SeatType seat_type) {
	switch (seat_type) {
		case world::SeatType::Controller:
		case world::SeatType::Driver:
			return anim::MountMode::Seated;
		case world::SeatType::Gunner:
			return anim::MountMode::Gunner;
		default:
			return anim::MountMode::OnFoot;
	}
}

inline anim::MountMode mount_mode_for(const world::Entity &entity) {
	return entity.mounted
			? mount_mode_for_seat_type(entity.mount_type)
			: anim::MountMode::OnFoot;
}

inline anim::AimOverlayInputs aim_overlay_inputs_for(
		const world::AiEntity &entity, const world::Entity &world_entity) {
	anim::AimOverlayInputs in;
	in.aim_yaw = entity.heading;
	in.aim_pitch = entity.pitch;
	in.body_yaw = entity.inf.body_heading;
	in.leg_yaw_r = entity.inf.leg_yaw[0];
	in.leg_yaw_l = entity.inf.leg_yaw[1];
	in.pitch_kick_accum = entity.inf.pitch_kick_accum;
	// The body overlay consumes the undoubled recoil accumulator. The camera is
	// the separate consumer that adds 2*R. [orig: entity+0x380 read
	// @0x4b1bce; the FP person leg pitch = entPitch + 2*(+0x380) in
	// Camera_ComputeThirdPersonView @0x437fc0..0x437fc7]
	in.pitch_blend = entity.inf.recoil_pitch;
	in.lean = entity.inf.lean_angle;
	in.roll = entity.roll;
	in.body_pitch = entity.body_pitch;
	in.torso_roll = entity.inf.torso_roll;
	in.aim_state =
			(world::infantry_anim_flags(entity.inf.anim_state) & 0x40u) != 0;
	in.rolling =
			entity.inf.anim_state == world::anim_state::kRollLeft ||
			entity.inf.anim_state == world::anim_state::kRollRight;
	in.mount_mode = mount_mode_for(world_entity);
	if (in.mount_mode != anim::MountMode::OnFoot) {
		in.mount_config_valid = world_entity.mounted_config_valid;
		in.mount_config = world_entity.mounted_config_valid
				? world_entity.mounted_config
				: 0;
	}
	return in;
}

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_POSE_INPUTS_H
