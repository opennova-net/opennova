// The joiner-side aircraft attitude integrator, pinned THROUGH the live mover
// (the client-executed subset of Entity_UpdateAircraftPhysics @0x490310):
// the asymmetric rate damp and the two-gain sideslip pitch pick. These are the
// two behaviors a tidier rewrite gets wrong, and they are exercised here at the
// entry every predicted CHel/cpln row takes, not on a detached helper.
// [orig: Entity_UpdateAircraftPhysics @0x490310 — airborne damp
//  @0x492213..0x492246, grounded damp @0x492323..0x492365, sideslip pitch
//  @0x491F50..0x491FA5; the #553 `air_attitude.h` duplicate of these legs was
//  folded here 2026-08-21]

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

#include <base/io/bam.h>

#include <cstdio>
#include <cstdlib>

namespace {

namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

struct Rig {
	w::World world;
	w::EntityHandle heli;
	w::VehicleTraits traits;
};

// A boxless CHel row on a terrain-less world: the contact solve is inactive,
// so the airborne pick is the local derivation `ground == INT32_MIN ->
// airborne` [orig: the Flags read @0x491E35 region; boxless stand-in].
void make_rig(Rig &r) {
	// A joiner's world: its AiSystem runs non-authoritative, the client motor
	// path [orig: g_napi_np_ctx.is_authority == 0 on a client].
	r.world.ai.is_authority = false;
	r.world.registry.configure_pool(0, 8);
	r.world.registry.configure_pool(1, 8);
	w::Entity seed;
	seed.kind = w::EntityKind::Item;
	seed.item_id = 0x07DA; // Blackhawk wire type (items.def 102010)
	seed.position = {100.0f, 200.0f, 60.0f};
	seed.yaw = 90;
	r.heli = r.world.registry.spawn(1, seed);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.unit_type = 5;
	r.traits.acceleration = 512;
	r.traits.turn_rate = 0x600000;
	r.traits.climb_speed = 50 * 293;
}

// Prime a predicted row past its chase (no fresh record), engine on, both
// commands zero so the sideslip legs see only what the test seeds.
w::Entity *prime(Rig &r, bool engine_on) {
	w::Entity *heli = r.world.registry.get(r.heli);
	if (heli == nullptr) return nullptr;
	auto &m = heli->veh;
	m.net_predicted = true;
	m.net_interp_progress = 1;
	m.net_recv_speed = 0;
	m.net_recv_lat = 0;
	m.net_recv_steer_bam = 0;
	m.net_engine_on = engine_on;
	m.yaw_seeded = true;
	m.yaw_bam = 0;
	m.net_alt_target = w::to_fixed(60.0f);
	return heli;
}

// THE DAMP IS NOT SYMMETRIC. The airborne shift `(v + 8) >> 4` truncates
// toward zero on the positive side, and the `+ (v >> 31)` term adds -1 only
// for negatives — so a small POSITIVE rate never decays while its negative
// mirror reaches zero. A hull left alone settles from below and creeps from
// above; "fixing" it into a symmetric damp changes every idle aircraft's
// resting attitude [orig: @0x492213..0x492246].
bool run_pilot_planar_drag() {
	bool ok = true;
	for (bool terminal : { false, true }) {
		Rig r;
		make_rig(r);
		auto *heli = prime(r, true);
		w::Entity pilot;
		pilot.flags = terminal ? 0x100u : 0u;
		pilot.mounted = true;
		pilot.mount_target = r.heli;
		pilot.mount_type = w::SeatType::Controller;
		const auto pilot_h = r.world.registry.spawn(0, pilot);
		w::Seat seat;
		seat.type = w::SeatType::Controller;
		seat.occupant = pilot_h;
		heli->seats.push_back(seat);
		heli->primary_occupant = pilot_h;
		heli->veh.vel_x = 10240;
		heli->veh.vel_y = -10240;
		r.world.vehicles.aircraft_client_tick(*heli, r.traits);
		const int32_t once = (1019 * 10240 + 512) >> 10;
		const int32_t neg_once = (-1019 * 10240 + 512) >> 10;
		ok &= expect(heli->veh.vel_x == (terminal ? once : (1019 * once + 512) >> 10),
				"pilot Flags 0x100 gates second positive planar drag");
		ok &= expect(heli->veh.vel_y == (terminal ? neg_once : (1019 * neg_once + 512) >> 10),
				"pilot Flags 0x100 gates second negative planar drag");
	}
	return ok;
}

bool run_airborne_rate_damp_is_asymmetric() {
	bool ok = true;
	// Large rates: the negative side decays one step further.
	{
		Rig pos, neg;
		make_rig(pos);
		make_rig(neg);
		w::Entity *hp = prime(pos, true);
		w::Entity *hn = prime(neg, true);
		if (!expect(hp && hn, "damp rigs spawned")) return false;
		const int32_t r = 16000;
		// Seed ONLY the roll rate so the pitch legs (self-level on a zero
		// pitch, the sideslip pick on zero commands) contribute nothing.
		hp->veh.air_roll_rate = r;
		hn->veh.air_roll_rate = -r;
		pos.world.vehicles.aircraft_client_tick(*hp, pos.traits);
		neg.world.vehicles.aircraft_client_tick(*hn, neg.traits);
		// One tick applies: the airborne 1/16 damp, the engine-on path (no
		// extra damp), the 15 deg/tick clamp (inactive at 16000) and the
		// integration `roll += rate`. The rate after the tick is what the damp
		// left.
		const int32_t pos_step = r - hp->veh.air_roll_rate;
		const int32_t neg_step = hn->veh.air_roll_rate - (-r);
		ok &= expect(pos_step == ((r + 8) >> 4),
		             "positive rate damps by the truncating (v+8)>>4 step");
		ok &= expect(neg_step == pos_step + 1,
		             "the negative side decays one step further (the >>31 term)");
	}
	// Small rates: the damp leaves a positive +7 untouched while the negative
	// mirror reaches zero. Measured over the ticks before the SELF-LEVEL term
	// engages: the integration accumulates 7/tick of roll, and once
	// `(roll + 0x100) >> 9` reaches 1 (roll ~256, tick ~37) the self-level
	// pulls the rate down through a different leg [orig: @0x4921CD..0x492210]
	// — that is the witnessed system, not the damp's symmetry.
	{
		Rig stuck, settles;
		make_rig(stuck);
		make_rig(settles);
		w::Entity *hs = prime(stuck, true);
		w::Entity *hz = prime(settles, true);
		if (!expect(hs && hz, "small-rate rigs spawned")) return false;
		hs->veh.air_roll_rate = 7;
		hz->veh.air_roll_rate = -1;
		bool stuck_held = true;
		for (int t = 0; t < 30; ++t) {
			stuck.world.vehicles.aircraft_client_tick(*hs, stuck.traits);
			settles.world.vehicles.aircraft_client_tick(*hz, settles.traits);
			stuck_held &= hs->veh.air_roll_rate == 7;
		}
		ok &= expect(stuck_held,
		             "a small POSITIVE airborne rate never decays through the damp");
		ok &= expect(hz->veh.air_roll_rate == 0,
		             "a small negative airborne rate reaches zero");
		// And the consequence: the positive hull keeps rolling while the
		// negative one stopped.
		ok &= expect(hs->veh.air_roll_bam == 7 * 30,
		             "the stuck rate integrates every tick");
	}
	return ok;
}

// Grounded damps an EIGHTH, airborne a SIXTEENTH — a grounded hull settles
// twice as fast [orig: grounded @0x492323..0x492365]. A boxless row grounds
// when its Z sits within a unit of the cached ground sample.
bool run_grounded_damps_twice_as_hard() {
	Rig air, ground;
	make_rig(air);
	make_rig(ground);
	w::Entity *ha = prime(air, true);
	w::Entity *hg = prime(ground, true);
	if (!expect(ha && hg, "ground/air rigs spawned")) return false;
	// The grounded pick: ground_cache within 0x10000 of Z.
	hg->veh.ground_cache = w::to_fixed(60.0f) - 0x8000;
	const int32_t r = 16000;
	ha->veh.air_roll_rate = r;
	hg->veh.air_roll_rate = r;
	air.world.vehicles.aircraft_client_tick(*ha, air.traits);
	ground.world.vehicles.aircraft_client_tick(*hg, ground.traits);
	const int32_t air_step = r - ha->veh.air_roll_rate;
	const int32_t ground_step = r - hg->veh.air_roll_rate;
	bool ok = expect(ground_step == ((r + 4) >> 3),
	                 "grounded damps by the (v+4)>>3 step");
	ok &= expect(ground_step > air_step, "grounded damps harder than airborne");
	return ok;
}

// THE SIDESLIP PITCH FEEDBACK PICKS BETWEEN TWO GAIN PAIRS by asking whether
// the big step would move the pitch CLOSER TO ZERO: the strong pair
// (rate += d<<4, pitch += d<<5) when it levels, the weak pair (d<<2 / d<<2)
// otherwise. It is a levelling test, not a magnitude limit; one gain drops the
// nose-down authority a diving aircraft needs [orig: @0x491F50..0x491FA5].
bool run_sideslip_pitch_two_gain_pick() {
	// The measured along-track motion comes from vel_x at yaw 0; the command
	// is zero, so `along > |cmd|` gates the leg on and d = along - 0.
	auto run = [](int32_t pitch, int32_t &rate_delta, int32_t &angle_delta) {
		Rig r;
		make_rig(r);
		w::Entity *h = prime(r, true);
		if (h == nullptr) return false;
		// A sideslip large enough that the pick, not the damp, dominates the
		// observed deltas; the self-level and damp terms are subtracted below
		// from a control run with the same state but no motion.
		h->veh.air_pitch_bam = pitch;
		h->veh.vel_x = 0;
		Rig c;
		make_rig(c);
		w::Entity *hc = prime(c, true);
		if (hc == nullptr) return false;
		hc->veh.air_pitch_bam = pitch;
		hc->veh.vel_x = 0;
		// Motion only on the measured hull.
		h->veh.vel_x = 4096;
		r.world.vehicles.aircraft_client_tick(*h, r.traits);
		c.world.vehicles.aircraft_client_tick(*hc, c.traits);
		// The tilt->acceleration block adds the same pitch-driven terms to both
		// runs; the velocity-dependent weathervane acts on yaw only. The
		// feedback leg is the difference in the pitch RATE before the damp and
		// in the pitch angle before the integration; compare the post-tick
		// rates/angles of the two runs and recover the leg's contribution
		// through the shared damp (a linear map on the rate).
		rate_delta = h->veh.air_pitch_rate - hc->veh.air_pitch_rate;
		angle_delta = h->veh.air_pitch_bam - hc->veh.air_pitch_bam;
		return true;
	};
	bool ok = true;
	// Positive pitch: the strong step (+d<<5) pitches FURTHER -> weak pair.
	// Negative pitch: the same step LEVELS -> strong pair.
	int32_t weak_rate = 0, weak_angle = 0, strong_rate = 0, strong_angle = 0;
	// pitch = +/-10,000,000 BAM (~0.84 deg): with e = along = 4096, the strong
	// step e<<5 = 131072 pitches a positive hull FURTHER and levels a negative
	// one, so the two runs take opposite branches.
	ok &= expect(run(10000000, weak_rate, weak_angle), "weak-pair run");
	ok &= expect(run(-10000000, strong_rate, strong_angle), "strong-pair run");
	// The strong pair moves the pitch far more than the weak pair for the same
	// slip, and its rate and angle shifts differ (<<4 vs <<5) while the weak
	// pair's are equal (<<2 / <<2) — before the shared damp/integration folds.
	ok &= expect(std::abs(strong_angle) > std::abs(weak_angle),
	             "the levelling correction takes the STRONG pair");
	ok &= expect(std::abs(strong_angle) > 2 * std::abs(weak_angle),
	             "the two gain pairs are genuinely different (>= 8x in angle)");
	// Both corrections act against the measured slip: vel +X at yaw 0 is
	// forward motion, e = along - 0 > 0, and the leg adds +e<<k to the rate.
	ok &= expect(weak_rate > 0 && strong_rate > 0,
	             "the feedback opposes the slip in both branches");
	return ok;
}

// The global 15 deg/tick clamp binds AFTER the contact solve and before the
// integration [orig: @0x4925A8..0x492601, the negative bound 0xF5555560].
bool run_global_rate_clamp_binds() {
	Rig r;
	make_rig(r);
	w::Entity *h = prime(r, true);
	if (!expect(h != nullptr, "clamp rig spawned")) return false;
	h->veh.air_roll_rate = 999999999;
	const int32_t roll0 = h->veh.air_roll_bam;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	constexpr int32_t kAttitudeRateClamp = 178956960; // 0xAAAAAA0
	bool ok = expect(h->veh.air_roll_rate <= kAttitudeRateClamp,
	                 "the rate is clamped to 15 deg/tick");
	ok &= expect(opennova::io::bam_sub(h->veh.air_roll_bam, roll0) ==
	                     h->veh.air_roll_rate,
	             "the clamped rate is what integrates into the angle");
	return ok;
}

bool run_gear_clearance_servo() {
	Rig r;
	make_rig(r);
	w::Entity *h = prime(r, true);
	r.traits.climb_speed = 0; // hold altitude while testing the threshold
	h->veh.ground_cache = w::to_fixed(55.0f);
	h->veh.gear_phase = 0;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	bool ok = expect(h->veh.gear_phase == 1598, "five-unit clearance retracts gear");
	h->veh.gear_phase = 65500;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.gear_phase == 65535, "gear retract caps at unsigned max");
	h->veh.ground_cache = w::to_fixed(55.01f);
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.gear_phase == 65535 - 1598, "below five units extends gear");
	h->veh.gear_phase = 10;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.gear_phase == 0, "gear extension caps at zero");
	return ok;
}

