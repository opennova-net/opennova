// Seat/mount pose predicates + the aim-overlay input builder. Moved verbatim
// from the shell binding's simulation internals (ADR 0028): every input is
// world/anim state, and the legacy render/collision paths and the engine-side
// pose provider must select the same channels (ADR 0016 one-impl).
#ifndef OPENNOVA_SIMASSETS_POSE_INPUTS_H
#define OPENNOVA_SIMASSETS_POSE_INPUTS_H

#include <runtime/anim/aim_overlay.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/vehicle_mount.h>

namespace opennova::simassets {

// The seat-type channel predicates moved into world (vehicle_mount.h, S7a);
// the using declarations keep this header's consumers unchanged.
using world::mount_blocks_weapon_channel;
using world::mount_collapses_right_hand_row;
using world::seat_type_blocks_weapon_channel;

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
	// The ladder latch locks the arms onto the body animation — both hands on
	// the rungs. [orig: Entity_BuildBoneTransformMatrices @ 0x4b1cf1 tests
	// Flags & 0x100000]
	in.arms_locked =
			((world_entity.flags | world_entity.engine_flags) &
			 world::kEntityFlagLadderContact) != 0;
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
