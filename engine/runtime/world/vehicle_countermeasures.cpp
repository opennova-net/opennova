#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>

namespace opennova::world {

// [orig: AIEntity_ReleaseFlareCountermeasures @0x455EF0]
void VehicleSystem::release_flares(Entity &vehicle) {
	const AiEntity *body = world_.ai.for_handle(vehicle.handle);
	if (body == nullptr)
		return;
	const int ammo =
			world_.tables.ammo.index_of(body->profile.type == 2 ? "GROUND_FLARE" : "FLARE");
	if (ammo < 0)
		return;
	const VehicleTraits *vt = traits.get(vehicle.item_id);
	if (vt == nullptr || vt->flare_points.empty()) {
		world_.round_sim.fire_source(world_, &vehicle,
				{ to_fixed(vehicle.position.x), to_fixed(vehicle.position.y),
						to_fixed(vehicle.position.z) },
				vehicle.veh.yaw_seeded ? vehicle.veh.yaw_bam
									   : bam_heading_from_mission_yaw_deg(vehicle.yaw),
				vehicle.veh.yaw_seeded ? vehicle.veh.air_pitch_bam
									   : bam_from_degrees_wrapped(vehicle.pitch),
				uint8_t(ammo));
		return;
	}
	const CollisionMatrix matrix = entity_placement_matrix(vehicle);
	for (const VehicleEffectPoint &point : vt->flare_points) {
		int32_t p[3], d[3];
		matrix.transform_point(point.position, p);
		int32_t local[3] = { point.direction[0], point.direction[1], point.direction[2] };
		if (local[2] == 0)
			local[2] = 24576;
		matrix.rotate_point(local, d);
		constexpr double angle_scale = 683565275.5764316;
		const int32_t yaw = static_cast<int32_t>(static_cast<uint32_t>(
				static_cast<int64_t>(std::atan2(double(d[1]), double(d[0])) * angle_scale)));
		const int32_t horizontal = static_cast<int32_t>(
				std::min(2147418112.0, std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1])));
		const int32_t pitch = static_cast<int32_t>(static_cast<uint32_t>(
				static_cast<int64_t>(std::atan2(double(d[2]), double(horizontal)) * angle_scale)));
		world_.round_sim.fire_source(
				world_, &vehicle, { p[0], p[1], p[2] }, yaw, pitch, uint8_t(ammo));
	}
}

// The pilot's entry is skipped: slots 1..9 read their actual seat occupant.
// [orig: Entity_UpdateAircraftPhysics @0x490310 — Entity_BuildWeaponSlotList
//  into the frame's two 10-entry arrays @0x4911a5..0x4911b5; entries 1..9 OR
//  the seat occupant's +0x12C bit 5 @0x4911c5..0x49145c; the (phase & 0x3F)
//  latch clear @0x49145e..0x491465; the +0x224 (net climb) && pressed gate,
//  the un-latched release and the latch @0x49146c..0x49148b]
// The scan has no side effect, so the early-out on a parked hull (retail
// scans first and tests +0x224 after) changes nothing observable.
void VehicleSystem::tick_flare_input(Entity &vehicle) {
	auto &m = vehicle.veh;
	const uint32_t phase = world_.logic_tick + 36u * uint32_t(vehicle.net_id);
	if ((phase & 63u) == 0)
		m.flare_latched = false;
	if (m.net_climb == 0)
		return;
	// Retail's stack arrays: rebuilt every tick with no allocation.
	VehiclePanelSlotList slots;
	build_vehicle_panel_slots(world_, vehicle.handle, slots);
	bool pressed = false;
	for (int i = 1; i < slots.count; ++i) {
		const Entity *holder = world_.registry.get(slots[i].entity);
		if (holder == nullptr)
			continue;
		for (const Seat &seat : holder->seats) {
			if (seat.retail_slot != slots[i].type)
				continue;
			const Entity *rider = world_.registry.get(seat.occupant);
			if (rider != nullptr && (rider->net_move_input & 0x20u) != 0)
				pressed = true;
		}
	}
	if (pressed) {
		if (!m.flare_latched)
			release_flares(vehicle);
		m.flare_latched = true;
	}
}

} // namespace opennova::world
