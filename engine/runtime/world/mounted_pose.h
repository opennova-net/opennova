// The engine-side mounted-pose resolver (S4, ADR 0028): the live UseGun /
// emplacement seat frame from the carrier model's authored userpoint posed
// through its own PANM — the sim-parse counterpart of the shell binding's
// model-bound resolver. [orig: UseGun Entity_AttachToBoneAndUpdateTransform
// @ 0x5463d0; the addeweap attachment frame build_bone_attachment_matrix
// @ 0x56C630 over build_direction_look_at_matrix @ 0x612C90]
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

// Resolve one seat from already evaluated rest/live part matrices.
bool resolve_model_mounted_pose_from_parts(
        const opennova::threedi::Threedi3di3 &model, const world::Entity &carrier,
        const world::Seat &seat, const MountedPosePartMatrices &rest_parts,
        const MountedPosePartMatrices &live_parts, world::MountedPose &out);

// Resolve the seat's live world pose from the carrier's parsed model:
// the userpoint (seat.bone_index is the 1-based USRP row) is re-anchored into
// its owning part's rest frame, carried by the live PANM part matrix, and
// composed with the carrier's mission euler; an addeweap attachment frame
// (seat.attachment_frame + an authored direction) owns the complete look-at
// orientation, an ordinary gunner seat re-bases the part delta onto the
// carrier baseline. ctrl_values is the 96-slot global register bus (may be
// null); time_ms the retail PANM clock. Mirrors the binding resolver's
// gates/fallthroughs exactly — false means "use the portable fallback".
bool resolve_model_mounted_pose(const opennova::threedi::Threedi3di3 &model,
                                const world::Entity &carrier,
                                const world::Seat &seat,
                                const int32_t *ctrl_values, uint32_t time_ms,
                                world::MountedPose &out);

} // namespace opennova::world
