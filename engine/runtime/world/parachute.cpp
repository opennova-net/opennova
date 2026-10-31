#include <runtime/world/parachute.h>
#include <runtime/world/entity.h>
#include <algorithm>

namespace opennova::world {

// [orig: Entity_UpdateInfantryPlayerBody @0x4B7AD9..0x4B7C8D; canopy branch
// @0x4B7BFD; free-fall cadence @0x4B7C4C, velocity gate @0x4B7C52]
ParachuteEvents parachute_tick(ParachuteState &state, uint32_t &flags,
        uint32_t &carry_flags, int32_t &velocity_z, bool authority, uint32_t tick,
        bool opens_on_jump, bool move_order_jump) {
    if (authority && (flags & kEntityFlagDead) == 0) {
        // The carried parachute, or under dfx2.exe the MoveOrder jump bit (DFX2-111).
        // [orig: @0x4B7AE5..0x4B7AFA] [orig: the twin @0x4B4140 - `test byte ptr [esi+12Ch],
        //  20h` @0x4B7BCD (dfx2.exe)]
        const bool trigger = opens_on_jump ? move_order_jump : (carry_flags & 0x10u) != 0;
        if (velocity_z <= -0x3800 && trigger)
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
        // The open canopy spends the carried parachute; dfx2.exe's carries none to spend.
        // [orig: @0x4B7BC9] [orig: no store in the twin's arm @0x4B7C57..0x4B7CA4 (dfx2.exe)]
        if (!opens_on_jump) carry_flags &= ~0x10u;
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
