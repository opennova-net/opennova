#include "vehicle_motor_detail.h"
#include "world.h"

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
	// Every motor-effect submit passes the vehicle as the descriptor tag, so
	// the smoke and fire groups take the section gate [orig:
	// Entity_UpdateVehiclePhysics `push esi` @ 0x48B0BB / @ 0x48B0F0 ahead of
	// the submits @ 0x48B0BC / 0x48B0F1, and the sibling pairs @ 0x46FBC9,
	// 0x478151, 0x47AC65, 0x47DDD9, 0x48093C, 0x48352B, 0x48419C, 0x488C8A,
	// 0x48D671, 0x49050C (esi = the entity, e.g. @ 0x475DF7)].
	event.section_tagged = true;
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
// 849188/849298 are Effect_sboatwake / Effect_SmlSplash. The sound is the
// crossing queue's: the host's fan sends it to every in-match player, dead or
// alive (mask 128), the authority's own through its loopback leg
// (Server_RouteWaterCrossings).
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
	// [orig: `test [esi+24h], 2000h` @0x464b16 -> g_SndBodyWater1 @0x464b1f /
	//  g_SndSurfaceWtr @0x464b2f -> Server_SendOverlayActionToAlive @0x464b3e]
	world.out.water_crossings.add(x, y, z, airborne);
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
		// [orig: Vehicle_ProcessSelectorZeroGround @0x46E100 (IDB misnomer, the
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
		//  Vehicle_ProcessSelectorZeroBoat (IDB misnomer, the selector-zero boat)
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
// The aircraft and tracked solves pass their contact copy (seven depths): a crashed, frozen
// (+0x2EF) hull with a diagonal pad pair down, or its middle spine probe pressed (> 0: the
// aircraft's probe 6, the tracked's probe 5, each solve's L/2 station), latches +0x2FC, clears
// its sinks, its chassis state and the +0x364 cue dword. The aircraft also drops its crash pair
// (+0x2EF, +0x2EC) there and has no miss arm; the tracked keeps the pair and clears +0x2FC on a
// miss. The authority parks an aircraft while crashed and unparks it once neither crashed nor
// wrecked (+0x2F0); the tracked parks while crashed or wrecked and unparks only when +0x2FC is
// clear too. The aircraft's inverted-rebuild gate does not read +0x2FC.
// [orig: ground @0x47DC68..0x47DF79 (the latch @0x47DC56..0x47DD04, `cmp [esp+..+var_248], ecx;
//  jle` @0x47DC86, the +0x364 clear @0x47DCFC, the miss clear @0x47DD04; the park @0x47DD30..0x47DD4C,
//  the unpark @0x47DD81..0x47DDA4; the rebuild gate @0x47DF79..0x47DFA2);
// tank @0x4780CD..0x478381 (growth @0x478510); bike @0x47AC46..0x47AD65;
// air @0x4807C6..0x480BC7 (the latch @0x4807C6..0x48087C, `cmp [esp+..+var_1D8], ecx; jle`
//  @0x4807F6, the pair clear @0x48082B..0x480831, the +0x364 clear @0x48087C; the park
//  @0x4808A8..0x4808BC, the unpark @0x4808ED..0x480907; the rebuild gate @0x480AE0..0x480AFC);
// boat @0x483474..0x48367D]
void vehicle_crash_state(
		World &world, Entity &e, const VehicleTraits &t, const int32_t *depths, int32_t up) {
	auto &m = e.veh;
	const bool boat = t.family == VehicleFamily::Watercraft;
	const bool tank = t.family == VehicleFamily::Tank;
	const bool bike = t.family == VehicleFamily::Bike;
	const bool air = vehicle_family_uses_direct_air_mover(t.family);
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
		if (!tank && !bike && m.crashed && m.byte_2ef && depths != nullptr) {
			const int32_t spine = depths[air ? 6 : 5];
			if ((depths[0] != 0 && depths[2] != 0) || (depths[1] != 0 && depths[3] != 0) ||
					spine > 0) {
				m.wreck_2fc = 1;
				clear_sinks();
				if (air)
					m.byte_2ef = m.crashed = 0;
				vehicle_clear_chassis(m);
				m.plat_at_rest = m.plat_porpoise = false;
			} else if (!air) {
				m.wreck_2fc = 0;
			}
		}
		if (!bike && m.wreck_2fc && m.speed < 32 && !m.settle_2f0)
			m.settle_2f0 = 1;
		if (!tank && !bike && authority && (m.crashed || (!air && m.settle_2f0)))
			set_parked(e, true);
		if (!bike && m.crashed && !m.settle_2f0 && m.speed > 32)
			clear_sinks();
		if (!tank && !bike && authority && !m.crashed && !m.settle_2f0 && (air || !m.wreck_2fc))
			set_parked(e, false);
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
	} else if (!bike && !m.crashed && (air || !m.wreck_2fc) && up < 0 && (e.flags & 0x10u) == 0 &&
			(!tank || (!authority && !m.byte_2ef))) {
		vehicle_rebuild_rest_orientation(world, e, false);
	}
	m.spring_energy = std::max(0, m.spring_energy);
}

// The aircraft and tracked solves flush the four sinks after their max-depth scans: a
// wreck at rest that is inverted, or an airborne hull under the water word, reads +0x2F0
// and the live Position.Z. The tank solve has no such clear: its sinks change only at
// the crashed clear, the wreck latch, the diagonal reset and the contact tail.
// [orig: aircraft `cmp [esi+2F0h], al` @0x480CEB, the up test @0x480CF3, Flags 0x2000
//  @0x480CF9, `[esi+0Ch]` vs g_EnvWaterHeightFixed @0x480D02..0x480D0B, the stores
//  @0x480D0D..0x480D1F; tracked @0x47E122..0x47E157; tank sink stores @0x47810D,
//  @0x4785E6, @0x47902E, @0x47931F]
void vehicle_flush_sinks(World &world, Entity &e, int32_t up_z16, int32_t pz) {
	auto &m = e.veh;
	if ((m.settle_2f0 && up_z16 < 0) ||
			((e.flags & kEntityFlagInAir) != 0 && pz < world.env.water_z))
		std::fill_n(m.plat_acc, 4, 0);
}
} // namespace opennova::world::detail
