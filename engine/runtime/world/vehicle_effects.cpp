#include "vehicle_motor_detail.h"
#include "world.h"

#include <cstdio>

namespace opennova::world::detail {
namespace {
// Live damage smoke/fire use independent emitter slots, shared between the
// mover health band and the contact wreck state. [orig: @0x48B04B..0x48B11A]
void damage_effect(World &world, Entity &e, uint8_t family, const char *effect, bool release) {
	bool &active = family == 4 ? e.veh.damage_smoke_active : e.veh.damage_fire_active;
	if (release ? !active : active)
		return;
	DestructionEffectEvent event;
	event.effect = effect;
	event.pos = e.position;
	if (const auto *t = world.vehicles.traits.get(e.item_id))
		if (t->family == VehicleFamily::Watercraft)
			event.pos.z = std::max(event.pos.z, float(from_fixed(world.env.water_z)));
	event.family = family;
	event.attach_net_id = e.net_id;
	event.attach_bms_id = e.bms_id;
	event.attach_wire_handle = e.handle.packed;
	event.attach_spawn_origin = e.spawn_origin;
	event.release = release;
	world.out.destruction.effects.push_back(std::move(event));
	active = !release;
}
const char *fire_effect(const VehicleTraits &t, bool contact) {
	// [orig: @0x48B0B1, @0x484191, @0x488C80, @0x48D646..0x48D663,
	// @0x490501; contact @0x4809C8]
	if (t.family == VehicleFamily::Tank)
		return "Effect_vehicleFireLarge";
	if (t.family == VehicleFamily::Ground ||
			(contact && vehicle_family_uses_direct_air_mover(t.family)))
		return "Effect_vehicleFireMed";
	if (t.family == VehicleFamily::Watercraft) {
		if (t.unit_type == 8)
			return "Effect_vehicleFireLarge";
		if (t.unit_type == 7)
			return "Effect_vehicleFireMed";
	}
	return "Effect_vehicleFireSmall";
}
void set_parked(Entity &e, bool value) {
	e.flags = value ? e.flags | 0x10u : e.flags & ~0x10u;
	e.engine_flags = value ? e.engine_flags | 0x10u : e.engine_flags & ~0x10u;
}
} // namespace

// All six contact callbacks spawn this local effect AND fan the positioned
// sound on their first submerged pad. The two resource-table bindings at
// 849188/849298 are Effect_sboatwake / Effect_SmlSplash.
// [orig: Entity_ProcessVehicleSuspension @0x463C60; Entity_ProcessTrackedVehiclePhysics
// @0x47C1C0; Entity_ProcessPlatformPhysics @0x481870; Server_SendOverlayActionToAlive @0x50A1B0]
void vehicle_water_entry(World &world, Entity &e, int32_t x, int32_t y, bool was_water) {
	if (was_water || (e.flags & 0x8000u) == 0)
		return;
	const bool airborne = (e.flags & kEntityFlagInAir) != 0;
	const int32_t z = world.env.water_z;
	world.out.vehicle_effects.push_back({ airborne ? "Effect_SmlSplash" : "Effect_sboatwake",
			{ float(from_fixed(x)), float(from_fixed(y)), float(from_fixed(z)) }, { 0, 0, -0.5f },
			world.logic_tick });
	world.out.water_crossings.add(x, y, z, airborne);
	if (world.ai.is_authority) {
		SoundSlotEvent event;
		event.source_handle = e.handle.packed;
		event.pos[0] = x;
		event.pos[1] = y;
		event.pos[2] = z;
		std::snprintf(event.set_name, sizeof(event.set_name), "%s",
				airborne ? "SURFACE_WTR" : "BODYWATER1");
		world.out.slot_sounds.push_back(event);
	}
}

void vehicle_smoke_effect(World &world, Entity &e, bool release) {
	// [orig: VehicleEffect_InitAll @0x455C33..0x455C84]
	damage_effect(world, e, 4, "Effect_smkSigB", release);
}
void vehicle_release_damage_effects(World &world, Entity &e) {
	// [orig: Entity_RebuildOrientationMatrixFromAxes @0x4632E0;
	// Entity_RespawnVehicle @0x45FF40; Entity_ProcessFallingDeathPhysics @0x461D30]
	vehicle_smoke_effect(world, e, true);
	damage_effect(world, e, 5, "", true);
}
void vehicle_health_effects(World &world, Entity &e, const VehicleTraits &t, bool critical) {
	// The critical fire band uses health before the cadence drain. Tank first
	// smokes, switching to large fire strictly below criticalHp/8.
	// [orig: @0x48B046..0x48B0E7, @0x488C38..0x488CC7]
	// The ctank/cbike rows never reach that mover [orig:
	// Entity_DispatchPhysics_ctank @0x48F000, Entity_DispatchPhysics_cbike @0x48EFF0].
	if (t.physics == 0 && t.family != VehicleFamily::Watercraft &&
			t.family != VehicleFamily::Tank && t.family != VehicleFamily::Bike &&
			!vehicle_family_uses_direct_air_mover(t.family)) {
		// The simple ground callback only sounds its critical warning.
		// [orig: Entity_ProcessInfantryPhysics @0x46E100 (IDB misnomer, the
		//  selector-zero ground mover), `test byte ptr [esp+var_84], 1Fh`
		//  @0x46E250..0x46E266]
		if (critical && ((world.logic_tick + 36u * uint32_t(e.net_id)) & 31u) == 0)
			world.vehicles.play_contact_sound(e, t, 34);
		return;
	}
	if (critical) {
		// The warning one-shot (profile +0x88 = slot 34) runs on a 32-tick
		// cadence in the ground, bike, tank, boat and selector-zero movers and
		// on a 64-tick cadence in the direct-air mover.
		// [orig: Entity_UpdateVehiclePhysics `test bl, 1Fh` @0x48B08C;
		//  Entity_UpdateLightVehiclePhysics @0x48416C; Entity_UpdateTankVehiclePhysics
		//  @0x488C44; Entity_UpdateWatercraftPhysics @0x48D61F;
		//  Entity_ProcessAirVehiclePhysics (IDB misnomer, the selector-zero boat)
		//  @0x46FB79; Entity_UpdateAircraftPhysics `test byte ptr [esp+var_A4], 3Fh`
		//  @0x4904D7..0x4904F0]
		const uint32_t cadence_mask = vehicle_family_uses_direct_air_mover(t.family) ? 63u : 31u;
		if (((world.logic_tick + 36u * uint32_t(e.net_id)) & cadence_mask) == 0)
			world.vehicles.play_contact_sound(e, t, 34);
		if (t.family == VehicleFamily::Tank && e.health >= (t.critical_hp >> 3))
			vehicle_smoke_effect(world, e);
		else
			damage_effect(world, e, 5, fire_effect(t, false), false);
	} else if (e.health > 0 && e.health < (e.health_max >> 2)) {
		vehicle_smoke_effect(world, e);
	}
}

// The contact latches own wreck-rest burning, independently of the 64-tick
// critical-health band. Call before the fit selection: after the sink growth in
// every family but the tank, whose block precedes its growth.
// [orig: ground @0x47DC68..0x47DF79; tank @0x4780CD..0x478381 (growth @0x478510);
// bike @0x47AC46..0x47AD65; air @0x4807C6..0x480BC7;
// boat @0x483474..0x48367D]
void vehicle_crash_state(
		World &world, Entity &e, const VehicleTraits &t, const bool *contacts, int32_t up) {
	auto &m = e.veh;
	const bool boat = t.family == VehicleFamily::Watercraft;
	const bool tank = t.family == VehicleFamily::Tank;
	const bool bike = t.family == VehicleFamily::Bike;
	const bool authority = world.ai.is_authority;
	const auto clear_sinks = [&] { std::fill_n(m.plat_acc, 4, 0); };
	if (boat) {
		if ((e.flags & kEntityFlagInAir) == 0 && up < 0 && !m.settle_2f0)
			m.settle_2f0 = 1;
		if (authority) {
			set_parked(e, m.settle_2f0 != 0);
			if ((e.flags & kEntityFlagInAir) == 0 && up > 0)
				set_parked(e, false);
		}
	} else {
		if (!tank && !bike && m.crashed && m.byte_2ef && contacts != nullptr) {
			if ((contacts[0] && contacts[2]) || (contacts[1] && contacts[3]) || contacts[6]) {
				m.wreck_2fc = 1;
				clear_sinks();
				vehicle_clear_chassis(m);
				m.byte_2ef = m.crashed = 0;
			} else
				m.wreck_2fc = 0;
		}
		if (!bike && m.wreck_2fc && m.speed < 32 && !m.settle_2f0)
			m.settle_2f0 = 1;
		if (!tank && !bike && authority)
			set_parked(e,
					m.crashed || m.settle_2f0 ||
							(t.family == VehicleFamily::Ground && m.wreck_2fc));
		if (!bike && m.crashed && !m.settle_2f0 && m.speed > 32)
			clear_sinks();
	}
	if (e.health > 0 && m.settle_2f0) {
		if (!m.damage_smoke_active) {
			vehicle_smoke_effect(world, e);
			m.movement_effects_disabled = true;
		}
		if (!boat && authority && ((e.flags | e.engine_flags) & 0x4000000u) == 0 &&
				e.health > t.critical_drain) {
			e.health -= 5;
			e.last_attacker = {};
		}
		const int32_t fire_threshold = vehicle_family_uses_direct_air_mover(t.family)
				? 20 * t.critical_drain
				: e.health_max >> 2;
		if (tank || e.health > t.critical_drain) {
			if (e.health <= fire_threshold)
				damage_effect(world, e, 5, fire_effect(t, true), false);
		} else if (m.damage_smoke_active)
			vehicle_release_damage_effects(world, e);
		if (!tank && !bike && !boat && authority)
			set_parked(e, true);
		if (!bike && !boat && (e.flags & 0x10u) == 0)
			vehicle_rebuild_rest_orientation(world, e, false);
	}
	if (boat) {
		if ((e.flags & 0x10u) != 0 && up > 0)
			vehicle_rebuild_rest_orientation(world, e, true);
		else if ((e.flags & 0x10u) == 0 && up < 0)
			vehicle_rebuild_rest_orientation(world, e, false);
	} else if (!bike && !m.crashed && !m.wreck_2fc && up < 0 && (e.flags & 0x10u) == 0 &&
			(!tank || (!authority && !m.byte_2ef))) {
		vehicle_rebuild_rest_orientation(world, e, false);
	}
	m.spring_energy = std::max(0, m.spring_energy);
	// The tank solve has no such clear: its sinks change only at the crashed
	// clear, the wreck latch, the diagonal reset and the contact tail.
	// [orig: tank sink stores @0x47810D, @0x4785E6, @0x47902E, @0x47931F]
	if (!boat && !bike && !tank &&
			((m.wreck_2fc && up < 0) ||
					((e.flags & kEntityFlagInAir) != 0 &&
							to_fixed(e.position.z) < world.env.water_z)))
		clear_sinks();
}
} // namespace opennova::world::detail
