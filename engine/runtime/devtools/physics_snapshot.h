// The typed contact-capture record the embedder pushes into the F3 Physics
// window (ADR 0042 d6: records in, typed requests out). A plain value built
// from the CollisionWorld contact-debug ring plus the capture state the
// Simulation owns (armed + kind mask), snapshotted here so the window never
// reaches into a live World. An invalid snapshot clears the window (the world
// unloaded).
#pragma once

#include <cstdint>

namespace opennova::devtools {

// Mirrors world::CollisionWorld::ContactDebugKind::kCount; the window carries
// its own count so ImGui code never includes the collision header (the seam
// static_asserts the two stay equal). The embedder fills `name` from
// contact_debug_kind_name (static storage).
inline constexpr int kContactKindCount = 6;
inline constexpr uint32_t kContactKindMaskAll =
		(1u << kContactKindCount) - 1;

struct PhysicsKindCount {
	const char *name = "";
	int32_t held = 0;    // events currently in the ring (<= the ring cap)
	uint64_t total = 0;  // lifetime recorded — overwrite never hides throughput
};

struct PhysicsSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	bool capturing = false;   // CollisionWorld contact capture armed
	uint32_t kind_mask = kContactKindMaskAll; // bit i = draw kind i
	int32_t recent = 0;        // ring events inside the contact-debug TTL
	PhysicsKindCount kinds[kContactKindCount];
};

}  // namespace opennova::devtools
