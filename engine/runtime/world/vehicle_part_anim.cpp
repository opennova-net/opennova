// The vehicle part-animation tick: the rotor spin machine (ground or helo,
// by the brain's profile type) and the wheel phase, run at the tail of every
// family mover.
// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0 (ground, profile type 2);
//  the HELO twin @0x48FA70 (profile type 1, decay 46603); the wheel phase
//  increment @0x48C4C5..0x48C4F4 / the watercraft form @0x48E9F0..0x48E9F9]

#include <runtime/world/vehicle_part_anim.h>

#include <runtime/world/ai.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// The class gate [orig: `brain+4 -> profile+16` @0x4928C9..0x4928D1 / the
// `== 1` twin inside @0x48FA70]. A row with no brain has no profile to read;
// its family stands in (vehicle_part_anim.h).
RotorMachine machine_for(World &world, const Entity &veh, const VehicleTraits &traits) {
	if (const AiEntity *ai = world.ai.for_handle(veh.handle))
		return rotor_machine_for_profile(ai->profile.type);
	return (traits.family == VehicleFamily::Helicopter ||
	        traits.family == VehicleFamily::Plane)
			? RotorMachine::Helo
			: RotorMachine::Ground;
}

} // namespace

void vehicle_part_anim_tick(World &world, Entity &veh, const VehicleTraits &traits) {
	Entity::VehicleMotorState &m = veh.veh;
	// A WATERCRAFT runs neither machine: Entity_UpdateWatercraftPhysics
	// @0x48D480..0x48EF74 has no call to @0x4928B0 or @0x48FA70 (the ground
	// machine's callers are the infantry, air, light, mounted-infantry, tank
	// and ground movers @0x46F99E/@0x4700F5/@0x4869EA/@0x4889F5/@0x48AE3D/
	// @0x48D42B; the helo twin's only caller is @0x4905A6) — so a boat never
	// draws the rotor roll from the shared stream; only its wheel phase
	// advances @0x48E9F0..0x48E9F9.
	const RotorMachine machine = traits.family == VehicleFamily::Watercraft
			? RotorMachine::None
			: machine_for(world, veh, traits);
	if (machine != RotorMachine::None) {
		// The engine-running latch: the +0x170 occupantEntity read @0x4928E8 is
		// the claimant that Entity::primary_occupant mirrors (attach/detach own
		// it), not any-control-seat occupancy.
		const bool occupied = veh.primary_occupant.valid();

		// Seed the rate when it is zero. A non-player-control item rolls from
		// the shared stream — the roll is the one PRNG draw this tick takes,
		// and an unoccupied such item takes it EVERY tick because the
		// unoccupied arm resets the rate [orig: @0x4928DF..0x492935].
		int32_t rolled = 0;
		if (rotor_rate_needs_roll(m.part_spin, traits.player_control))
			rolled = rotor_rate_from_roll(world.next_prng16());
		rotor_seed_rate(m.part_spin, traits.player_control, occupied, rolled);
		rotor_tick(m.part_spin, occupied, rotor_decay_for(machine));
	}

	// The wheel phase: |slip| + (speed << 13) [orig: @0x48C4C5..0x48C4D0]; the
	// slip register (+0x46C) rides the D-NET-161 level-frame re-derive and is
	// 0 here.
	if (traits.family == VehicleFamily::Watercraft)
		m.wheel_phase = watercraft_wheel_phase_step(m.wheel_phase, m.cmd_speed);
	else
		m.wheel_phase = wheel_phase_step(m.wheel_phase, m.speed, /*slip_abs=*/0);
}

} // namespace opennova::world
