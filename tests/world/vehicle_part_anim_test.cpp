// Vehicle part-animation registers: the rotor spin machine, the PRNG-rolled
// rate for non-player-control items, the high-word register and the wheel
// phase — the pure machine, and then the same machine driven through the
// live mover tail on a Rig (the claimant latch, the shared-stream draws, the
// register projection).
// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0; the spawn seed
//  @0x40EE70..0x40EE94; the register reads @0x492ACA / @0x4929B4]

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cmath>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// ------------------------------------------------------------------ pure pins

// A player-control rotor spins up and down at the SAME rate, so it takes as
// long to stop as to start — and it never touches the PRNG.
void test_player_control_rotor_symmetry() {
	RotorState s;
	CHECK(!rotor_rate_needs_roll(s, true), "a player-control item never rolls");
	rotor_seed_rate(s, true, true, 0);
	CHECK(s.rate == kRotorRateFull, "occupied: the full rate");
	for (int i = 0; i < 5; ++i) rotor_tick(s, true);
	CHECK(s.speed == kRotorRateFull * 5, "spin-up is linear");
	CHECK(s.angle == kRotorRateFull * (1 + 2 + 3 + 4 + 5),
			"the angle accumulates the speed every tick");

	for (int i = 0; i < 5; ++i) rotor_tick(s, false);
	CHECK(s.speed == 0, "spin-down takes the same number of ticks");
	CHECK(s.rate == 0, "an unoccupied tick clears the rate");

	// Unoccupied, a player-control item seeds NOTHING.
	RotorState idle;
	rotor_seed_rate(idle, true, false, 0);
	CHECK(idle.rate == 0, "unoccupied player-control seeds no rate");

	// Both ends clamp rather than running away.
	RotorState fast;
	fast.rate = kRotorRateFull;
	fast.speed = kRotorSpeedMax;
	rotor_tick(fast, true);
	CHECK(fast.speed == kRotorSpeedMax, "spin-up caps");
	RotorState low;
	low.speed = kRotorRateFull / 2;
	rotor_tick(low, false);
	CHECK(low.speed == 0, "the last step down lands on zero, not below");
}

// A NON-player-control item rolls its spin-up rate from the shared PRNG
// stream: three rates by percentage band, and it re-rolls every unoccupied
// tick because the rate is reset to zero.
void test_rolled_rate() {
	CHECK(rotor_rate_from_roll(0) == kRotorRateFull, "0% -> full");
	CHECK(rotor_rate_from_roll(33) == kRotorRateFull, "33 -> full (> 33 is the edge)");
	CHECK(rotor_rate_from_roll(34) == kRotorRateMid, "34 -> mid");
	CHECK(rotor_rate_from_roll(66) == kRotorRateMid, "66 -> mid (> 66 is the edge)");
	CHECK(rotor_rate_from_roll(67) == kRotorRateLow, "67 -> low");
	CHECK(rotor_rate_from_roll(99) == kRotorRateLow, "99 -> low");
	CHECK(rotor_rate_from_roll(100) == kRotorRateFull, "the roll is % 100");

	RotorState s;
	CHECK(rotor_rate_needs_roll(s, false), "zero rate on a non-player item rolls");
	rotor_seed_rate(s, false, false, kRotorRateLow);
	CHECK(s.rate == kRotorRateLow, "the rolled rate is taken even unoccupied");
	CHECK(!rotor_rate_needs_roll(s, false), "a seeded rate does not roll again");
	rotor_tick(s, false);
	CHECK(s.rate == 0 && rotor_rate_needs_roll(s, false),
			"an unoccupied tick resets the rate, so the next tick rolls again");

	// Spin-up at the rolled rate, spin-down at the FULL rate: a slow-start
	// rotor stops faster than it started.
	RotorState r;
	rotor_seed_rate(r, false, true, kRotorRateLow);
	rotor_tick(r, true);
	CHECK(r.speed == kRotorRateLow, "spin-up uses the rolled rate");
	rotor_tick(r, false);
	CHECK(r.speed == 0, "spin-down uses the full rate and lands on zero");
}

// The spawn seed: full speed at once, the full rate armed.
void test_spawn_full() {
	RotorState s;
	rotor_spawn_full(s);
	CHECK(s.speed == kRotorSpeedMax && s.rate == kRotorRateFull,
			"a spawned-in-flight rotor is already at full speed");
	CHECK(kSpawnRotorFullBit == 0x20000, "the record flag");
}

// The PANM track reads the HIGH WORD, so the accumulator carries far more
// precision than the animation samples.
void test_register_is_the_high_word() {
	CHECK(part_register(0x00010000) == 1, "bit 16 is register 1");
	CHECK(part_register(0x0001FFFF) == 1,
			"the low word is precision the track never sees");
	CHECK(part_register(0x00020000) == 2, "and rolls at the word boundary");
	// A negative accumulator's high word wraps as unsigned — the steer track
	// relies on that.
	CHECK(part_register(-0x10000) == 0xFFFF, "negatives wrap into the word");
}

// Wheels advance from speed; the slip kick is a recorded gap a peer runs at 0.
void test_wheel_phase() {
	CHECK(wheel_phase_step(0, 1, 0) == (1 << 13), "speed drives the phase");
	CHECK(wheel_phase_step(0, 0, 0) == 0, "a stopped wheel does not turn");
	// The slip kick adds on top when present.
	CHECK(wheel_phase_step(0, 1, 500) == 500 + (1 << 13),
			"an active skid kicks the phase");
	// Watercraft ride the forward command instead.
	CHECK(watercraft_wheel_phase_step(0, 3) == (3 << 13),
			"watercraft use the brain's forward command");
}

// ------------------------------------------------------------------ the Rig

