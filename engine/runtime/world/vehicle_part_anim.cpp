#include <runtime/world/vehicle_system.h>
// The vehicle part-animation tick: the rotor spin machine (ground or helo,
// by the brain's profile type) and the wheel phase. Ground runs at the
// mover tail; the aircraft rotor runs at its head before the lift gate.
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
	const RotorMachine called = vehicle_family_uses_direct_air_mover(traits.family)
			? RotorMachine::Helo
			: RotorMachine::Ground;
	if (const AiEntity *ai = world.ai.for_handle(veh.handle))
		return rotor_machine_for_profile(ai->profile.type) == called ? called : RotorMachine::None;
	return called;
}

} // namespace

// The mover commits all six staged words only inside the strict alignment
// threshold. Outside it, only yaw changes; pitch and range stay active.
// [orig: cveh @0x48D0E3..0x48D15D; cbik @0x48669A..0x486711;
// ctan @0x48AA23..0x48AA9E; aircraft @0x492694..0x492703]
void VehicleSystem::slew_turret(Entity &veh, int32_t step) {
	AiEntity *ai = world_.ai.for_handle(veh.handle);
	if (ai == nullptr)
		return;
	AiBrain &b = ai->brain;
	const int32_t delta = io::bam_sub(b.f[AiBrain::kStagingBlock + 3], b.f[AiBrain::kActiveYaw]);
	if (io::bam_abs(delta) < 0x2108421u) {
		for (int i = 0; i < 6; ++i)
			b.f[AiBrain::kActiveBlock + i] = b.f[AiBrain::kStagingBlock + i];
	} else {
		b.f[AiBrain::kActiveYaw] = delta > 0 ? io::bam_add(b.f[AiBrain::kActiveYaw], step)
											 : io::bam_sub(b.f[AiBrain::kActiveYaw], step);
	}
}

void VehicleSystem::part_anim_tick(Entity &veh, const VehicleTraits &traits) {
    World &world = world_;
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
		if (machine == RotorMachine::Helo && occupied)
			play_rotor_start_sound(veh, traits);
		rotor_seed_rate(m.part_spin, traits.player_control, occupied, rolled);
		rotor_tick(m.part_spin, occupied, rotor_decay_for(machine));
		if (machine == RotorMachine::Helo) {
			world.rotor_wash.update(veh, traits);
			update_rotor_sound(veh, traits);
		}
	}

	// Boat phase is command-driven; ground/bike wheelspin runs before contacts
	// in vehicle_wheel_traction_tick [orig: @0x48C4C5..0x48C4D0].
	if (traits.family == VehicleFamily::Watercraft)
		m.wheel_phase = watercraft_wheel_phase_step(m.wheel_phase, m.cmd_speed);
}

} // namespace opennova::world
