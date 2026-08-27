#include <runtime/world/iris_march.h>

#include <formats/env/env_weather_core.h> // the indoor sample sentinels

namespace opennova::world {

// [orig: compute_ambient_light_along_direction @ 0x5c7a00]
void compute_iris_march(World &world, CollisionWorld &collision,
		const OcclusionWorld &occlusion, const int32_t cam[3], int32_t end[3],
		const int32_t sun[3], IrisMarch &out) {
	out = IrisMarch{};
	const EntityHandle local_player = world.cached.local_player;
	if (world.registry.get(local_player) == nullptr) return;

	// The full retail clip: terrain first unless the local player is indoors,
	// then every eligible solid in that player's candidate slice. The mutable
	// endpoint retains the nearest hit across the complete walk.
	// [orig: raycast_entity_collision @ 0x413760]
	collision.clip_segment_to_nearest_collision(world, local_player, cam, end);

	// The three ray clip radii — the shared witnessed triple
	// (CollisionWorld::kSunOcclusionClipRadii).
	const int32_t local_candidate_count = collision.candidate_count(local_player);

	// Samples at end, end + (cam-end)/3, end + 2(cam-end)/3 [orig: the thirds
	// march @ 0x5c7ad8..0x5c7b30].
	const int32_t step[3] = { (cam[0] - end[0]) / 3, (cam[1] - end[1]) / 3,
		(cam[2] - end[2]) / 3 };
	for (int s = 0; s < IrisMarch::kSampleCount; ++s) {
		const int32_t p[3] = { end[0] + step[0] * s, end[1] + step[1] * s,
			end[2] + step[2] * s };
		BlinkAccum blink;
		collision.query_candidate_blink_boxes_at_point(world, local_player, p, blink);
		if (blink.hits[0] != 0) {
			// Indoor sample: the hit's pool-2 entity carries interior data or
			// the curve runs on all-zero inputs (gain 255)
			// [orig: Pool_GetEntryUnchecked(2, hit >> 20) @ 0x5c7646; the
			//  pool_entry[12] == 0 skip @ 0x5c7652].
			const EntityHandle h = EntityHandle::make(2,
					static_cast<int32_t>(blink.hits[0] >> 20));
			if (occlusion.has_instance(h)) {
				// Lighting_SetInteriorLightGroup(building, section). The next
				// sample may replace/clear it; an indoor-no-data sample does not.
				out.interior_group_entity = h;
				out.interior_group_section = BlinkAccum::hit_section(blink.hits[0]);
				out.samples[out.count++] = env::WeatherCore::kIrisSampleIndoor;
			} else {
				out.samples[out.count++] = env::WeatherCore::kIrisSampleIndoorNoData;
			}
			continue;
		}
		// Lighting_SetInteriorLightGroup(0, 0) on every outdoor sample.
		out.interior_group_entity = EntityHandle{};
		out.interior_group_section = 0;
		// Outdoor sample: level = 8 minus one per blocked sun ray
		// [orig: @ 0x5c7784..0x5c77d7; the local player's +0x1C0 count gates
		// all three calls]. Each cast is the candidate-scoped walker with the
		// local player as BOTH exclusion entities and allowAllTypes = 1: it
		// iterates only that player's own +0x1BC/+0x1C0 slice (the 17-tick
		// arena), skips candidates owner-linked to the player, requires an
		// ItemDef, and reports BLOCKED on the first obstructed candidate
		// (retail: raycast_find_collision_entity @0x539a70 — slice walk
		// @0x539b5d..0x539bc4, pushed @0x5c7765..0x5c77c6 at radii
		// -0x2000/-0x5000/-0x8000, see docs/render/render-lighting-re.md).
		int32_t level = 8;
		const int32_t ray_end[3] = { p[0] + sun[0], p[1] + sun[1], p[2] + sun[2] };
		if (local_candidate_count > 0) {
			for (int r = 0; r < 3; ++r) {
				if (collision.candidate_segment_hits_solid(world, local_player, p,
							ray_end, CollisionWorld::kSunOcclusionClipRadii[r]))
					--level;
			}
		}
		out.samples[out.count++] = level;
	}
}

} // namespace opennova::world