// The vehicle_motor_test Rig: a PlayerControl dune buggy with a control seat
// and a player organic to mount into it.
struct Rig {
	World w;
	EntityHandle veh_h, drv_h;
	Rig() {
		w.registry.configure_pool(0, 16);
		w.registry.configure_pool(1, 16);
		Entity veh;
		veh.net_id = 200;
		veh.bms_id = 77;
		veh.spawn_origin = (1u << 24) | 3u;
		veh.kind = EntityKind::Item;
		veh.has_item_def = true; // seats need the def (D-NET-422)
		veh.item_id = 1291;
		// Control seats need PlayerControl [orig: Entity_AttachToVehicleSlot @0x4947cc].
		veh.item_attrib = kItemAttribPlayerControl;
		veh.position = {100.0f, 200.0f, 10.0f};
		veh.yaw = 0;
		veh.health = 3000;
		veh.health_max = 3000;
		veh.alive = true;
		Seat ctrl;
		ctrl.type = SeatType::Controller;
		ctrl.bone_index = 1;
		ctrl.source_name = "ctrlx00";
		veh.seats.push_back(ctrl);
		veh_h = w.registry.spawn(1, veh);

		Entity drv;
		drv.kind = EntityKind::Organic;
		drv.item_id = 5305;
		drv.player_class = 8;
		drv.position = {100.0f, 199.0f, 10.0f};
		drv.health = 150;
		drv.health_max = 150;
		drv.alive = true;
		drv_h = w.registry.spawn(0, drv);
	}
	Entity &veh() { return *w.registry.get(veh_h); }
	Entity &drv() { return *w.registry.get(drv_h); }
	void mount() { CHECK(w.vehicles.process_attach(drv_h, veh_h, 1), "mount"); }
	void tick(int n, const VehicleTraits &t) {
		for (int i = 0; i < n; ++i) w.vehicles.tick_motor(veh(), t);
	}
};

VehicleTraits buggy_traits(bool player_control) {
	VehicleTraits t;
	t.physics = 1;
	t.player_speed = 94 * 293;
	t.acceleration = 15 * 4;
	t.deceleration = 70 * 4;
	t.turn_rate = 65 * 192426;
	t.turn_rate2 = 41 * 192426;
	t.player_control = player_control;
	return t;
}

// A PLAYER-CONTROL vehicle's rotor spins only while its claimant is seated
// (the +0x170 latch, not any seat), at the full rate, and decays 186413/tick
// after the dismount — and the shared PRNG stream is never touched.
void test_player_control_rotor_follows_the_claimant() {
	Rig r;
	const VehicleTraits t = buggy_traits(true);
	const uint32_t prng0 = r.w.prng16_state;
	r.tick(3, t);
	CHECK(r.veh().veh.part_spin.speed == 0 && r.veh().veh.part_spin.rate == 0,
			"unoccupied: no rate, no spin");
	CHECK(r.w.prng16_state == prng0,
			"a player-control item never draws from the shared stream");
	r.mount();
	CHECK(r.veh().primary_occupant.valid(), "the claimant latched");
	r.tick(4, t);
	CHECK(r.veh().veh.part_spin.rate == kRotorRateFull, "occupied: the full rate");
	CHECK(r.veh().veh.part_spin.speed == 4 * kRotorRateFull,
			"four ticks of linear spin-up");
	CHECK(r.w.prng16_state == prng0, "still no PRNG draw");
	// Dismount: the claimant clears, the spin-down takes the full rate.
	CHECK(r.w.vehicles.detach(r.drv_h), "dismount");
	r.tick(1, t);
	CHECK(!r.veh().primary_occupant.valid(), "the claimant cleared");
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull,
			"one unoccupied tick sheds 186413");
	CHECK(r.veh().veh.part_spin.rate == 0, "and clears the rate");
	r.tick(10, t);
	CHECK(r.veh().veh.part_spin.speed == 0, "spin-down lands on zero");
	CHECK(r.w.prng16_state == prng0,
			"the whole spin-down drew nothing from the shared stream");
}

// A NON-player-control item re-rolls its rate EVERY unoccupied tick of the
// MACHINE — one draw from the shared stream per tick, no more, no less — so
// the stream position after N machine ticks is exactly N draws on. The full
// mover never reaches the machine for such an item: Entity_UpdatePartSpinAccumulator
// is called only inside the `itemDef->attrib & 0x40` block (cveh gate @0x48D38B,
// call @0x48D42B; ctan @0x48AD97/@0x48AE3D; cbik @0x486944/@0x4869EA), so through
// tick_motor the stream is untouched and the spin state stays zero.
void test_non_player_control_rolls_once_per_unoccupied_tick() {
	Rig r;
	const VehicleTraits t = buggy_traits(false);
	const uint32_t mover0 = r.w.prng16_state;
	r.tick(3, t);
	CHECK(r.w.prng16_state == mover0,
			"the full mover draws nothing for a non-PlayerControl item");
	CHECK(r.veh().veh.part_spin.rate == 0 && r.veh().veh.part_spin.speed == 0,
			"and never spins it");
	World probe;
	probe.prng16_state = r.w.prng16_state;
	const int kTicks = 17;
	for (int i = 0; i < kTicks; ++i) r.w.vehicles.part_anim_tick(r.veh(), t);
	for (int i = 0; i < kTicks; ++i) (void)probe.next_prng16();
	CHECK(r.w.prng16_state == probe.prng16_state,
			"exactly one shared-stream draw per unoccupied tick");
	// The seeded rate is one of the three rolled bands, and the unoccupied arm
	// clears it again — so after the tick it reads zero.
	CHECK(r.veh().veh.part_spin.rate == 0,
			"the unoccupied arm resets the rolled rate");
	// Occupied, the rolled rate persists and the stream is left alone.
	Entity &veh = r.veh();
	veh.primary_occupant = r.drv_h;
	Entity &drv = r.drv();
	drv.mounted = true;
	drv.mount_target = r.veh_h;
	drv.mount_type = SeatType::Controller;
	veh.seats[0].occupant = r.drv_h;
	const uint32_t before = r.w.prng16_state;
	r.w.vehicles.part_anim_tick(veh, t);
	const int32_t rolled = veh.veh.part_spin.rate;
	CHECK(rolled == kRotorRateFull || rolled == kRotorRateMid ||
					rolled == kRotorRateLow,
			"an occupied non-player item seeds one of the three rolled rates");
	CHECK(r.w.prng16_state != before, "the seed took one draw");
	const uint32_t after = r.w.prng16_state;
	for (int i = 0; i < 5; ++i) r.w.vehicles.part_anim_tick(veh, t);
	CHECK(veh.veh.part_spin.rate == rolled && r.w.prng16_state == after,
			"a seeded rate holds without further draws");
	CHECK(veh.veh.part_spin.speed == 6 * rolled, "spin-up at the rolled rate");
}

