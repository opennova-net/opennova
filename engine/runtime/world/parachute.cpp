#include <runtime/world/parachute.h>
#include <runtime/world/entity.h>
#include <algorithm>

namespace opennova::world {

// [orig: Entity_UpdateInfantryPlayerBody @0x4B7AD9..0x4B7C8D; canopy branch
// @0x4B7BFD; free-fall cadence @0x4B7C4C, velocity gate @0x4B7C52]
ParachuteEvents parachute_tick(ParachuteState &state, uint32_t &flags,
        uint32_t &carry_flags, int32_t &velocity_z, bool authority, uint32_t tick) {
    if (authority && (flags & kEntityFlagDead) == 0) {
        if (velocity_z <= -0x3800 && (carry_flags & 0x10u) != 0)
            flags |= kEntityFlagParachute;
        if ((flags & kEntityFlagLadderContact) != 0 || (flags & kEntityFlagInAir) == 0)
            flags &= ~kEntityFlagParachute;
    }
    ParachuteEvents events;
    const bool deployed = (flags & kEntityFlagParachute) != 0;
    if (deployed != ((carry_flags & 0x20u) != 0)) {
        if (deployed) {
            carry_flags |= 0x20u;
            state = {};
            events.opened = true;
        } else {
            carry_flags &= ~0x20u;
            events.closed = true;
        }
    }
    if (deployed) {
        state.inflation = std::min<uint16_t>(uint16_t(state.inflation + 0x300u), 0x7FFFu);
        state.flap = std::min<uint16_t>(uint16_t(state.flap + 0x200u), 0x7FFFu);
        carry_flags &= ~0x10u;
        events.flap = (tick & 63u) == 0;
        if (velocity_z < -0x1C00) velocity_z += 0x29C;
    } else {
        state.flap = uint16_t(state.flap - 0x280u);
        state.inflation = uint16_t(state.inflation - 0x40u);
        if (state.flap > 0x7FFFu) state = {};
        if (state.inflation > 0x7FFFu) state.inflation = 0;
        events.free_fall = (tick & 63u) == 0 && velocity_z < -0x3000;
        velocity_z = std::max(velocity_z, -0x8000);
    }
    return events;
}

} // namespace opennova::world