w::Entity *add_pilot(Rig &r, w::Entity &h) {
	w::Entity seed;
	seed.kind = w::EntityKind::Organic;
	seed.health = 100;
	seed.player_class = 1;
	seed.mounted = true;
	seed.mount_target = h.handle;
	seed.mount_type = w::SeatType::Controller;
	const auto handle = r.world.registry.spawn(0, seed);
	w::Seat seat;
	seat.type = w::SeatType::Controller;
	seat.retail_slot = 8;
	seat.occupant = handle;
	h.seats.push_back(seat);
	h.primary_occupant = handle;
	h.health = 100;
	h.has_item_def = true;
	h.item_attrib = 0x40;
	r.traits.player_control = true;
	r.world.cached.local_player = handle;
	return r.world.registry.get(handle);
}

bool run_collective_and_view_boundaries() {
	Rig r;
	make_rig(r);
	w::Entity *h = prime(r, true);
	w::Entity *pilot = add_pilot(r, *h);
	r.traits.climb_speed = 0;
	r.traits.player_speed = 10000;
	r.world.ai.is_authority = true;
	h->veh.net_predicted = false;
	h->veh.part_spin.speed = w::kRotorSpeedMax;
	r.world.logic_tick = 1;
	h->bound_radius = 1;
	h->position.z = 60.5f; // near ground, above the grounded servo threshold
	h->veh.ground_cache = w::to_fixed(59.0f);
	h->veh.net_alt_target = w::to_fixed(80.0f);
	h->veh.net_climb = w::to_fixed(3.0f);
	pilot->analog_throttle = 64;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	bool ok = expect(h->veh.net_climb == 3 * 65536 - 8192,
			"near-ground analog collective changes retained climb");
	ok &= expect(h->veh.net_alt_target == 62 * 65536 - 8192,
			"near-ground altitude reconciles from climb, not old absolute target");
	pilot->net_move_input = 0x80;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.net_climb == 3 * 65536 + 8192,
			"keyboard collective takes precedence over fourth analog axis");
	// At precisely two radii the input writes absolute altitude first.
	h->position.z = 61;
	h->veh.net_alt_target = 70 * 65536;
	h->veh.net_climb = 65536;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.net_climb == 11 * 65536 + 16384,
			"two-radius equality takes the absolute-altitude input arm");
	// Zero climb parks commands even with cyclic keys held.
	h->position.z = 60.5f;
	h->veh.net_climb = 1;
	pilot->net_move_input = 0x40 | w::Entity::kMoveOrderMoving;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.net_climb == 0 && h->veh.net_alt_target == 59 * 65536 - 16384 &&
					h->veh.cmd_speed == 0 && h->veh.cmd_lateral_speed == 0,
			"zero climb parks target below ground and clears cyclic commands");
	const int index = r.world.ai.attach(pilot->handle);
	r.world.ai.at(index)->pitch = w::bam_from_degrees_wrapped(-60.0);
	h->veh.air_pitch_bam = 0;
	h->veh.view_tilt_bam = 0;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.view_tilt_bam == -3848520, "pilot view tilt uses doubled limit step");
	h->veh.view_tilt_bam = -298261599;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	ok &= expect(h->veh.view_tilt_bam == -298261600, "pilot view tilt lower clamp");
	return ok;
}