// The cap binds through the live tick.
void test_spin_caps_through_the_mover() {
	Rig r;
	const VehicleTraits t = buggy_traits(true);
	r.mount();
	r.tick(2000, t);
	CHECK(r.veh().veh.part_spin.speed == kRotorSpeedMax,
			"the rotor speed caps at 214748352");
}

// The projection publishes the accumulators' HIGH words.
void test_register_projection_through_the_mover() {
	Rig r;
	const VehicleTraits t = buggy_traits(true);
	r.mount();
	r.tick(3, t);
	const Entity::VehicleMotorState &m = r.veh().veh;
	const VehicleCtrlRegisters regs = vehicle_ctrl_registers(m);
	CHECK(regs.rotor == static_cast<int32_t>(part_register(m.part_spin.angle)),
			"PF_VEHICLE_ROTOR is the angle accumulator's high word");
	CHECK(regs.tail_rotor == regs.rotor,
			"the tail rotor publishes the rotor word (pending its own witness)");
	CHECK(regs.wheels == static_cast<int32_t>(part_register(m.wheel_phase)),
			"PF_VEHICLE_WHEELS is the wheel phase's high word");
	CHECK(m.part_spin.angle == kRotorRateFull * (1 + 2 + 3),
			"three ticks of angle accumulation");
}

// THE TWO MACHINES: the profile type picks ground (2) or helo (1), nothing
// else runs one; the helo twin's empty decay is 46603/tick against the ground
// machine's 186413.
void test_rotor_machines_by_profile_type() {
	CHECK(rotor_machine_for_profile(2) == RotorMachine::Ground, "type 2 -> ground");
	CHECK(rotor_machine_for_profile(1) == RotorMachine::Helo, "type 1 -> helo");
	CHECK(rotor_machine_for_profile(3) == RotorMachine::None, "type 3 (organic) -> none");
	CHECK(rotor_machine_for_profile(0) == RotorMachine::None, "unresolved -> none");
	CHECK(rotor_decay_for(RotorMachine::Helo) == kRotorDecayHelo &&
					rotor_decay_for(RotorMachine::Ground) == kRotorDecayGround,
			"each machine's decay");
	RotorState helo;
	helo.speed = 100000;
	rotor_tick(helo, false, kRotorDecayHelo);
	CHECK(helo.speed == 100000 - kRotorDecayHelo && helo.angle == helo.speed,
			"the helo twin decays 46603 and still adds the speed to the angle");
	RotorState ground;
	ground.speed = 100000;
	rotor_tick(ground, false, kRotorDecayGround);
	CHECK(ground.speed == 0, "the ground machine's 186413 stops it in one tick");
}

// Through the mover tail: a Helicopter-family row with no brain takes the helo
// machine (its family stands in for the profile), so its rotor winds down at
// 46603/tick after the dismount.
void test_helo_family_decays_at_the_helo_rate() {
	Rig r;
	VehicleTraits t = buggy_traits(true);
	t.family = VehicleFamily::Helicopter;
	r.mount();
	for (int i = 0; i < 3; ++i) r.w.vehicles.part_anim_tick(r.veh(), t);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull,
			"occupied: the full spin-up rate, both machines");
	CHECK(r.w.vehicles.detach(r.drv_h), "dismount");
	r.w.vehicles.part_anim_tick(r.veh(), t);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo,
			"unoccupied: the helo decay");
	VehicleTraits g = buggy_traits(true);
	r.w.vehicles.part_anim_tick(r.veh(), g);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo - kRotorDecayGround,
			"a ground-family row decays at the ground rate");
}

} // namespace

// A physics-keyed WATERCRAFT runs no rotor machine — its full mover never
// calls either — so a non-player-control boat draws NOTHING from the shared
// stream and its spin state stays zero; only the wheel phase advances, once
// per tick, from the forward command.
void test_watercraft_runs_no_rotor_machine() {
	Rig r;
	VehicleTraits t = buggy_traits(false);
	t.family = VehicleFamily::Watercraft;
	Entity &veh = r.veh();
	veh.veh.cmd_speed = 3;
	const uint32_t prng0 = r.w.prng16_state;
	for (int i = 0; i < 5; ++i) r.w.vehicles.part_anim_tick(veh, t);
	CHECK(r.w.prng16_state == prng0, "a boat never draws the rotor roll");
	CHECK(veh.veh.part_spin.rate == 0 && veh.veh.part_spin.speed == 0 &&
					veh.veh.part_spin.angle == 0,
			"a boat's spin state stays zero");
	CHECK(veh.veh.wheel_phase == 5 * (3 << 13),
			"the wheel phase rides the forward command, once per tick");
	// The authority boat tick runs the part-anim exactly once.
	t.physics = 1;
	veh.veh.wheel_phase = 0;
	r.w.vehicles.tick_watercraft_motor(veh, t, nullptr);
	CHECK(veh.veh.wheel_phase == (veh.veh.cmd_speed << 13),
			"one authority tick advances the phase by one step");
}

