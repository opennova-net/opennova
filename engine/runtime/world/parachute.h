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
// The deploy's trigger is the title's (TitleRules::parachute_opens_on_jump): the carried
// parachute bit (`carry_flags` & 0x10), which an open canopy clears, or the body's MoveOrder
// jump bit (`move_order_jump`), which clears nothing.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B7AD9..0x4B7C8D]
// [orig: the twin @0x4B4140 - @0x4B7BB5..0x4B7D6C (dfx2.exe)]
ParachuteEvents parachute_tick(ParachuteState &state, uint32_t &flags,
        uint32_t &carry_flags, int32_t &velocity_z, bool authority, uint32_t tick,
        bool opens_on_jump, bool move_order_jump);

} // namespace opennova::world
