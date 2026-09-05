// The typed AI record the embedder pushes into the F3 AI window (ADR 0042 d6:
// records in). A plain value: the engine join (world::inspect::ai_debug_report)
// computed on the window's cadence. An invalid snapshot clears the window (the
// world unloaded).
#pragma once

#include <cstdint>

#include <runtime/world/inspect.h>

namespace opennova::devtools {

struct AiDebugSnapshot {
	world::inspect::AiDebugReport report;
	bool valid = false;
	uint64_t logic_tick = 0;
};

}  // namespace opennova::devtools
