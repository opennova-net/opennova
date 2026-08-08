#pragma once

#include <cstdint>

// The per-tick system contract: TickContext + ISystem, split from the
// world.h umbrella (W3-7) so system implementers (BMS events, WAC, AI)
// stop pulling the whole World aggregate through their headers.

namespace opennova::world {

class World;

// ----------------------------------------------------------------------------
// Systems plugged into the tick service (WAC VM, BMS evaluator, ...).
// ----------------------------------------------------------------------------
struct TickContext {
    World *world = nullptr;
    uint32_t logic_tick = 0;
    bool is_authority = true;
    bool pre_mission = false; // BMS PreMission pass (EventFlags PreMission=2)
};

struct ISystem {
    virtual ~ISystem() = default;
    virtual const char *name() const = 0;
    virtual void on_load(World &) {}
    virtual void tick(World &, const TickContext &) = 0;
};

} // namespace opennova::world
