#pragma once

#include <cstdint>

namespace opennova::particle {

// THE PARTICLE GROUP's CHILD RENDER GATE — which children of an effect group
// draw on a given pass [orig: CParticleGroup_RenderChildren @0x5E5890].
//
// A group is drawn in more than one pass, and each child declares which pass
// it belongs to by its own distance threshold. That is how an effect puts its
// near detail and its far billboard in the same group without drawing both:
// the near pass takes children whose threshold the viewer is inside, the far
// pass takes the rest.
//
// Getting the comparison the wrong way round draws every child on every pass —
// which looks like a working effect at one distance and a doubled one at
// another.

// Pass flags [orig: the `& 1` / `& 2` / `& 4` tests @0x5E58C4..].
inline constexpr uint32_t kRenderFlagFar = 0x1u;    // render when BEYOND
inline constexpr uint32_t kRenderFlagNear = 0x2u;   // render when WITHIN
inline constexpr uint32_t kRenderFlagAlways = 0x4u; // no distance test

// A child whose class id is not 7 is skipped entirely unless the ALWAYS flag
// is set [orig: the `!= 7` break @0x5E58B7 and the `& 4` arm @0x5E58BE].
inline constexpr int32_t kGroupChildClassId = 7;

// Does this child draw on this pass?
//
// `distance` is the child's cached viewer distance; a child with no cached
// value uses ZERO [orig: the `if (result) distance = *(float*)result` guard —
// distance stays 0.0 when the pointer is null], which places it inside every
// near threshold rather than outside.
inline bool group_child_draws(int32_t child_class_id, uint32_t render_flags,
		float distance, float threshold, bool has_cached_distance) {
	// The ALWAYS flag short-circuits the class check too.
	if ((render_flags & kRenderFlagAlways) != 0u) return true;
	if (child_class_id != kGroupChildClassId) return false;
	// No flags at all means no distance test — the child draws.
	if (render_flags == 0u) return true;

	const float d = has_cached_distance ? distance : 0.0f;

	if ((render_flags & kRenderFlagFar) != 0u) {
		// FAR pass: skipped when at or inside the threshold
		// [orig: `if (distance <= threshold1) goto skip`]. Note the <=, so a
		// child exactly on its threshold belongs to the NEAR pass, not both.
		return d > threshold;
	}
	if ((render_flags & kRenderFlagNear) != 0u) {
		// NEAR pass: skipped when strictly beyond
		// [orig: `if (distance > threshold2) goto skip`].
		return d <= threshold;
	}
	// A flag word with none of the three set skips the child
	// [orig: the `if ((renderFlags & 2) == 0) goto skip` fall-through].
	return false;
}

} // namespace opennova::particle
