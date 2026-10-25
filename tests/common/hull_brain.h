// A rig hull's brain (+0x64): the class init gives one to every row of a brain
// class (an AIData def whose ai_function row is CHel / cpln / cveh / cbot / ctrn),
// and each family mover jumps a hull without one from its savedLivePose stamp to
// its matrix-build tail, so a rig that wants its hull to move brains it first.
// The profile's class word (+16) is what the part machine and the rotor gate read:
// HELO (1) for an aircraft family (helo1, or an aircraft row's own default_aip),
// GROUND (2) otherwise.
// [orig: Entity_InitVehicleAI @0x460200 (the store @0x460252); the movers' brain
//  tests, cveh @0x48AFA6, ctan @0x488B56, cbik @0x484086]
// Header-only test infrastructure.
#pragma once

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace test_common {

inline int32_t hull_profile_type(opennova::world::VehicleFamily family) {
	return opennova::world::vehicle_family_uses_direct_air_mover(family) ? 1 : 2;
}

inline opennova::world::AiEntity &brain_hull(opennova::world::World &w,
		opennova::world::EntityHandle h,
		opennova::world::VehicleFamily family = opennova::world::VehicleFamily::Ground) {
	opennova::world::AiEntity &ae = *w.ai.at(w.ai.attach(h));
	ae.profile.type = hull_profile_type(family);
	return ae;
}

} // namespace test_common
