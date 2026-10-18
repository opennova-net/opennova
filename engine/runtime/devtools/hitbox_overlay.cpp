#include <runtime/devtools/hitbox_overlay.h>

#include <runtime/world/round_sim.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace opennova::devtools {

uint32_t HitboxOverlayLayer::section_color(int32_t section, bool masked, bool fallback) {
	if (masked) return overlay_rgba(0.45f, 0.08f, 0.08f, 0.8f);
	if (fallback) return overlay_rgba(1.0f, 0.65f, 0.15f, 0.95f);
	// The zone table lives in the engine [world/round_sim.h].
	const double mult = world::hit_zone_damage_multiplier(section);
	if (mult >= 3.0) return overlay_rgba(1.0f, 0.25f, 0.85f);        // the x3.0 head
	if (mult > 1.0) return overlay_rgba(1.0f, 0.58f, 0.18f, 0.95f);  // the x1.25 torso
	if (mult < 1.0) return overlay_rgba(0.45f, 1.0f, 0.25f, 0.95f);  // the x0.5 limbs
	return overlay_rgba(0.2f, 0.9f, 1.0f, 0.9f);
}

void HitboxOverlayLayer::draw(OverlayCanvas &canvas) {
	if (!record_.valid) return;
	const auto &entities = record_.report.entities;
	for (const world::CollisionWorld::DebugHitboxEntity &e : entities) {
		const world::Vec3 pos = overlay_point(e.pos);
		// The broad-phase sphere: faint white, amber when no face mesh backs it
		// (the bound sphere alone resolves hits there).
		canvas.sphere_outline(pos, overlay_units(e.bound_radius),
				e.has_faces ? overlay_rgba(1.0f, 1.0f, 1.0f, 0.22f) : overlay_rgba(1.0f, 0.7f, 0.2f, 0.8f));
		for (const world::CollisionWorld::DebugHitboxFace &f : e.faces) {
			uint32_t color = overlay_index_color(f.material, 0.8f);
			if ((f.flags & world::kFaceFlagNeverHit) != 0) color = overlay_rgba(0.45f, 0.08f, 0.08f, 0.8f);
			canvas.triangle(overlay_point(f.v[0]), overlay_point(f.v[1]), overlay_point(f.v[2]), color);
		}
	}
	for (const mission::DebugHitboxOrganic &o : record_.report.organics) {
		canvas.sphere_outline(overlay_point(o.center), overlay_units(o.radius_q16),
				section_color(o.section, o.masked, o.fallback));
	}
	// Labels on the nearest meshed bodies: faces drawn / authored, husk state.
	std::vector<std::pair<float, const world::CollisionWorld::DebugHitboxEntity *>> nearest;
	for (const auto &e : entities) nearest.emplace_back(canvas.distance(overlay_point(e.pos)), &e);
	std::sort(nearest.begin(), nearest.end(),
			[](const auto &a, const auto &b) { return a.first < b.first; });
	const size_t count = std::min(nearest.size(), static_cast<size_t>(kLabelNearest));
	for (size_t i = 0; i < count; ++i) {
		const auto &e = *nearest[i].second;
		char text[96];
		std::snprintf(text, sizeof(text), "%s %d/%d faces  r %.1f%s", e.has_faces ? "mesh" : "sphere",
				static_cast<int>(e.faces.size()), e.face_total, overlay_units(e.bound_radius),
				e.husk ? "  husk" : "");
		const world::Vec3 pos = overlay_point(e.pos);
		canvas.text({pos.x, pos.y, pos.z + overlay_units(e.bound_radius)}, text, overlay_rgba(0.9f, 0.9f, 0.9f));
	}
}

}  // namespace opennova::devtools
