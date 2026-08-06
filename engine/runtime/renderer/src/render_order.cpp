#include "renderer/render_order.h"

#include <cstring>

namespace renderer {

TechniqueClass technique_class_for_submit(uint32_t stack_default_flags,
                                          uint32_t submit_flags) {
	// Stack-frame class defaults first, then the per-submit requests, in the
	// original's exact test order [orig: collect_render_objects_for_batch
	// @ 0x5d90d7..0x5d9145 (identical chain in the bone-path collector
	// @ 0x5d95c0..0x5d961f)].
	if (stack_default_flags & kStackDefaultClip)
		return TechniqueClass::Clip;
	if (stack_default_flags & kStackDefaultProjShadow)
		return TechniqueClass::ProjShadow;
	if (stack_default_flags & kStackDefaultDepthMask)
		return TechniqueClass::DepthMask;
	if (submit_flags & kSubmitClipPass)
		return TechniqueClass::Clip;
	if (submit_flags & kSubmitDepthMaskPass)
		return TechniqueClass::DepthMask;
	if (submit_flags & kSubmitProjShadowPass)
		return TechniqueClass::ProjShadow;
	if (submit_flags & kSubmitMatchTerrainPass)
		return TechniqueClass::MatchTerrain;
	return TechniqueClass::Normal;
}

uint32_t opaque_sort_key(float view_depth, uint32_t effect_index, bool alpha_tested) {
	// [orig: @ 0x5d926e..0x5d92c8] ftol + clamp to [0, 1023], then pack
	// (dist>>4)&0x3F | (effect&0x3F)<<6 | ((dist>>8)&3 | alphaTest<<2)<<12.
	// The original's OR of bits 15+ from an uninitialized slot (@ 0x5d92b9)
	// is D-RORD-6 - deliberately not reproduced (bits 15+ stay zero).
	int32_t dist = static_cast<int32_t>(view_depth);
	if (dist < 0)
		dist = 0;
	else if (dist > 1023)
		dist = 1023;
	const uint32_t udist = static_cast<uint32_t>(dist);
	uint32_t key = (udist >> 4) & 0x3Fu;
	key |= (effect_index & 0x3Fu) << 6;
	key |= ((udist >> 8) & 0x3u) << 12;
	if (alpha_tested)
		key |= 1u << 14;
	return key;
}

uint32_t transparent_sort_key(float view_depth) {
	// [orig: @ 0x5d931c..0x5d9326] or eax,-1 / sub eax,[depth slot]: the
	// slot's raw IEEE bits read as an integer, one's-complemented - exact
	// back-to-front under the unsigned ascending sort.
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(view_depth), "float width");
	std::memcpy(&bits, &view_depth, sizeof(bits));
	return ~bits;
}

TransparentQueue transparent_queue_for(float world_height, float water_height) {
	// [orig: @ 0x5d934e..0x5d9359] fcomp + jp: height >= threshold (or
	// unordered) selects the above-water queue.
	return (world_height >= water_height) ? TransparentQueue::AboveWater
	                                      : TransparentQueue::BelowWater;
}

int transparent_rung_for(TransparentQueue side, bool camera_above_water) {
	// The side opposite the camera flushes first (the far bracket), the
	// camera's side last [orig: SortAndFlush(camAbove ? 3 : 2) @ 0x5c9596;
	// SortAndFlush(3 - camAbove) @ 0x5c967a].
	const bool below = side == TransparentQueue::BelowWater;
	const bool far_side = below == camera_above_water;
	return far_side ? kRungAlphaFarSide : kRungAlphaCameraSide;
}

} // namespace renderer
