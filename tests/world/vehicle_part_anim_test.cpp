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
		veh.item_id = 1291;
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
	void mount() { CHECK(entity_process_vehicle_attach(w, drv_h, veh_h, 1), "mount"); }
	void tick(int n, const VehicleTraits &t) {
		for (int i = 0; i < n; ++i) tick_vehicle_motor(w, veh(), t);
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
	CHECK(entity_detach_from_vehicle(r.w, r.drv_h), "dismount");
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

// A NON-player-control item re-rolls its rate EVERY unoccupied tick — one
// draw from the shared stream per tick, no more, no less — so the stream
// position after N ticks is exactly N draws on.
void test_non_player_control_rolls_once_per_unoccupied_tick() {
	Rig r;
	const VehicleTraits t = buggy_traits(false);
	World probe;
	probe.prng16_state = r.w.prng16_state;
	const int kTicks = 17;
	r.tick(kTicks, t);
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
	r.tick(1, t);
	const int32_t rolled = veh.veh.part_spin.rate;
	CHECK(rolled == kRotorRateFull || rolled == kRotorRateMid ||
					rolled == kRotorRateLow,
			"an occupied non-player item seeds one of the three rolled rates");
	CHECK(r.w.prng16_state != before, "the seed took one draw");
	const uint32_t after = r.w.prng16_state;
	r.tick(5, t);
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
	for (int i = 0; i < 3; ++i) vehicle_part_anim_tick(r.w, r.veh(), t);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull,
			"occupied: the full spin-up rate, both machines");
	CHECK(entity_detach_from_vehicle(r.w, r.drv_h), "dismount");
	vehicle_part_anim_tick(r.w, r.veh(), t);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo,
			"unoccupied: the helo decay");
	VehicleTraits g = buggy_traits(true);
	vehicle_part_anim_tick(r.w, r.veh(), g);
	CHECK(r.veh().veh.part_spin.speed == 3 * kRotorRateFull - kRotorDecayHelo - kRotorDecayGround,
			"a ground-family row decays at the ground rate");
}

} // namespace

// A WATERCRAFT runs no rotor machine — its mover never calls either — so a
// non-player-control boat draws NOTHING from the shared stream and its spin
// state stays zero; only the wheel phase advances, once per tick, from the
// forward command.
void test_watercraft_runs_no_rotor_machine() {
	Rig r;
	VehicleTraits t = buggy_traits(false);
	t.family = VehicleFamily::Watercraft;
	Entity &veh = r.veh();
	veh.veh.cmd_speed = 3;
	const uint32_t prng0 = r.w.prng16_state;
	for (int i = 0; i < 5; ++i) vehicle_part_anim_tick(r.w, veh, t);
	CHECK(r.w.prng16_state == prng0, "a boat never draws the rotor roll");
	CHECK(veh.veh.part_spin.rate == 0 && veh.veh.part_spin.speed == 0 &&
					veh.veh.part_spin.angle == 0,
			"a boat's spin state stays zero");
	CHECK(veh.veh.wheel_phase == 5 * (3 << 13),
			"the wheel phase rides the forward command, once per tick");
	// The authority boat tick runs the part-anim exactly once.
	t.physics = 1;
	veh.veh.wheel_phase = 0;
	tick_watercraft_motor(r.w, veh, t, nullptr);
	CHECK(veh.veh.wheel_phase == (veh.veh.cmd_speed << 13),
			"one authority tick advances the phase by one step");
}

int main() {
	test_watercraft_runs_no_rotor_machine();
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