// The SELECTOR-ZERO boat mover (a cbot row with no physics key, or an afloat
// catv row without one: the shipped Drivable LCAC / Indo Landing Craft) runs
// the GROUND-profile spin machine inside its PlayerControl block at the head
// of the mover: a player-control hull seeds the full rate on its claimant with
// no PRNG draw, spins up 186413/tick to the cap, decays 186413/tick after the
// dismount, and never touches the wheel phase there (the mover's own
// command-driven step does). The claimant start/stop sound edge sits in the
// same block. [orig: Entity_ProcessAirVehiclePhysics @0x46FA00 gate @0x47004B,
//  claimant edge @0x470055..0x4700EB, Entity_UpdatePartSpinAccumulator
//  @0x4700F5; Entity_DispatchPhysics_cbot @0x48EFAC;
//  Entity_DispatchPhysicsUpdate @0x48F035]
void test_selector_zero_boat_runs_the_ground_machine() {
	for (int form = 0; form < 2; ++form) {
		Rig r;
		VehicleTraits t = buggy_traits(true);
		t.physics = 0;
		t.water_speed = t.player_speed;
		Entity &veh = r.veh();
		if (form == 0) {
			t.family = VehicleFamily::Watercraft; // cbot, no physics key
		} else {
			t.family = VehicleFamily::Ground; // catv, no physics key, afloat
			t.amphibian = true;
			veh.flags |= 0x8000u;
		}
		r.w.vehicles.traits.set(veh.item_id, t);
		auto &brain = *r.w.ai.at(r.w.ai.attach(r.veh_h));
		brain.profile.type = 2; // d_lcac: `type GROUND`
		const uint32_t prng0 = r.w.prng16_state;
		r.tick(2, t);
		CHECK(veh.veh.part_spin.rate == 0 && veh.veh.part_spin.speed == 0,
				"unoccupied: the selector-zero boat seeds no rate");
		r.mount();
		CHECK(veh.primary_occupant.valid(), "the claimant latched");
		r.tick(3, t);
		CHECK(veh.veh.part_spin.rate == kRotorRateFull,
				"occupied: the ground machine seeds the full rate on the boat");
		CHECK(veh.veh.part_spin.speed == 3 * kRotorRateFull,
				"the selector-zero boat spins up 186413/tick");
		CHECK(part_register(veh.veh.part_spin.angle) > 0,
				"the HELO_TAILROTOR register (the fan) turns");
		CHECK(r.w.prng16_state == prng0,
				"a player-control boat never draws the rotor roll");
		CHECK(veh.veh.wheel_phase == 0,
				"the block writes no wheel phase (the command is zero)");
		CHECK(r.w.vehicles.detach(r.drv_h), "dismount");
		r.tick(1, t);
		CHECK(veh.veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayGround,
				"unoccupied: the ground decay");
		CHECK(veh.veh.part_spin.rate == 0, "the unoccupied tick clears the rate");
	}
	// The physics-keyed boat keeps no machine at all.
	{
		Rig r;
		VehicleTraits t = buggy_traits(true);
		t.family = VehicleFamily::Watercraft;
		t.water_speed = t.player_speed;
		r.w.vehicles.traits.set(r.veh().item_id, t);
		r.mount();
		for (int i = 0; i < 3; ++i) r.w.vehicles.tick_watercraft_motor(r.veh(), t, nullptr);
		CHECK(r.veh().veh.part_spin.rate == 0 && r.veh().veh.part_spin.speed == 0,
				"the full watercraft mover runs no rotor machine");
	}
}

void test_suspension_registers_and_rotor_threshold_tick() {
	Entity::VehicleMotorState state;
	state.wheel_comp[0] = -100;
	state.wheel_comp[1] = 0x2000;
	state.wheel_comp[2] = 0x20000;
	state.wheel_comp[3] = 0x6000;
	const auto regs = vehicle_ctrl_registers(state);
	CHECK((regs.tires == std::array<int32_t, 14>{ 0, 0x2000, 65486, 0x4000, 0x6000, 0x10000 }),
			"tires clamp after the midpoint average, rear order is right then left");
	CHECK(wheel_phase_step(INT32_MAX, -1, 1) == 2147475456,
			"reverse wheel accumulation wraps without signed-shift UB");
	RotorState rotor;
	rotor.angle = INT32_MAX;
	rotor.speed = 1;
	rotor_tick(rotor, true);
	CHECK(rotor.angle == INT32_MIN, "rotor angle wraps at the signed boundary");

	Rig r;
	VehicleTraits t = buggy_traits(true);
	t.family = VehicleFamily::Helicopter;
	t.climb_speed = 1000;
	r.mount();
	r.veh().veh.part_spin.speed = kRotorSpeedMax - kRotorRateFull;
	r.veh().veh.part_spin.rate = kRotorRateFull;
	r.veh().veh.ground_cache = 0;
	r.veh().veh.net_alt_target = 65536;
	r.veh().veh.net_climb = 65536;
	r.veh().veh.ai_drive = true;
	r.veh().veh.wheel_phase = 123;
	r.w.vehicles.aircraft_client_tick(r.veh(), t);
	CHECK(r.veh().veh.part_spin.speed == kRotorSpeedMax, "rotor reaches full speed");
	CHECK(r.veh().veh.net_engine_on, "collective opens on the threshold tick");
	CHECK(r.veh().veh.wheel_phase == 123, "the air mover has no ground wheel phase");
}

