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

// The contact-kind palette (ContactDebugKind order): the window's swatches
// and the Game-view layer's crosses.
inline constexpr float kContactKindColors[kContactKindCount][3] = {
	{1.0f, 0.35f, 0.15f}, // Projectile hit
	{1.0f, 0.4f, 0.7f},   // Knife hit
	{1.0f, 0.7f, 0.2f},   // Move contact
	{0.9f, 0.3f, 1.0f},   // Vehicle hull
	{0.75f, 0.6f, 0.4f},  // Terrain hit
	{0.3f, 0.9f, 1.0f},   // Water hit
};

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
