#pragma once

// nw_pp --scenario-events: one decoded SCENARIO_EVENT line per message a
// network-parity scenario asserts on (ADR 0050 R5; scripts/net/scenarios/).
// The decode is the engine's own (ingame_decode.h); this only formats the
// fields as stable key=value pairs for scripts/net/compare_scenario.py.
//
//   SCENARIO_EVENT frame=N ts_ns=T dir=S|C session=N tag=0xNN kind=<name>
//       <fields> decode=0|1
//
// Handles print as (pool<<12)|slot hex words, pool-0 indices as decimals.
// C2S 0x0C prints only when its ground carrier changes (the first uplink
// included), so a mount or dismount shows as one line and the uplink's own
// handle names a joiner actor; S2C 0x46's roster slot 0 names the host's.

#include <net/npwire/wire_capture.h>

#include <cstdint>
#include <map>
#include <string>

namespace opennova::nwpp {

struct ScenarioEventTracker {
	// session -> the last C2S 0x0C ground carrier printed
	std::map<int, uint16_t> carrier;
};

// The SCENARIO_EVENT line (no newline) for a scenario-relevant message, or
// an empty string for every other message.
std::string format_scenario_event(const InGameMessage &message, uint64_t ts_nanos,
		ScenarioEventTracker &tracker);

} // namespace opennova::nwpp