void test_tracks_and_turret_motor_commit() {
	{
		Rig r;
		auto t = buggy_traits(true);
		t.family = VehicleFamily::Helicopter;
		AiEntity &ai = *r.w.ai.at(r.w.ai.attach(r.veh_h));
		ai.profile.type = 2;
		r.veh().veh.part_spin.speed = 123;
		r.veh().veh.part_spin.rate = 456;
		r.w.vehicles.part_anim_tick(r.veh(), t);
		CHECK(r.veh().veh.part_spin.speed == 123 && r.veh().veh.part_spin.angle == 0,
				"air mover does not call the ground rotor machine for a ground profile");
		t.family = VehicleFamily::Ground;
		ai.profile.type = 1;
		r.w.vehicles.part_anim_tick(r.veh(), t);
		CHECK(r.veh().veh.part_spin.speed == 123,
				"ground mover does not call the helo rotor machine for a helo profile");
	}
	int32_t phases[2] = {};
	track_phase_tick(phases, 0, 8192);
	CHECK(phases[0] == -65536 && phases[1] == 65536,
			"stationary steering counter-rotates the two tracks");
	track_phase_tick(phases, -2, 0);
	CHECK(phases[0] == -131072 && phases[1] == 0, "reverse drive advances both tracks backwards");
	for (VehicleFamily family :
			{ VehicleFamily::Ground, VehicleFamily::Bike, VehicleFamily::Tank }) {
		Rig r;
		auto t = buggy_traits(true);
		t.family = family;
		AiEntity &ai = *r.w.ai.at(r.w.ai.attach(r.veh_h));
		ai.profile.type = 2;
		for (int i = 0; i < 6; ++i)
			ai.brain.f[AiBrain::kStagingBlock + i] = 10 + i;
		ai.brain.f[AiBrain::kStagingBlock + 3] = 0x2108421;
		ai.brain.f[AiBrain::kActivePitch] = 99;
		r.w.vehicles.tick_motor(r.veh(), t);
		CHECK(ai.brain.f[AiBrain::kActiveYaw] == 0x2108421,
				"the exact threshold slews yaw by one ground step");
		CHECK(ai.brain.f[AiBrain::kActivePitch] == 99,
				"pitch does not commit on the equality boundary");
		r.w.vehicles.tick_motor(r.veh(), t);
		for (int i = 0; i < 6; ++i)
			CHECK(ai.brain.f[AiBrain::kActiveBlock + i] == ai.brain.f[AiBrain::kStagingBlock + i],
					"alignment commits the entire staged transform");
		ai.brain.f[AiBrain::kActiveYaw] = INT32_MAX;
		ai.brain.f[AiBrain::kStagingBlock + 3] = INT32_MIN + 10;
		r.w.vehicles.tick_motor(r.veh(), t);
		CHECK(ai.brain.f[AiBrain::kActiveYaw] == INT32_MIN + 10,
				"yaw alignment uses the wrapped shortest delta");
		ai.brain.f[AiBrain::kActivePitch] = -65536;
		r.veh().veh.track_phase[0] = -65536;
		r.veh().veh.track_phase[1] = 0x23450000;
		const auto tank = vehicle_ctrl_registers(r.veh().veh, VehicleRenderFamily::Tank, &ai);
		CHECK(tank.tracks[0] == 65535 && tank.tracks[1] == 0x2345,
				"tank track phase words are unsigned");
		CHECK(tank.gun_yaw == -32768 && tank.gun_pitch == -1,
				"turret yaw and pitch words are signed");
		CHECK((tank.mask & VC_TRACKS) && !(tank.mask & VC_TIRES) && (tank.mask & VC_VEHICLE_GUN),
				"tank render selects track and turret ownership");
		ai.profile.type = 1;
		const auto helo = vehicle_ctrl_registers(r.veh().veh, VehicleRenderFamily::Helicopter, &ai);
		CHECK(helo.mask == (VC_ROTORS | VC_HELO_GUN | VC_HELO_GEAR),
				"helo owns its rotor and gun channels");
	}
}

void test_tank_fourteen_tire_projection() {
	Entity::VehicleMotorState m;
	const int32_t compression[6] = { 1000, 2000, 4000, 8000, 4500, 3000 };
	for (int i = 0; i < 6; ++i)
		m.wheel_comp[i] = compression[i];
	const auto controls = vehicle_ctrl_registers(m, VehicleRenderFamily::Tank);
	const int32_t expected[14] = { 1000, 1000, 2750, 4455, 6250, 8000, 8000, 2000, 2000, 2500, 2970,
		3500, 4000, 4000 };
	CHECK((controls.mask & VC_TANK_TIRES) != 0, "tank owns all fourteen tires");
	for (int i = 0; i < 14; ++i)
		CHECK(controls.tires[i] == expected[i], "tank tire projection");
	m.wheel_comp[0] = -2000;
	m.wheel_comp[4] = 1000;
	const auto negative = vehicle_ctrl_registers(m, VehicleRenderFamily::Tank);
	CHECK(negative.tires[2] == 0, "mean is computed before its nonnegative clamp");
	m.gear_phase = 65000;
	const auto helo = vehicle_ctrl_registers(m, VehicleRenderFamily::Helicopter);
	CHECK(helo.gear == 65000, "gear uses the unsigned low word");
}

