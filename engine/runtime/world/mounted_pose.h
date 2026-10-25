// The engine-side mounted-pose resolver (S4, ADR 0028): the live UseGun /
// emplacement seat frame from the carrier model's authored userpoint posed
// through its own PANM — the sim-parse counterpart of the shell binding's
// model-bound resolver. [orig: UseGun Entity_AttachToBoneAndUpdateTransform
// @ 0x5463d0; the addeweap attachment frame Bone_BuildAttachmentMatrix
// @ 0x56C630 over Math_BuildDirectionLookAtMatrix @ 0x612C90]
#pragma once

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/world/vehicle_mount.h>

#include <cstdint>
#include <vector>

namespace opennova::world {

// The CTRL register-bus composition feeding resolve_model_mounted_pose: the
// same three sources the retail resolver publishes, written by ordinal onto
// the 96-slot bus — the part-anim phase pair (VEHICLE_SPECIAL1 gated OFF by
// the carrier's ItemDefAttrib 0x1000 bit, SPECIAL2 always), the heat glow,
// and the emplaced gun yaw/pitch.
struct MountedPoseControlSources {
    int32_t part_anim_phase0 = 0;
    int32_t part_anim_phase1 = 0;
    bool has_heat_glow = false;
    int32_t heat_glow = 0;
    bool has_emplaced = false;
    float emplaced_gun_yaw = 0.0f;
    float emplaced_gun_pitch = 0.0f;
    uint16_t emplaced_spin_phase = 0;
};

void compose_mounted_pose_controls(
        uint32_t carrier_item_attrib, const MountedPoseControlSources &sources,
        int32_t (&r_ctrl)[opennova::threedi::THREEDI_CTRL_REGISTER_COUNT]);

// The vehicle render class's CTRL callback (def+0x144), which retail runs
// before every userpoint transform and bone attachment frame of that entity.
// A UseGun seat attachment does not call it; there the projection stands for
// the global bus value the carrier's last class publication left.
void compose_vehicle_pose_controls(World &world, const Entity &carrier,
        int32_t (&r_ctrl)[opennova::threedi::THREEDI_CTRL_REGISTER_COUNT]);

// The resolver's PANM clock: retail's 16 ms logic-tick time, unless a debug
// override (>= 0) pins it.
uint32_t mounted_pose_time_ms(uint32_t logic_tick, int64_t override_ms);

using MountedPosePartMatrices = std::vector<opennova::threedi::ThreediMatrix4x4>;

// Evaluate the complete LOD-0 part pose once. A carrier can expose several
// attachment/seat userpoints; callers may reuse these matrices for every seat
// instead of re-running PANM for each one.
bool evaluate_model_mounted_pose_parts(
        const opennova::threedi::Threedi3di3 &model, uint32_t time_ms,
        const int32_t *ctrl_values, MountedPosePartMatrices &out);

// Resolve one addeweap attachment frame (a Gunner seat with attachment_frame) from
// already evaluated rest/live part matrices; any other seat is false.
bool resolve_model_mounted_pose_from_parts(
        const opennova::threedi::Threedi3di3 &model, const world::Entity &carrier,
        const world::Seat &seat, const MountedPosePartMatrices &rest_parts,
        const MountedPosePartMatrices &live_parts, world::MountedPose &out);

// The UseGun rider's seat frame (a Gunner seat without attachment_frame) as the UseGun
// carry builds it each tick: the carrier's Q22 placement matrix as render floats in
// every bone slot, the model's bone pass over them (threedi_panm_pose_parts_over, LOD 0),
// the userpoint's part (its subobject index) back to Q22 / 16.16, the userpoint's
// position words through that matrix (out.position) and that matrix's own Yaw, Pitch and
// Roll (out.heading / pitch / roll). The userpoint's direction does not enter. False for
// any other seat, a missing userpoint, or a part index outside the LOD's parts.
// [orig: Entity_AttachToBoneAndUpdateTransform @0x5463D0 - Model_TransformBoneMatrices
// @0x54652B, Math_FloatMatrixToFixedPoint22 @0x54653E, Math_FixedPointTransformPoint22
// @0x546564, Math_FixedPointMatrixToEulerAngles @0x54656F]
bool resolve_usegun_seat_pose(const opennova::threedi::Threedi3di3 &model,
        const world::Entity &carrier, const world::Seat &seat,
        const int32_t *ctrl_values, uint32_t time_ms, world::MountedPose &out);

// Resolve the seat's live world pose from the carrier's parsed model. A UseGun
// seat goes to resolve_usegun_seat_pose. For an addeweap attachment frame the
// userpoint (seat.bone_index is the 1-based USRP row) is re-anchored into its
// owning part's rest frame, carried by the live PANM part matrix, and composed
// with the carrier's mission euler; an authored direction owns the complete
// look-at orientation, a zero one re-bases the part delta onto the carrier
// baseline. ctrl_values is the 96-slot global register bus (may be null); time_ms
// the retail PANM clock. False means "use the portable fallback".
bool resolve_model_mounted_pose(const opennova::threedi::Threedi3di3 &model,
                                const world::Entity &carrier,
                                const world::Seat &seat,
                                const int32_t *ctrl_values, uint32_t time_ms,
                                world::MountedPose &out);

// The seat bone of a carrier whose model's bone callback hands every bone slot the
// carrier's own orientation matrix (an ItemType_Vehicle model's BoneCallback_Simple), as
// the sitex / ctrlx / drvrx carry and the attach request read it: the seat userpoint's
// position words (Seat::bone_point) through the carrier's Q22 placement matrix
// (entity_placement_matrix), the heading the fpatan of its direction words
// (Seat::bone_direction) rotated by that matrix, truncated as the CRT's _ftol2_sse does,
// and the pitch and roll that matrix's own eulers. No part animation reaches such a
// seat. [orig: EntityDef_LoadModelsAndCallbacks @0x439F50 (the vehicle's
// BoneCallback_Simple @0x43A072); BoneCallback_Simple @0x4E2600 (the copies
// @0x4E267E..0x4E26B7); Entity_GetBoneTransformAndOrientation @0x4B0C50 -
// Math_FixedPointTransformPoint22 @0x4B0D42, Math_TransformPointFixedPoint22 @0x4B0D64,
// the fpatan @0x4B0D69..0x4B0D7C and its store @0x4B0D81,
// Math_FixedPointMatrixToEulerAngles @0x4B0DA1 (pitch @0x4B0DB1, roll @0x4B0DBB)]
void seat_bone_pose_on_carrier_matrix(const world::Entity &carrier, const world::Seat &seat,
                                      world::MountedPose &out);

} // namespace opennova::world
