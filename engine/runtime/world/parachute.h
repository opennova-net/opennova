#pragma once

#include <cstdint>

namespace opennova::world {

// The two canopy words at entity+0x378/+0x37A.
struct ParachuteState {
    uint16_t inflation = 0;
    uint16_t flap = 0;
};
struct ParachuteEvents {
    bool opened = false;
    bool closed = false;
    bool flap = false;
    bool free_fall = false;
};

// Runs after gravity and before position integration. Only authority can
// deploy/close; every role consumes replicated deployment and applies braking.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B7AD9..0x4B7C8D]
ParachuteEvents parachute_tick(ParachuteState &state, uint32_t &flags,
        uint32_t &carry_flags, int32_t &velocity_z, bool authority, uint32_t tick);

} // namespace opennova::world