void test_rotor_wash_particles_and_lifetime() {
	World world;
	world.registry.configure_pool(1, 4);
	Entity seed;
	seed.kind = EntityKind::Item;
	seed.position = { 0, 0, 6 };
	seed.yaw = 90;
	seed.health = seed.health_max = 1000;
	const auto h = world.registry.spawn(1, seed);
	Entity &helo = *world.registry.get(h);
	VehicleTraits traits;
	traits.family = VehicleFamily::Helicopter;
	traits.player_speed = 20000;
	traits.player_control = true;
	helo.veh.part_spin.speed = kRotorSpeedMax;
	helo.veh.vel_x = 1000;
	world.out.fire_sounds.set_listener({ 0, 0, 6 });
	world.vehicles.part_anim_tick(helo, traits);
	CHECK(world.rotor_wash.active_count() == 1, "rotor motor allocates one focal-wind slot");
	const auto slot = helo.veh.rotor_wash_handle;
	world.vehicles.part_anim_tick(helo, traits);
	CHECK(helo.veh.rotor_wash_handle == slot && world.rotor_wash.active_count() == 1,
			"rotor updates reuse their slot");
	namespace p = opennova::particle;
	p::ParticleDef def;
	def.flags = p::particle_flag::FocalWind;
	def.move = p::move_flag::Normal;
	def.emit_burst = 0;
	def.emit_rate = 0;
	p::Emitter emitter;
	p::emitter_init(emitter, &def, {}, 1);
	p::Particle sample;
	sample.position = { 1, 3, 0 };
	sample.age = sample.lifetime = 10;
	sample.curve_phase = 14;
	sample.phase_rate = 62.5f;
	emitter.particles.push_back(sample);
	{
		p::EmitterEnvironment env;
		env.forces = &world.rotor_wash;
		p::emitter_advance(emitter, 0.016f, env);
	}
	CHECK(emitter.particles[0].force_zone == slot,
			"focal particle reacquires its zone at phase/index cadence");
	CHECK(emitter.particles[0].velocity.y < 0 && emitter.particles[0].velocity.x > 0,
			"inner rotor wash pulls down and pushes radially outward");
	p::Particle annulus = sample;
	annulus.position = { 13, 3, 0 };
	annulus.force_zone = slot;
	world.rotor_wash.apply(annulus, 0, false);
	CHECK(annulus.velocity.y > 0 && annulus.velocity.x < 0,
			"outer annulus adds lift and inward drag");
	int32_t magnitude = 0, direction[3];
	const int32_t position[3] = { 65536, 0, 2 * 65536 };
	CHECK(world.rotor_wash.sample_sway(position, magnitude, direction) && magnitude > 0,
			"foliage samples the same rotor field");
	Entity tree;
	tree.render_sway = true;
	tree.position = { 1, 0, 0 };
	const auto sway = world.rotor_wash.sway_pose(tree);
	CHECK(sway.active && sway.offset.x > 0,
			"Sway renderer gets the radial bend and shared wave pose");
	world.logic_tick += 3;
	const auto next_sway = world.rotor_wash.sway_pose(tree);
	CHECK(next_sway.active && next_sway.basis[0] != sway.basis[0],
			"foliage rotation follows the position-seeded clock");
	tree.render_sway = false;
	CHECK(!world.rotor_wash.sway_pose(tree).active,
			"ordinary renderers do not inherit tree deformation");
	// One persistent surface-effect group per zone: the first dust hit creates
	// it, every later hit only re-triggers it, and nothing is released while the
	// surface effect stays the same. [orig: WeatherParticle_UpdateAllEmitters
	// @0x5CB407..0x5CB4A0]
	using Kind = VehicleEffectEvent::Kind;
	world.env.water_z = -100 * 65536;
	for (int i = 0; i < 12; ++i) {
		++world.logic_tick;
		world.rotor_wash.tick();
	}
	int ensures = 0, triggers = 0, releases = 0;
	bool zone_bound = !world.out.vehicle_effects.empty();
	for (const auto &event : world.out.vehicle_effects) {
		zone_bound &= event.force_zone == slot && event.effect == "Effect_RwDust";
		if (event.kind == Kind::EnsureZoneGroup)
			++ensures;
		else if (event.kind == Kind::TriggerZoneGroup)
			++triggers;
		else if (event.kind == Kind::ReleaseZoneGroup)
			++releases;
		else
			zone_bound = false;
	}
	CHECK(zone_bound && ensures == 1 && triggers >= 1 && releases == 0,
			"ground-directed rays create one persistent dust group per zone and re-trigger it per hit");
	world.out.vehicle_effects.clear();
	world.env.water_z = 0;
	for (int i = 0; i < 12; ++i) {
		++world.logic_tick;
		world.rotor_wash.tick();
	}
	CHECK(world.out.vehicle_effects.size() >= 3 &&
					world.out.vehicle_effects[0].kind == Kind::ReleaseZoneGroup &&
					world.out.vehicle_effects[0].effect == "Effect_RwDust" &&
					world.out.vehicle_effects[1].kind == Kind::EnsureZoneGroup &&
					world.out.vehicle_effects[1].effect == "Effect_RwWater" &&
					world.out.vehicle_effects[2].kind == Kind::TriggerZoneGroup &&
					world.out.vehicle_effects[2].effect == "Effect_RwWater",
			"a surface change releases the dust group and creates the water group before re-triggering");
	world.env.water_z = 65536;
	for (int i = 0; i < 9; ++i) {
		++world.logic_tick;
		world.rotor_wash.tick();
	}
	bool ring = false;
	for (const auto &row : world.rotor_wash.water_wakes().rows())
		ring |= row.active;
	CHECK(ring, "downwash over a nonzero water plane produces the surface-ring bank");
	// Every spawn binds its zone: a zoneless emitter inside the pool searches
	// the nearest containing zone, an open spawn window wins without a search,
	// a spawn outside every zone stays zoneless, and the re-trigger spawn lands
	// at the hit with the zone as its window. Positions are render-frame
	// (x, up, -y): (1, 3, 0) is 3.2 units from the rotor at mission (0, 0, 6),
	// (400, 3, 0) is outside the 75-unit slot box.
	// [orig: CParticleEmitter_SpawnNewParticle @0x5F37C6..0x5F37D8;
	//  Terrain_FindNearestAmbientSoundZone @0x5CBCD0; CEffectWorld_SpawnAllActiveChildren
	//  @0x5E5E70]
	p::ParticleDef spawn_def = def;
	spawn_def.age = 1.0f;
	p::Emitter search;
	p::emitter_init(search, &spawn_def, { 1, 3, 0 }, 1);
	CHECK(p::emitter_spawn_one(search, &world.rotor_wash) &&
					search.particles.back().force_zone == slot,
			"a zoneless spawn inside the pool binds the nearest containing zone");
	search.force_zone = uint16_t(0x8000u | 77u);
	CHECK(p::emitter_spawn_one(search, &world.rotor_wash) &&
					search.particles.back().force_zone == uint16_t(0x8000u | 77u),
			"an open spawn window binds without a search");
	p::Emitter outside;
	p::emitter_init(outside, &spawn_def, { 400, 3, 0 }, 1);
	CHECK(p::emitter_spawn_one(outside, &world.rotor_wash) &&
					outside.particles.back().force_zone == 0,
			"a spawn outside every zone stays zoneless");
	CHECK(p::emitter_spawn_one_at(outside, { 2, 3, 0 }, { 0, 1, 0 }, slot, &world.rotor_wash) &&
					outside.particles.back().force_zone == slot &&
					outside.particles.back().position.x == 2.0f &&
					outside.particles.back().position.y == 3.0f,
			"the re-trigger spawn lands at the hit position with the zone as its window");
	world.vehicles.respawn(helo);
	CHECK(helo.veh.rotor_wash_handle == 0 && world.rotor_wash.active_count() == 0,
			"respawn releases the vehicle's wind slot");
	annulus.velocity = {};
	world.rotor_wash.apply(annulus, 0, false);
	CHECK(annulus.velocity.y == 0, "detached particles cannot use a retired wind slot");
}

