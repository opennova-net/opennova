#include <runtime/world/collision_debug_rows.h>

#include <runtime/world/collision.h>
#include <runtime/world/world.h>

namespace opennova::world {

void ray_debug_rows(const CollisionWorld &collision, uint32_t now_tick, std::vector<RayDebugRow> &out) {
	out.clear();
	if (!collision.ray_debug_enabled()) return;
	const uint32_t mask = collision.ray_debug_mask();
	const uint32_t ttl = static_cast<uint32_t>(collision.ray_debug_ttl_ticks());
	const auto &rings = collision.ray_debug_rings();
	for (size_t c = 0; c < rings.size(); ++c) {
		if ((mask & (1u << c)) == 0) continue;
		const CollisionWorld::RayDebugRing &ring = rings[c];
		const int32_t cap = static_cast<int32_t>(ring.events.size());
		if (cap == 0 || ring.count <= 0) continue;
		int32_t idx = ((ring.next - ring.count) % cap + cap) % cap;
		for (int32_t i = 0; i < ring.count; ++i, idx = (idx + 1) % cap) {
			const CollisionWorld::RayDebugEvent &ev = ring.events[static_cast<size_t>(idx)];
			// A stamp ahead of the clock is a restarted clock's leftover.
			if (ev.tick > now_tick) continue;
			const uint32_t age = now_tick - ev.tick;
			if (age > ttl) continue;
			RayDebugRow row;
			row.category = ev.category;
			row.result = ev.result;
			row.age_ticks = age;
			row.start = ev.start;
			row.end = ev.end;
			row.hit = ev.hit;
			out.push_back(row);
		}
	}
}

void contact_debug_rows(const World &world, const CollisionWorld &collision, uint32_t now_tick,
		std::vector<ContactDebugRow> &out) {
	out.clear();
	if (!collision.contact_debug_enabled()) return;
	const uint32_t mask = collision.contact_debug_mask();
	const CollisionWorld::ContactDebugRing &ring = collision.contact_debug_ring();
	const int32_t cap = static_cast<int32_t>(ring.events.size());
	if (cap == 0 || ring.count <= 0) return;
	int32_t idx = ((ring.next - ring.count) % cap + cap) % cap;
	for (int32_t i = 0; i < ring.count; ++i, idx = (idx + 1) % cap) {
		const CollisionWorld::ContactDebugEvent &ev = ring.events[static_cast<size_t>(idx)];
		if ((mask & (1u << ev.kind)) == 0 || ev.tick > now_tick) continue;
		const uint32_t age = now_tick - ev.tick;
		if (age > static_cast<uint32_t>(CollisionWorld::kContactDebugTtlTicks)) continue;
		ContactDebugRow row;
		row.kind = ev.kind;
		row.hit_class = ev.hit_class;
		row.age_ticks = age;
		row.pos = ev.pos;
		row.target = ev.target;
		if (ev.target != 0xFFFF) {
			EntityHandle handle;
			handle.packed = ev.target;
			if (const Entity *target = world.registry.get(handle)) {
				row.target_live = true;
				row.target_position = target->position;
				row.target_bound_radius = target->bound_radius;
			}
		}
		out.push_back(row);
	}
}

}  // namespace opennova::world
