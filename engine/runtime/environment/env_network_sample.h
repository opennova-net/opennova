#pragma once

// The mission-start authoritative network environment sample: the ONE
// derivation from the mission-selected ENV resource + the BMS header onto
// world::EnvNetworkState, the lossless source of the scheduled S2C 0x0A
// phase-2 projection. Moved from apps/nw_server's startup lib (ADR 0042 d2 —
// an engine fact computed in one engine function); the Godot shell feeds the
// same publish seam from its live Weather snapshot (ADR 0015's implementation
// note), and the dedicated host publishes this mission-sourced sample.

#include <formats/mission/bms.h>
#include <runtime/world/world.h>

#include <istream>
#include <string>

namespace opennova::env {

// Parse one actual ENV resource, fold the BMS header overrides onto it, and
// publish the complete retail-native network sample. This stays independent
// of socket/session startup so focused runtime harnesses do not need a
// synthetic environment.
bool publish_initial_network_environment(
		std::istream &input,
		const bms::Header &header,
		world::EnvNetworkState &state,
		std::string &error);

// Retail settles mission-start environment state through 255 complete weather
// ticks before the server can publish phase 2.
void prewarm_network_environment(world::EnvNetworkState &state) noexcept;

} // namespace opennova::env
