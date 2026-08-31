// The typed AI record the embedder pushes into the F3 AI window (ADR 0042 d6:
// records in, typed requests out). A plain value: the engine join
// (world::inspect::ai_debug_report) computed on the window's cadence, plus the
// shell's overlay-view state (a device fact — the world-parented AI debug view
// lives on the Godot side, so its toggles' truth is read back from the shell,
// never assumed from the window's own clicks). An invalid snapshot clears the
// window (the world unloaded).
#pragma once

#include <cstdint>

#include <runtime/world/inspect.h>

namespace opennova::devtools {

// The shell-owned AI overlay view's toggle state. available = a loaded world
// with a live debug-view set behind it; while false the window's toggle strip
// draws disabled.
struct AiOverlayState {
	bool available = false;
	bool master = false;
	bool labels = true;
	bool routes = true;
	bool targets = true;
	bool rings = true;
};

struct AiDebugSnapshot {
	world::inspect::AiDebugReport report;
	bool valid = false;
	uint64_t logic_tick = 0;
	AiOverlayState overlay;
};

}  // namespace opennova::devtools
