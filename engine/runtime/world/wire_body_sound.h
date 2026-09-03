// The wire-fed remote body's footstep/foley consume — the presentation-side
// sibling of AiSystem::infantry_anim_sound_pass (world/infantry_sound.cpp). A wire
// row has no AiEntity, so its identity arrives as the items.def type id and
// the packed avatar character id, and its profile resolves through
// world.organic_sound_profiles (audio/footstep_slot.h organic_slot_set).
#pragma once

#include <cstdint>

namespace opennova::world {

class World;

// Consume the authored trigger words a wire body's clip playhead crossed and
// queue the witnessed slot sounds into world.out.slot_sounds. One word per
// authored clip frame ENTERED, and inside each word the foley block precedes
// the foot block [orig: org1 foley @0x4bf169-0x4bf23e then feet
// @0x4bf23e-0x4bf2b0; org2 foley @0x4b76f1-0x4b77c6 then feet
// @0x4b77c6-0x4b78a8]. Foley plays at the body ORIGIN (`body`, mission-frame
// 16.16) with no dip and no surface pick; a footstep plays at FOOT level,
// dipped by the frame's capsule bottom, through the shared slot pick
// [orig: the dip @0x4b77d3]. `on_entity` is the decoded wire carrier link
// (PF_CARRIER_HANDLE >= 0), the wire's stand-in for Entity::ground_target.
void wire_body_slot_sounds(World &world, const uint32_t *words, int count,
                           int32_t capsule_bottom, int32_t item_id,
                           uint16_t character_id, uint16_t source_handle,
                           bool on_entity, const int32_t body[3]);

} // namespace opennova::world
