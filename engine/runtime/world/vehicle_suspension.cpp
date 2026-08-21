// The ground-vehicle suspension spring leg: the parked latch with its
// role-picked disable rate, the per-wheel compress/oscillate step over the
// contact solve's pad depths, and the state reset.
// [orig: Entity_ProcessWheeledVehicleSuspension @0x46B140 latch
//  @0x46B1A6..0x46B213; Entity_ProcessTrackedVehiclePhysics @0x47C1C0 spring
//  dt @0x47C218..0x47C22B + the spring loop; Suspension_CompressWheelQuadratic
//  @0x45CFB0; Suspension_OscillateWheelFast @0x45D110;
//  Entity_ClearSuspensionState @0x4592B0]

#include "world/vehicle_suspension.h"

#include "world/ground_conform.h"
#include "world/vehicle_motor.h"
#include "world/world.h"

namespace opennova::world {

namespace {

// The parked-flow replication bit: the authority raises it at the latch edge
// and a client reads it there [orig: Flags |= 0x10 @0x46B1ED / test
// @0x46B1F3; D-NET-196's replicated Flags 0x10].
constexpr uint32_t kEntityFlagSuspensionParked = 0x10u;

} // namespace

void vehicle_suspension_clear(Entity::VehicleMotorState &m) {
	// WITNESS PENDING: Entity_ClearSuspensionState @0x4592B0's exact field
	// set. Every field this TU owns resets here.
	for (int k = 0; k < 4; ++k) {
		m.wheel_comp[k] = 0;
		m.wheel_osc[k] = Entity::VehicleMotorState::WheelOsc{};
	}
	m.spring_energy = 0;
}

bool vehicle_suspension_latch(World &world, Entity &veh) {
	Entity::VehicleMotorState &m = veh.veh;
	// WITNESS PENDING (see the header): armed off until the +0x2ED producer
	// and the +0x2EC clears are witnessed — the literal gate parks every
	// fresh row on its first tick.
	if (!kSuspensionLegArmed) return false;
	// The gate: the mover's disable request and the latch both clear
	// [orig: @0x46B1A6..0x46B1B9].
	if (m.susp_disable_req != 0 || m.susp_latched != 0) return false;

	// The one-shot role pick [orig: @0x46B1C5..0x46B1DB]. Stored on the
	// entity; the consumer is the per-wheel contact loop @0x46B22E (WITNESS
	// PENDING: what it scales).
	m.susp_rate_pick = world.vehicle_authority
			? kSuspensionDisableRateAuthority
			: kSuspensionDisableRateNonAuthority;
	m.susp_byte_2ef = 0; // [orig: @0x46B1E0-region]
	if (world.vehicle_authority) {
		veh.flags |= kEntityFlagSuspensionParked; // [orig: @0x46B1ED]
	} else if ((veh.flags & kEntityFlagSuspensionParked) == 0) {
		// A client takes the latch only when the authority's bit says so
		// [orig: the `test Flags, 0x10` @0x46B1F3 — the replicated park].
		return false;
	}
	m.susp_latched = 1;  // [orig: @0x46B1F9]
	m.susp_byte_2ee = 0; // [orig: the store after the latch]
	vehicle_suspension_clear(m); // [orig: Entity_ClearSuspensionState @0x4592B0 call @0x46B20E]
	return true;
}

float vehicle_suspension_dt(const Entity::VehicleMotorState &m) {
	return m.susp_latched != 0 ? kSuspensionDtParked : kSuspensionDtUnparked;
}

void vehicle_suspension_step(World &world, Entity &veh, const VehicleTraits &traits,
                             const int32_t pad_depths[4]) {
	(void)world;
	// WITNESS PENDING (see the header): the compress arm grows the pad
	// offsets, and the rest Z of the live solves moves with them — armed
	// together with the latch so the solves keep their witnessed zero-state
	// rest until the park machine is complete.
	if (!kSuspensionLegArmed) return;
	Entity::VehicleMotorState &m = veh.veh;
	// The travel bound from the def's spring_comp percentage and the dt by
	// the latch [orig: @0x47C51F..0x47C544; @0x47C218..0x47C22B].
	const int32_t travel = conform_travel_from_def(traits.spring_comp);
	const float dt = vehicle_suspension_dt(m);
	// One dt of compression in the kernel's 16.16 units: the step the
	// compressing arm integrates is the per-step delta (dword_815180 >> 4)
	// scaled by dt [orig: the `fmul dt; fistp` feeding the call @0x47E2C1].
	const int32_t step = static_cast<int32_t>(static_cast<float>(kSuspStep) * dt);
	for (int k = 0; k < 4; ++k) {
		ConformOscillator osc;
		osc.amplitude = m.wheel_osc[k].amplitude;
		osc.extension = m.wheel_osc[k].extension;
		osc.energy = m.wheel_osc[k].energy;
		osc.phase = m.wheel_osc[k].phase;
		if (pad_depths[k] > 0) {
			// A penetrating pad compresses [orig: the d > 0 arm ->
			// Suspension_CompressWheelQuadratic @0x47E2C1].
			(void)conform_spring_compress(osc, m.wheel_comp[k], m.spring_energy,
			                              step, travel, traits.spring);
		} else {
			// A wheel in the air releases through the oscillator [orig: the
			// else arm -> Suspension_OscillateWheelFast @0x47E2D3].
			(void)conform_spring_oscillate(osc, m.wheel_comp[k], m.spring_energy,
			                               traits.shock, traits.spring, m.slide_z);
		}
		m.wheel_osc[k].amplitude = osc.amplitude;
		m.wheel_osc[k].extension = osc.extension;
		m.wheel_osc[k].energy = osc.energy;
		m.wheel_osc[k].phase = osc.phase;
	}
	// WITNESS PENDING: the sink growth rates on airborne wheels (+250/tick
	// wheeled @?, +187 tracked @0x47DB59..0x47DBE4, +100 bike) and the
	// corner-lift feedback of the stepped compression into the conform.
}

} // namespace opennova::world
