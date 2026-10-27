#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <runtime/world/entity.h>

namespace opennova::world {

// Fixed mission-frame descriptor emitted by WAC and BMS. The portable
// particle scene owns the actual group and entity-slot lifetime (+460).
// [orig: CEffectWorld_SpawnEmitterAtPosition @0x5F6DF0]
struct ScriptEffectEvent {
    EntityHandle owner;
    std::array<int32_t, 3> position{};
    std::array<int32_t, 3> direction{};
    std::string name;
    bool lookup_by_name = false; // BMS: strict named lookup, no stockeffect fallback
    bool store_slot = true;      // entity+460; fxrain leaves it alone
    // The entity's slot (entity_effect_slot's token value) the group is stored in, the one every
    // +460 producer shares (D-PTL-26).
    uint64_t slot = 0;
    bool release_previous = false; // fx2ssn releases BEFORE attempting the spawn
    uint64_t source_tick = 0;
    uint64_t source_order = 0;
};

} // namespace opennova::world
