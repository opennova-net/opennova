#pragma once

#include <cstdint>
#include <optional>

// The per-tick system contract: TickContext + ISystem, split from the
// world.h umbrella (W3-7) so system implementers (BMS events, WAC, AI)
// stop pulling the whole World aggregate through their headers.

namespace opennova::world {

class World;

// ----------------------------------------------------------------------------
// Systems plugged into the tick service (WAC VM, BMS evaluator, ...).
// ----------------------------------------------------------------------------
enum class TickPhase : uint8_t {
    Gameplay,
    PreMission,
    PreRound,
};

struct TickContext {
    World *world = nullptr;
    uint32_t logic_tick = 0;
    bool is_authority = true;
    TickPhase phase = TickPhase::Gameplay;
    // World freezes the shared WAC/BMS gate before either executes. Direct
    // system callers may omit it and let the system evaluate its own gate.
    std::optional<bool> script_admitted;
};

struct ISystem {
    virtual ~ISystem() = default;
    virtual const char *name() const = 0;
    virtual void on_load(World &) {}
    // Publish derived shared state before World makes its per-tick decisions.
    virtual void prepare_tick(World &) {}
    virtual void tick(World &, const TickContext &) = 0;
};

} // namespace opennova::world
