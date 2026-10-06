#pragma once
#include <cstdint>
namespace opennova::world {
class World;
// stance_bits retain MoveOrder bits 8/9: prone=1, crouch=2.
void emit_stance_change_sound(World &world, uint16_t source, const int32_t pos[3],
    uint8_t &previous, uint8_t stance_bits, uint32_t flags, bool parent_has_definition);

// Whether a body's sound block reads the anim event word on logic tick `tick`: the NPC updater
// (org1) on odd ticks, the player body (org2) on even [orig: org1 `and eax,1; jz skip`
// @0x4bf144-0x4bf156; org2 `test current_tick,1; jnz skip` @0x4b76e6 (var = current_tick
// @0x4b4147)]. The one gate the bodies and the editor's clip preview share.
inline bool anim_sound_tick(uint32_t tick, bool player_body) {
    return ((tick & 1u) != 0) != player_body;
}

// One sound an anim event word plays: its sound-profile slot, and where it plays: `foot` -1 at the
// body's origin (a foley bit), 0 or 1 the left or right foot at foot level.
struct AnimEventSound {
    int slot = 0;
    int foot = -1;
};
inline constexpr int kAnimEventSoundMax = 8; // the six foley bits and the two feet

// The sounds one event word plays, in the order the body's block plays them: the six foley bits,
// 0x20 << i the slot SSAudio(i + 1) [orig: org1 @0x4bf169-0x4bf23e; org2 @0x4b76f1-0x4b77c6], then
// the left foot (0x1) and the right (0x2) through audio::footstep_slot over the state under the feet
// (`feet_z` the body dipped to foot level by the frame's capsule bottom, the water plane, the ground
// entity link, the surface class there) [orig: org1 @0x4bf23e-0x4bf2b0; org2 @0x4b77c6-0x4b78a8].
// Bits no sound block reads (the NPC's fire bits) play nothing here. Returns how many it wrote.
int anim_event_sounds(uint32_t word, int32_t feet_z, int32_t water_z, bool on_entity,
                      int32_t surface_type, AnimEventSound out[kAnimEventSoundMax]);
}
