// The typed ray-capture record the embedder pushes into the F3 Rays window
// (ADR 0042 d6: records in, typed requests out). A plain value built from the
// CollisionWorld ray-debug rings plus the presentation filter the Simulation
// owns (category mask + TTL), snapshotted here so the window never reaches
// into a live World. An invalid snapshot clears the window (the world
// unloaded).
#pragma once

#include <cstdint>

namespace opennova::devtools {

// Mirrors world::CollisionWorld::RayDebugCategory::kCount; the window carries
// its own count so ImGui code never includes the collision header. The
// embedder fills `name` from ray_debug_category_name (static storage).
inline constexpr int kRayCategoryCount = 15;
// Every category bit set (the "All" filter and the snapshot's default mask).
inline constexpr uint32_t kRayCategoryMaskAll = (1u << kRayCategoryCount) - 1;

struct RaysCategoryCount {
	const char *name = "";
	int32_t held = 0;    // events currently in the ring (<= the ring cap)
	uint64_t total = 0;  // lifetime recorded — overwrite never hides throughput
};

struct RaysSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	bool recording = false;   // CollisionWorld ray capture enabled
	uint32_t category_mask = kRayCategoryMaskAll; // bit i = draw category i
	int32_t ttl_ticks = 0;           // the capture's fade window
	RaysCategoryCount categories[kRayCategoryCount];
};

}  // namespace opennova::devtools