bool run_dead_hull_skips_live_servos() {
	Rig r;
	make_rig(r);
	w::Entity *h = prime(r, true);
	h->flags |= w::kEntityFlagDead;
	h->veh.air_pitch_rate = 12345;
	h->veh.slide_z = 2345;
	h->veh.gear_phase = 123;
	const auto position = h->position;
	r.world.vehicles.aircraft_client_tick(*h, r.traits);
	return expect(h->position.z == position.z && h->veh.air_pitch_rate == 12345 &&
					h->veh.slide_z == 2345 && h->veh.gear_phase == 123,
			"dead aircraft skips live motion, rate damping and gear");
}

bool run_flare_roles_and_seat_cadence() {
	Rig r;
	make_rig(r);
	w::Entity *h = prime(r, true);
	w::Entity *pilot = add_pilot(r, *h);
	r.world.ai.attach(h->handle);
	auto *body = r.world.ai.for_handle(h->handle);
	body->profile.type = 1;
	r.world.tables.ammo.entries.resize(3);
	for (int i = 1; i <= 2; ++i) {
		auto &ammo = r.world.tables.ammo.entries[i];
		ammo.valid = true;
		ammo.name = i == 1 ? "FLARE" : "GROUND_FLARE";
		ammo.velocity = 10;
		ammo.max_age_ticks = 62;
		ammo.flags = w::kAmmoFlagNoGravity;
	}
	r.traits.flare_points = { { { 65536, 0, 0 }, { 65536, 0, 0 } },
		{ { -65536, 0, 0 }, { -65536, 0, 65536 } } };
	r.world.vehicles.traits.set(h->item_id, r.traits);
	w::Entity passenger = *pilot;
	passenger.net_move_input = 0x20;
	const auto rider = r.world.registry.spawn(0, passenger);
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	seat.retail_slot = 0;
	seat.occupant = rider;
	h->seats.push_back(seat);
	h->veh.net_climb = 65536;
	r.world.logic_tick = 1;
	r.world.vehicles.tick_flare_input(*h);
	bool ok = expect(r.world.out.rounds.count == 2, "two authored flare points fire");
	const auto first = r.world.out.rounds.records[0];
	ok &= expect(first.origin_x == 101 * 65536 && first.origin_y == 200 * 65536 &&
					first.dir_pitch > 0 && first.adm_index == 1 && first.mode_flags == 1,
			"flare uses model point, zero-Z direction lift, and ammo-indexed fire");
	r.world.vehicles.tick_flare_input(*h);
	ok &= expect(r.world.out.rounds.count == 2, "held passenger flare input is latched");
	r.world.logic_tick = 64;
	r.world.vehicles.tick_flare_input(*h);
	ok &= expect(r.world.out.rounds.count == 4, "64-phase cadence rearms held flare input");
	r.world.rules.mp_session = true;
	r.world.rules.logic_authority = false;
	r.world.vehicles.release_flares(*h);
	ok &= expect(r.world.out.source_fires.size() == 2 && r.world.out.rounds.count == 4,
			"local pilot queues flare uplink without authority ring duplication");
	r.world.cached.local_player = {};
	r.world.vehicles.release_flares(*h);
	ok &= expect(
			r.world.out.source_fires.size() == 2, "remote client does not originate flare rounds");
	r.world.rules.mp_session = false;
	body->profile.type = 2;
	r.traits.flare_points.clear();
	r.world.vehicles.traits.set(h->item_id, r.traits);
	r.world.vehicles.release_flares(*h);
	ok &= expect(r.world.out.rounds.count == 5 && r.world.out.rounds.records[4].adm_index == 2 &&
					r.world.out.rounds.records[4].origin_x == 100 * 65536,
			"ground profile selects GROUND_FLARE with origin fallback");
	return ok;
}

} // namespace

int main() {
	bool ok = run_collective_and_view_boundaries();
	ok &= run_dead_hull_skips_live_servos();
	ok &= run_flare_roles_and_seat_cadence();
	ok &= run_gear_clearance_servo();
	ok &= run_pilot_planar_drag();
	ok &= run_airborne_rate_damp_is_asymmetric();
	ok &= run_grounded_damps_twice_as_hard();
	ok &= run_sideslip_pitch_two_gain_pick();
	ok &= run_global_rate_clamp_binds();
	if (!ok) {
		std::fprintf(stderr, "aircraft_client_motor_test FAILED\n");
		return 1;
	}
	std::printf("aircraft_client_motor_test OK\n");
	return 0;
}