void test_helicopter_sound_curves_and_decay() {
	World world;
	world.registry.configure_pool(1, 2);
	Entity seed;
	seed.health = seed.health_max = 1000;
	const auto handle = world.registry.spawn(1, seed);
	Entity &helo = *world.registry.get(handle);
	VehicleTraits traits;
	traits.family = VehicleFamily::Helicopter;
	traits.player_control = true;
	traits.climb_speed = 65536;
	// Authored like retail sndprof.def SP_Apache1 (soundloop_2 V_APACHE_ILP .8 1.2,
	// soundloop_3 V_APACHE_DLP .8 1.2, no soundloop_1) plus decoy Soundloop_5..7
	// that the helicopter lanes must never consult: lanes 21/11/1 read itemDef
	// soundLoopId[2]/[1]/[0] = Soundloop_3/2/1 (+2100/+2096/+2092
	// @0x52919D/@0x5291ED/@0x529235), each with lifetime 15 (+16 @0x528F94).
	const char profile[] = "begin Rotor\r\n"
						   "Soundloop_2 V_APACHE_ILP .8 1.2\r\nSoundloop_3 V_APACHE_DLP .8 1.2\r\n"
						   "Soundloop_5 Decoy5\r\nSoundloop_6 Decoy6\r\nSoundloop_7 Decoy7\r\n"
						   "medloopfadeinstart 0\r\nmedloopfadeinend 50\r\n"
						   "medloopfadeoutstart 75\r\nmedloopfadeoutend 100\r\n"
						   "medlooppitchstart 0\r\nmedlooppitchend 100\r\n"
						   "medlooppitchstartp 50\r\nmedlooppitchendp 100\r\n"
						   "crsloopfadeinstart 50\r\ncrsloopfadeinend 100\r\n"
						   "crslooppitchstartp 60\r\ncrslooppitchendp 120\r\nend\r\n";
	CHECK(world.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1,
			"rotor sound profile parses its authored curves");
	helo.veh.part_spin.speed = kRotorSpeedMax;
	world.vehicles.update_rotor_sound(helo, traits);
	const auto retail = world.out.sound_emitters.drain();
	CHECK(retail.size() == 2,
			"a retail-shaped profile registers only the medium and cruise lanes");
	if (retail.size() == 2) {
		CHECK(retail[0].lane == 21 && retail[0].slot == 2 && retail[0].set_name == "V_APACHE_DLP" &&
						retail[0].pitch_q16 == 78600 && retail[0].volume_q8_8 == 65279 &&
						retail[0].lifetime_ticks == 15,
				"cruise is Soundloop_3 at lifetime 15 with its upper authored pitch");
		CHECK(retail[1].lane == 11 && retail[1].slot == 1 && retail[1].set_name == "V_APACHE_ILP" &&
						retail[1].volume_q8_8 == 0 && retail[1].lifetime_ticks == 15,
				"medium is Soundloop_2 and fades out at full rotor speed");
	}
	for (const auto &event : retail)
		CHECK(event.set_name.rfind("Decoy", 0) != 0, "Soundloop_5..7 are never consulted");
	// An item override for Soundloop_3 and an authored Soundloop_1 complete the
	// three lanes.
	traits.sound_loops[2] = "ItemCruise";
	traits.sound_loops[0] = "ItemLateral";
	world.vehicles.update_rotor_sound(helo, traits);
	const auto full = world.out.sound_emitters.drain();
	CHECK(full.size() == 3, "three rotor lanes are registered together");
	if (full.size() == 3) {
		CHECK(full[0].lane == 21 && full[0].set_name == "ItemCruise" &&
						full[0].pitch_q16 == 78600 && full[0].volume_q8_8 == 65279,
				"cruise uses the item override and its upper authored pitch");
		CHECK(full[1].lane == 11 && full[1].volume_q8_8 == 0,
				"medium fades out at full rotor speed");
		CHECK(full[2].lane == 1 && full[2].slot == 0 && full[2].set_name == "ItemLateral" &&
						full[2].pitch_q16 == 65536 && full[2].volume_q8_8 == 63240,
				"zero climb preserves the original negative-to-unsigned lateral volume");
	}
	helo.veh.part_spin.speed =
			static_cast<int32_t>((int64_t(32750) * kRotorSpeedMax + 65535) / 65536);
	world.vehicles.update_rotor_sound(helo, traits);
	const auto edge = world.out.sound_emitters.drain();
	CHECK(edge.size() == 3 && edge[1].volume_q8_8 == 65278,
			"the medium fade-in endpoint retains the reciprocal/ftol one-unit loss");
	CHECK(edge.size() == 3 && edge[0].volume_q8_8 == 0,
			"cruise starts at zero at the medium fade-in endpoint");
	helo.veh.part_spin.speed = kRotorSpeedMax;
	world.vehicles.part_anim_tick(helo, traits);
	CHECK(!world.out.sound_emitters.empty() && helo.veh.part_spin.speed < kRotorSpeedMax,
			"unoccupied helicopters refresh running loops while spinning down");
	world.out.sound_emitters.clear();
	helo.veh.part_spin.speed = 0;
	world.vehicles.part_anim_tick(helo, traits);
	CHECK(world.out.sound_emitters.empty(), "stopped rotors leave their old lanes to expire");
	helo.veh.part_spin.speed = kRotorSpeedMax;
	helo.engine_flags |= 2;
	world.vehicles.update_rotor_sound(helo, traits);
	CHECK(world.out.sound_emitters.empty(), "dead helicopters never refresh sound lanes");
}

