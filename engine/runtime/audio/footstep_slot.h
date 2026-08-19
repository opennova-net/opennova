#pragma once

#include <cstdint>

#include "audio/sound_profile.h"

namespace opennova::audio {

// THE FOOTSTEP SLOT PICK — which sound-profile slot one authored foot event
// resolves to, in retail's witnessed test order
// [orig: org2 @0x4b77c6-0x4b78a8; the sibling org1 block @0x4bf23e-0x4bf2b0].
//
// The tests run in this order and the FIRST match wins:
//   1. water — a nonzero water plane with the FEET under it (one slot for both
//      feet) [orig: Env_WaterHeightFixed @0x26C6454 read @0x4b77d3];
//   2. on-entity — the body is standing on another entity, i.e. its
//      `groundEntity` link (entity+0x28) is set [orig: the +0x28 store by
//      Entity_RaycastGroundHeightAndObject @0x525fd0 / @0x414370];
//   3. snow — charmap surface type 3 under the feet
//      [orig: Terrain_GetSurfaceTypeAtPosition @0x606510];
//   4. ground — everything else.
//
// `feet_z` is the body position ALREADY dipped to foot level by the frame's
// capsule bottom (the AnimMap out[3] cell both body updaters pass through);
// this function does not dip it. `water_z == 0` means "no water plane".
// `foot` is 0 = left, 1 = right; the water slot ignores it, matching retail.
//
// One implementation shared by both body channels: the authority/AI bodies
// (world::AiSystem::infantry_anim_sound_pass) and the wire-fed remote bodies.
inline int footstep_slot(int32_t feet_z, int32_t water_z, bool on_entity,
                         int32_t surface_type, int foot) {
	if (water_z != 0 && feet_z < water_z) return kSlotFootWater;
	if (on_entity) return foot == 0 ? kSlotFootLObject : kSlotFootRObject;
	if (surface_type == 3) return foot == 0 ? kSlotFootLSnow : kSlotFootRSnow;
	return foot == 0 ? kSlotFootLGround : kSlotFootRGround;
}

} // namespace opennova::audio
