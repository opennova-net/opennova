// The capture tables the F3 Rays and Physics windows share: one kind's counts
// in a CollisionWorld debug ring as the embedder pushes them (the rays'
// categories, the contacts' kinds), the row each reads as, and the table of
// kinds a window filters its Game-view layer by. ImGui-free: the snapshot
// headers that carry the counts reach the shell; capture_table.cpp draws.
#pragma once

#include <cstdint>
#include <string>

namespace opennova::devtools {

// One kind's counts in a capture ring. The embedder fills `name` from the
// collision world's name table (static storage).
struct CaptureCount {
	const char *name = "";
	int32_t held = 0;    // events currently in the ring (<= the ring cap)
	uint64_t total = 0;  // lifetime recorded — overwrite never hides throughput
};

// A kind's counts as its row reads ("Projectile hit: held 12 / total 340").
std::string capture_row_text(const CaptureCount &count);

// The kinds a window filters by: All and None, then a row per kind (its draw
// check, its swatch from `colors`, its name, its counts). `mask` is the
// window's edit of the filter (bit i: draw kind i), flipped here on a click
// and confirmed by the next push; true when a click changed it, the window
// then queuing its request.
bool capture_mask_table(const char *id, uint32_t &mask, uint32_t all, const CaptureCount *counts,
		const float (*colors)[3], int count);

}  // namespace opennova::devtools