void test_water_wake_ring_lifetime_and_geometry() {
	namespace r = opennova::renderer;
	r::WaterWakePool pool;
	pool.add(3 * 65536, -65536, 0.75f);
	CHECK(pool.rows()[0].x == 2 * 65536 && pool.rows()[0].y == -2 * 65536,
			"surface rings align down to the two-unit grid on both signs");
	for (int i = 0; i < 12; ++i)
		pool.tick();
	CHECK(pool.rows()[0].age == 12 && std::abs(pool.rows()[0].alpha - 0.75f) < 0.00001f,
			"water rings reach their authored opacity at tick twelve");
	r::WaterWakeFrame frame;
	const int32_t camera[3] = { 2 * 65536, -2 * 65536, 65536 };
	r::compile_water_wakes(pool, 65536, 64, camera, frame);
	CHECK(frame.vertices.size() == 171 && frame.indices.size() == 864,
			"water rings use nineteen angular columns and nine radial rows");
	// The ring is built in the render frame as (sin * r, 0, cos * r)
	// [orig: WaterRing_BuildMesh @ 0x5de01d..0x5de03e] and lands here
	// through the render -> presentation x/z swap: column 0 steps along +x,
	// column 1 turns 20 degrees toward +z.
	CHECK(std::abs(frame.vertices.front().x - 2.25f) < 0.00001f &&
					std::abs(frame.vertices.front().z - 2.0f) < 0.00001f &&
					std::abs(frame.vertices.front().y - 1.03125f) < 0.00001f &&
					frame.vertices.front().u == 0.125f && frame.vertices.front().v == -0.75f,
			"ring geometry carries the water depth bias and animated first UV");
	CHECK(std::abs(frame.vertices[1].x - (2.0f + 0.25f * std::cos(0.34906587f))) < 0.00001f &&
					std::abs(frame.vertices[1].z - (2.0f + 0.25f * std::sin(0.34906587f))) <
							0.00001f,
			"the ring's angular sense follows the render frame through the x/z swap");
	{
		// Retail's triangles are clockwise seen from above in the left-handed
		// render frame (the device's default CCW cull keeps them); the x/z swap
		// into the right-handed presentation frame keeps them clockwise from
		// above, so (b - a) x (c - a) points down there.
		const auto &a = frame.vertices[frame.indices[0]];
		const auto &b = frame.vertices[frame.indices[1]];
		const auto &c = frame.vertices[frame.indices[2]];
		const float normal_y = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
		CHECK(normal_y < 0.0f, "the ring's front faces point up");
	}
	CHECK(std::abs(frame.vertices[8 * 19].x - 22.25f) < 0.00001f && frame.vertices[8 * 19].v2 == 1,
			"outer ring reaches the authored twenty-unit gradient edge");
	for (int i = 0; i < 20; ++i)
		pool.tick();
	CHECK(!pool.rows()[0].active, "surface rings expire and compact at tick thirty-two");
	r::compile_water_wakes(pool, 65536, 84, camera, frame);
	CHECK(frame.vertices.empty(), "expired rings leave no draw geometry");
	for (int i = 0; i < 129; ++i)
		pool.add(i * 65536, 0, 1);
	CHECK(pool.rows()[127].x == 126 * 65536, "ring pool drops allocation after its 128 slots fill");
	for (int i = 0; i < 32; ++i)
		pool.tick();
	CHECK(!pool.rows()[0].active && !pool.rows()[127].active,
			"a full ring bank clears its vacated tail and terminates compaction");
}

int main() {
	test_water_wake_ring_lifetime_and_geometry();
	test_helicopter_sound_curves_and_decay();
	test_rotor_wash_particles_and_lifetime();
	test_tank_fourteen_tire_projection();
	test_tracks_and_turret_motor_commit();
	test_suspension_registers_and_rotor_threshold_tick();

	test_watercraft_runs_no_rotor_machine();
	test_selector_zero_boat_runs_the_ground_machine();
	test_player_control_rotor_symmetry();
	test_rolled_rate();
	test_spawn_full();
	test_register_is_the_high_word();
	test_wheel_phase();
	test_player_control_rotor_follows_the_claimant();
	test_non_player_control_rolls_once_per_unoccupied_tick();
	test_spin_caps_through_the_mover();
	test_register_projection_through_the_mover();
	test_rotor_machines_by_profile_type();
	test_helo_family_decays_at_the_helo_rate();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_part_anim_test OK\n");
	return 0;
}
