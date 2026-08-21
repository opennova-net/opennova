// The vehicle part-animation tick: the rotor spin machine and the wheel
// phase, run at the tail of every family mover.
// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0; the wheel phase
//  increment @0x48C4C0..0x48C4F4 / the watercraft form @0x48E9F0..0x48E9F9]

#include "world/vehicle_part_anim.h"

#include "world/vehicle_motor.h"
#include "world/world.h"

namespace opennova::world {

void vehicle_part_anim_tick(World &world, Entity &veh, const VehicleTraits &traits) {
	Entity::VehicleMotorState &m = veh.veh;
	// WITNESS PENDING: the move-context class-2 gate @0x4928C9..0x4928D1 (which
	// struct's `+4 -> +16 == 2` it tests, and therefore which of our families
	// skip the rotor machine). Until witnessed every mover that retail calls
	// this from runs it — the six call sites cover every vehicle family.

	// The engine-running latch: the +0x170 occupantEntity read @0x4928E8 is
	// the claimant that Entity::primary_occupant mirrors (attach/detach own
	// it), not any-control-seat occupancy.
	const bool occupied = veh.primary_occupant.valid();

	// Seed the rate when it is zero. A non-player-control item rolls from the
	// shared stream — the roll is the one PRNG draw this tick takes, and an
	// unoccupied such item takes it EVERY tick because the unoccupied arm
	// resets the rate [orig: @0x4928DF..0x492935].
	int32_t rolled = 0;
	if (rotor_rate_needs_roll(m.part_spin, traits.player_control))
		rolled = rotor_rate_from_roll(world.next_prng16());
	rotor_seed_rate(m.part_spin, traits.player_control, occupied, rolled);
	rotor_tick(m.part_spin, occupied);

	// WITNESS PENDING: the +0x2B8 vs +0x46C wheel-phase form and the slip
	// source @0x48C4C0..0x48C4F4. The motor's own speed register drives the
	// phase; the skid kick rides the terrain-contact slip vector, which this
	// port does not yet carry (0).
	if (traits.family == VehicleFamily::Watercraft)
		m.wheel_phase = watercraft_wheel_phase_step(m.wheel_phase, m.cmd_speed);
	else
		m.wheel_phase = wheel_phase_step(m.wheel_phase, m.speed, /*slip_abs=*/0);
}

} // namespace opennova::world
