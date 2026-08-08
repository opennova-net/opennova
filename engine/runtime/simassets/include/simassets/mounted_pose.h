// The engine-side mounted-pose resolver (S4, ADR 0028): the live UseGun /
// emplacement seat frame from the carrier model's authored userpoint posed
// through its own PANM — the sim-parse counterpart of the shell adapter's
// model-bound resolver. [orig: UseGun Entity_AttachToBoneAndUpdateTransform
// @ 0x5463d0; the addeweap attachment frame build_bone_attachment_matrix
// @ 0x56C630 over build_direction_look_at_matrix @ 0x612C90]
#ifndef OPENNOVA_SIMASSETS_MOUNTED_POSE_H
#define OPENNOVA_SIMASSETS_MOUNTED_POSE_H

#include <threedi/threedi_3di3.h>
#include <world/vehicle_mount.h>

#include <cstdint>

namespace opennova::simassets {

// Resolve the seat's live world pose from the carrier's parsed model:
// the userpoint (seat.bone_index is the 1-based USRP row) is re-anchored into
// its owning part's rest frame, carried by the live PANM part matrix, and
// composed with the carrier's mission euler; an addeweap attachment frame
// (seat.attachment_frame + an authored direction) owns the complete look-at
// orientation, an ordinary gunner seat re-bases the part delta onto the
// carrier baseline. ctrl_values is the 96-slot global register bus (may be
// null); time_ms the retail PANM clock. Mirrors the adapter resolver's
// gates/fallthroughs exactly — false means "use the portable fallback".
bool resolve_model_mounted_pose(const Threedi3di3 &model,
                                const world::Entity &carrier,
                                const world::Seat &seat,
                                const int32_t *ctrl_values, uint32_t time_ms,
                                world::MountedPose &out);

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_MOUNTED_POSE_H
