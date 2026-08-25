#pragma once

#include <cstdint>

namespace opennova::particle {

// THE PARTICLE GROUP's CHILD RENDER GATE — which children of an effect group
// draw on a given pass [orig: CParticleGroup_RenderChildren @0x5E5890].
//
// STAGED, NOT WIRED (2026-08-25 tidy): the effect scene
// (engine/runtime/particle/effect_scene.h) draws every child of a live group on
// one pass and has no per-child distance pass yet. Retail calls the gate from
// the particle manager's frame begin, once per live effect on each of its TWO
// passes with the pass's renderFlags [orig: CParticleManager_BeginFrame
// @0x5ECFC0 — the calls @0x5ED034 (pass 0) and @0x5ED07C (pass 1), each list
// walk followed by CParticleManager_TransformToViewSpace]; the owner-to-be is
// the group frame snapshot's child walk in effect_scene.cpp, with the device
// draw in godot/src/particle/nova_particle_renderer.cpp taking the pass flag.
// Consumed by tests/particle/group_render_gate_test.cpp only until then
// (scripts/lint/orphan_header_check.py).
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

// Pass flags [orig: the `& 4` test @0x5E58DB, the `& 1` test @0x5E5903 and the
// `& 2` test @0x5E5940].
inline constexpr uint32_t kRenderFlagFar = 0x1u;    // render when BEYOND
inline constexpr uint32_t kRenderFlagNear = 0x2u;   // render when WITHIN
inline constexpr uint32_t kRenderFlagAlways = 0x4u; // no distance test

// A child whose def class id (def+0x1E0) is 7 takes the ALWAYS arm ONLY: it
// draws when the pass carries flag 4 and is skipped on every other pass
// [orig: the `== 7` test @0x5E58D2 (jnz to the distance logic @0x5e58f3),
//  the `& 4` arm @0x5E58DB..0x5E58EE]. Every other child goes through the
// distance logic below and never sees the ALWAYS flag.
inline constexpr int32_t kGroupChildClassId = 7;

// Does this child draw on this pass?
//
// `distance` is the child's cached viewer distance; a child with no cached
// value uses ZERO [orig: the `if (result) distance = *(float*)result` guard
// @0x5e5925/@0x5e5962 — distance stays 0.0 when the pointer is null], which
// places it inside every near threshold rather than outside.
inline bool group_child_draws(int32_t child_class_id, uint32_t render_flags,
		float distance, float threshold, bool has_cached_distance) {
	// Class-7 children draw on the ALWAYS pass and on no other.
	if (child_class_id == kGroupChildClassId)
		return (render_flags & kRenderFlagAlways) != 0u;
	// No flags at all means no distance test — the child draws
	// [orig: the `test ebx, ebx` @0x5E58F3 -> the draw call @0x5e58ff].
	if (render_flags == 0u) return true;

	const float d = has_cached_distance ? distance : 0.0f;

	if ((render_flags & kRenderFlagFar) != 0u) {
		// FAR pass: skipped when at or inside the threshold
		// [orig: `if (distance <= threshold1) goto skip` @0x5E5934]. Note the
		// <=, so a child exactly on its threshold belongs to the NEAR pass, not
		// both.
		return d > threshold;
	}
	if ((render_flags & kRenderFlagNear) != 0u) {
		// NEAR pass: skipped when strictly beyond
		// [orig: `if (distance > threshold2) goto skip` @0x5E5971].
		return d <= threshold;
	}
	// A flag word with neither distance bit — including ALWAYS alone — skips a
	// non-7 child [orig: the `if ((renderFlags & 2) == 0) goto skip` @0x5E5943].
	return false;
}

} // namespace opennova::particle
