#include <runtime/devtools/collision_overlay.h>

#include <runtime/devtools/physics_snapshot.h>
#include <runtime/devtools/rays_snapshot.h>
#include <runtime/world/collision.h>

#include <base/io/fixed.h>

#include <algorithm>

namespace opennova::devtools {

namespace {

world::Vec3 from_fixed(const world::FixedVec3 &p) {
	return {static_cast<float>(p.x / io::kFp16OneD), static_cast<float>(p.y / io::kFp16OneD),
			static_cast<float>(p.z / io::kFp16OneD)};
}

float fade_of(uint32_t age, int32_t ttl) {
	const float t = ttl > 0 ? static_cast<float>(age) / static_cast<float>(ttl) : 1.0f;
	return std::max(0.15f, 1.0f - t);
}

}  // namespace

void RaysOverlayLayer::draw(OverlayCanvas &canvas) {
	if (!record_.valid) return;
	for (const world::RayDebugRow &row : record_.rows) {
		const float *rgb = kRayCategoryColors[row.category < kRayCategoryCount ? row.category : 0];
		const float alpha = fade_of(row.age_ticks, record_.ttl_ticks);
		const world::Vec3 start = from_fixed(row.start);
		if (row.result == world::CollisionWorld::kRayDebugHit) {
			const world::Vec3 hit = from_fixed(row.hit);
			canvas.line(start, hit, overlay_rgba(rgb[0], rgb[1], rgb[2], alpha));
			// The unreached remainder, faint.
			canvas.line(hit, from_fixed(row.end), overlay_rgba(rgb[0], rgb[1], rgb[2], alpha * 0.18f));
			canvas.cross(hit, 0.15f, overlay_rgba(rgb[0], rgb[1], rgb[2], alpha));
		} else if (row.result == world::CollisionWorld::kRayDebugBlocked) {
			canvas.line(start, from_fixed(row.end),
					overlay_rgba(rgb[0] * 0.55f, rgb[1] * 0.55f, rgb[2] * 0.55f, alpha));
		} else {
			canvas.line(start, from_fixed(row.end), overlay_rgba(rgb[0], rgb[1], rgb[2], alpha * 0.3f));
		}
	}
}

void ContactsOverlayLayer::draw(OverlayCanvas &canvas) {
	if (!record_.valid) return;
	for (const world::ContactDebugRow &row : record_.rows) {
		const float *rgb = kContactKindColors[row.kind < kContactKindCount ? row.kind : 0];
		const float alpha = std::max(0.25f, fade_of(row.age_ticks, record_.ttl_ticks));
		const uint32_t color = overlay_rgba(rgb[0], rgb[1], rgb[2], alpha);
		canvas.cross(from_fixed(row.pos), 0.25f, color);
		if (row.target_live) {
			const float radius = std::max(row.target_bound_radius, 0.5f);
			canvas.ground_circle(row.target_position, radius, overlay_fade(color, 0.6f), 20);
		}
	}
}

}  // namespace opennova::devtools
