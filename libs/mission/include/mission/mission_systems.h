#pragma once

#include "mission/event_runtime.h"
#include "wac/wac_system.h"
#include "world/ai.h"
#include "world/world.h"

namespace opennova::mission {

// Register the mission logic systems on `world` in the faithful tick order and
// initialise them (load_systems -> each system's on_load). One place owns the order
// so the editor binding and the game runtime can't drift.
//
// Order: WAC -> BMS -> AI. [orig: sub_4F81A0 / Server_TickUpdate runs the script
// evaluators then the AI update over one shared world. The relative WAC<->BMS order is
// grill-gated against sub_4F81A0 (verify before treating as settled); AI is registered
// last so it consumes the entity state the scripts mutate this tick.] The systems must
// already hold their loaded program/events; the caller owns the objects, which must
// outlive `world`.
//
// Header-only on purpose: pulling WacSystem (opennova_wac) in here would force every
// opennova_mission consumer to link the WAC VM, so the registration glue lives at the
// call site (the binding / the cross-system tests), which already link both.
inline void register_mission_systems(opennova::world::World &world,
                                     opennova::wac::WacSystem &wac,
                                     BmsEventSystem &bms,
                                     opennova::world::AiSystem &ai) {
	world.add_system(&wac);
	world.add_system(&bms);
	world.add_system(&ai);
	world.load_systems();
}

} // namespace opennova::mission
