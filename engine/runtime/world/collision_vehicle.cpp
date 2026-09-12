#include <runtime/world/collision.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <runtime/world/ai.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include "collision_detail.h"

namespace opennova::world {

namespace {

// Low-dword IMUL/ADD, then signed IDIV, as in the ricochet Z correction.
// [orig: Entity_CheckCollisionState @0x462A30, @0x463069..0x463094]
int32_t ricochet_term(int32_t z, int32_t lateral) {
	const uint32_t zz = uint32_t(z) * uint32_t(z);
	const uint32_t ll = uint32_t(lateral) * uint32_t(lateral);
	return static_cast<int32_t>(int64_t(static_cast<int32_t>(zz + ll)) / z);
}

bool rides_source(const World &world, const Entity &candidate, EntityHandle source) {
	EntityHandle parent = candidate.ground_target;
	for (int i = 0; i < 3 && parent.valid(); ++i) {
		if (parent == source)
			return true;
		const Entity *e = world.registry.get(parent);
		if (e == nullptr)
			break;
		parent = e->ground_target;
	}
	return false;
}

} // namespace

// The entity half of the shared contact pass. Terrain forces are already in
// `forces`; every neighboring model contributes at the SAME probe positions.
// [orig: Entity_CheckCollisionState @0x462A30, @0x462DFB..0x4632CC;
// Entity_ComputeCollisionForces @0x462150, @0x462561..0x462A24]
int32_t CollisionWorld::resolve_vehicle_probes(World &world, Entity &source,
		const int32_t hull_pos[3], const int32_t (*probes)[3], const int32_t *radii, int count,
		int32_t soft, int32_t hard, VehicleProbeForce *forces, EntityHandle &hit_entity) {
	hit_entity = {};
	source.carry_flags &= ~0x40u; // auxiliary contact flag, not the carrier Flags bit
	if (count <= 0)
		return 0;
	int32_t candidate_count = 0;
	const EntityHandle *candidates = candidate_slice(source.handle, candidate_count);
	if (candidates == nullptr)
		return 0;
	const VehicleTraits *own_traits = world.vehicles.traits.get(source.item_id);
	const int32_t own_mass = own_traits != nullptr ? own_traits->mass : 0;
	const bool can_push = world.ai.for_handle(source.handle) != nullptr &&
			source.veh.cmd_speed == 0; // brain+0x220 [orig: @0x462A73..0x462A88]

	std::vector<CollisionPoint> points(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i)
		points[size_t(i)] = { probes[i][0], probes[i][1], probes[i][2], 0 };
	ContactQuery q;
	q.points = points.data();
	q.radii = radii;
	q.num_points = count;
	for (int axis = 0; axis < 3; ++axis)
		q.prev_pos[axis] = source.saved_live_valid ? source.saved_live_pos[axis] : hull_pos[axis];
	q.source_bound_radius = to_fixed(source.bound_radius);
	// Low-byte bit 7 adds type-12 to the ordinary vehicle type-7/CB query.
	// [orig: @0x462A91..0x462A9F; Entity_ComputeBoneCollisionForce @0x4AE150]
	q.mask = (source.item_attrib2 & 0x80u) != 0 ? 24 : 8;
	BlinkAccum blink;
	LadderContact ladder;
	CollisionTargetView view;
	std::vector<CollisionMatrix> matrices;
	int32_t severity = 0;
	for (int c = 0; c < candidate_count; ++c) {
		const EntityHandle ch = candidates[c];
		Entity *other = world.registry.get(ch);
		if (other == nullptr || ch == source.handle || rides_source(world, *other, source.handle))
			continue;
		// [orig: the mounted-child exclusions @0x462E26..0x462E4F, including
		// @0x462E37/@0x462E3D; the old hull-center substitute used this same gate]
		const Entity *mass_source = other;
		if ((other->item_attrib & 0x60u) == 0x20u) {
			const Entity *parent = world.registry.get(other->ground_target);
			if (parent != nullptr && (parent->item_attrib & 0x40u) != 0)
				mass_source = parent;
		}
		bool push_other = false;
		if ((mass_source->item_attrib & 0x40u) != 0) {
			const VehicleTraits *other_traits = world.vehicles.traits.get(other->item_id);
			const VehicleTraits *resolved_traits = world.vehicles.traits.get(mass_source->item_id);
			const int32_t other_mass = other_traits != nullptr ? other_traits->mass : 0;
			const int32_t resolved_mass = resolved_traits != nullptr ? resolved_traits->mass : 0;
			if (int64_t(own_mass) > 2LL * other_mass ||
					source.bound_radius > 2.0f * other->bound_radius) {
				other->flags |= 0x40u;
				if (other->item_type == 1)
					other->veh.contact_wake_tick = world.logic_tick;
				continue;
			}
			push_other = can_push && int64_t(resolved_mass) < 2LL * own_mass &&
					mass_source->bound_radius < 2.0f * source.bound_radius;
		}
		// Keep the cheap current bound ahead of section-matrix production.
		q.points = points.data();
		q.radii = radii;
		q.num_points = count;
		int32_t bound_pos[3], bound_radius = 0;
		if (!target_bound(world, ch, bound_pos, bound_radius, false) ||
				!detail::contact_query_overlaps_bound(bound_pos, bound_radius, q))
			continue;
		const CollisionTargetView *target = target_view(world, ch, view, matrices);
		if (target == nullptr)
			continue;
		for (int i = 0; i < count; ++i) {
			q.points = &points[size_t(i)];
			q.radii = &radii[i];
			q.num_points = 1;
			ContactResult contact;
			if (!collision_contact_force(*target, q, blink, ladder, contact))
				continue;
			if ((contact.flags & 0x800u) != 0)
				source.carry_flags |= 0x40u;
			if ((other->item_attrib & 1u) != 0)
				continue; // callback-only target
			const int32_t x = contact.force[0], y = contact.force[1], z = contact.force[2];
			const double length = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
			const int32_t magnitude = static_cast<int32_t>(std::min(length, 2147418112.0));
			const int32_t ratio = magnitude > 128
					? int32_t((int64_t(io::bam_abs(z)) * 0x400000) / magnitude)
					: 0x400000; // [orig: @0x462FC2..0x462FCB]
			VehicleProbeForce &f = forces[i];
			f.fz = io::bam_sub(f.fz, z);
			const int32_t dx = io::bam_sub(probes[i][0], hull_pos[0]);
			const int32_t dy = io::bam_sub(probes[i][1], hull_pos[1]);
			const int32_t dot = io::bam_add(int32_t((int64_t(dx) * x + 0x8000) >> 16),
					int32_t((int64_t(dy) * y + 0x8000) >> 16));
			int32_t residual_x = x, residual_y = y;
			bool ricochet = true;
			if (dot >= 0) {
				if (ratio < soft || z > 0 || push_other) {
					// Full force at a wall/downward face or a pushable vehicle.
					// [orig: @0x46322D..0x463240; masks @0x463251..0x46326E]
					f.fx = io::bam_sub(f.fx, x);
					f.fy = io::bam_sub(f.fy, y);
					f.wall_contact = f.wall_contact || ratio < soft;
					hit_entity = ch;
					severity = 3;
					ricochet = false;
				} else if (ratio < ((int64_t(soft) + hard) >> 1)) {
					f.fx = io::bam_sub(f.fx, x >> 1);
					f.fy = io::bam_sub(f.fy, y >> 1);
					residual_x = x >> 1;
					residual_y = y >> 1;
					severity = std::max(severity, 2);
				} else if (ratio < hard) {
					f.fx = io::bam_sub(f.fx, x >> 2);
					f.fy = io::bam_sub(f.fy, y >> 2);
					residual_x = io::bam_sub(x, x >> 2);
					residual_y = io::bam_sub(y, y >> 2);
					severity = std::max(severity, 1);
				}
			}
			if (ricochet && z != 0 &&
					io::bam_abs(z) > (io::bam_add(io::bam_abs(x), io::bam_abs(y)) >> 3)) {
				f.fz = io::bam_sub(f.fz,
						io::bam_add(ricochet_term(z, residual_x), ricochet_term(z, residual_y)));
			}
			if (contact_debug_enabled_)
				contact_debug_record(ContactDebugKind::kVehicleHull, world.logic_tick, ch,
						probes[i], uint8_t(i));
		}
	}
	return severity;
}

} // namespace opennova::world
