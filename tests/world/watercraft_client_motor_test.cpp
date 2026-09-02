// The joiner-side watercraft prediction (net-re §5.38e B-facet, D-NET-196):
// chase + register-mirror + thrust/drag/keel prediction between subrate wire
// records [orig: Entity_UpdateWatercraftPhysics @0x48D480, client-executed
// subset]. The payoff contract: a remote boat at 15.6 m/s — ABOVE the
// chase-only sustain limit (~12.5 m/s), where the chase alone snap-cycles —
// glides with bounded per-tick steps because the mirrored speed/steer
// registers drive local physics between records. Plus the stale-record
// coast-down (an abandoned boat predicts to a stop).

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/angle.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <base/io/bam.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

struct Rig {
	w::World world;
	w::EntityHandle boat;
	w::VehicleTraits traits;
};

void set_zodiac_boxes(w::VehicleTraits &traits) {
	traits.box_y_lo = -(1 << 16);
	traits.box_y_hi = 1 << 16;
	traits.box_x_lo = -(5 << 15);
	traits.box_x_hi = 5 << 15;
	traits.box_z_lo = -(1 << 15);
	traits.box_z_hi = 1 << 16;
	traits.foot_x_lo = traits.box_x_lo;
	traits.foot_x_hi = traits.box_x_hi;
	traits.foot_y_lo = traits.box_y_lo;
	traits.foot_y_hi = traits.box_y_hi;
}

struct RampField {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	opennova::terrain::TerrainHeightField field;

	explicit RampField(bool ramp)
			: heightmap(kDim * kDim), sector_grid(256, 1) {
		for (int y = 0; y < kDim; ++y) {
			for (int x = 0; x < kDim; ++x) {
				heightmap[y * kDim + x] = static_cast<uint16_t>(
						ramp ? x * 16 : 2 * 256);
			}
		}
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
	}
};

void make_rig(Rig &r) {
	r.world.registry.configure_pool(0, 8);
	r.world.registry.configure_pool(1, 8);
	r.world.env.water_z = 10 << 16; // afloat everywhere (no terrain field)
	w::Entity seed;
	seed.kind = w::EntityKind::Item;
	seed.item_id = 0x050D; // Drivable Zodiac wire type
	seed.position = {100.0f, 200.0f, 10.0f};
	seed.yaw = 90; // mission 90 deg = engine heading BAM 0 = +X forward
	r.boat = r.world.registry.spawn(1, seed);
	r.traits.physics = 1;
	r.traits.family = w::VehicleFamily::Watercraft;
	r.traits.acceleration = 512;
	r.traits.turn_rate = 0x600000;
	r.traits.turn_rate2 = 0x180000;
	r.traits.water_speed = 20972; // ~20 m/s max drive speed, 16.16 u/tick
}

w::Entity *mount_prediction_driver(Rig &r, bool is_local) {
	w::Entity *vehicle = r.world.registry.get(r.boat);
	if (!expect(vehicle != nullptr, "prediction vehicle spawned")) return nullptr;
	w::Seat seat;
	seat.type = w::SeatType::Driver;
	seat.bone_index = 7;
	seat.source_name = "drvrx00";
	vehicle->seats.push_back(seat);

	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.item_id = 5305;
	body.player_class = 8;
	body.health = 150;
	body.health_max = 150;
	body.alive = true;
	const w::EntityHandle handle = r.world.registry.spawn(0, body);
	if (is_local) r.world.cached.local_player = handle;
	if (!expect(w::entity_process_vehicle_attach(r.world, handle, r.boat, 7),
	            "prediction driver mounted")) return nullptr;
	return r.world.registry.get(handle);
}

// Mimic the sim's staging edge (mirror_client_replica_mission_entities): a fresh
// wire record for the boat at world (16.16) x/y/z with the received registers.
void stage(w::Entity &boat, int32_t x, int32_t y, int32_t z,
           int32_t heading_bam, int32_t speed_reg, int32_t steer_bam) {
	auto &m = boat.veh;
	m.net_smooth_target[0] = x;
	m.net_smooth_target[1] = y;
	m.net_smooth_target[2] = z;
	m.net_smooth_heading = heading_bam;
	m.net_recv_speed = speed_reg;
	m.net_recv_steer_bam = steer_bam;
	m.net_interp_progress = 0;
	m.net_predicted = true;
}

bool run_fast_boat_glides() {
	Rig r;
	make_rig(r);
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "boat spawned")) return false;

	// True motion: +X at 16384/tick (15.6 m/s), heading east (engine BAM 0),
	// records every 8 ticks like a busy retail host.
	const int32_t step_fx = 16384;
	const int32_t x0 = w::to_fixed(100.0f);
	const int32_t y0 = w::to_fixed(200.0f);
	const int32_t z0 = w::to_fixed(10.0f);
	const int kWarm = 64, kMeasure = 96, kGap = 8;

	std::vector<int32_t> presented;
	for (int t = 0; t < kWarm + kMeasure; ++t) {
		if (t % kGap == 0) {
			stage(*boat, x0 + step_fx * t, y0, z0, /*heading*/ 0,
			      /*speed*/ step_fx, /*steer*/ 0);
		}
		w::watercraft_client_tick(r.world, *boat, r.traits);
		presented.push_back(w::to_fixed(boat->position.x));
	}

	int32_t max_step = 0;
	int stalled = 0;
	for (int t = kWarm; t < kWarm + kMeasure; ++t) {
		const int32_t d = std::abs(presented[t] - presented[t - 1]);
		if (d > max_step) max_step = d;
		if (d < step_fx / 4) ++stalled;
	}
	std::fprintf(stderr,
	             "[boat-fast] true step=%d  max step=%d  stalled=%d/%d\n",
	             step_fx, max_step, stalled, kMeasure);
	bool ok = true;
	ok &= expect(max_step <= 2 * step_fx + 1024,
	             "a 15.6 m/s boat glides (prediction carries it past the chase limit)");
	ok &= expect(stalled * 4 <= kMeasure,
	             "the predicted boat keeps moving between records");
	// Tracking: the predicted pose stays within the fast snap radius of truth.
	const int32_t true_x = x0 + step_fx * (kWarm + kMeasure - 1);
	ok &= expect(std::abs(true_x - presented.back()) <= 0x60000,
	             "prediction+chase lag stays under the fast snap radius");
	return ok;
}

bool run_abandoned_boat_coasts_to_rest() {
	Rig r;
	make_rig(r);
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "boat spawned")) return false;
	// One record at speed, then silence: after the 128-tick starvation edge the
	// mirrored command decays and the hull drags to rest
	// [orig: @0x48DDC0..0x48DDCE + the 1/64 shed].
	stage(*boat, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(10.0f),
	      0, 16384, 0);
	float prev_x = boat->position.x;
	float last_step = 0.0f;
	for (int t = 0; t < 1200; ++t) {
		w::watercraft_client_tick(r.world, *boat, r.traits);
		last_step = boat->position.x - prev_x;
		prev_x = boat->position.x;
	}
	std::fprintf(stderr, "[boat-coast] final per-tick step=%f m  recv_speed=%d\n",
	             double(last_step), boat->veh.net_recv_speed);
	bool ok = true;
	// The witnessed decay floors at 63 ((v+64)>>7 == 0 below 64), leaving a
	// bounded residual creep — faithful; under live records the age-forced
	// re-admits keep the chase pinning the hull to its true resting pose.
	ok &= expect(boat->veh.net_recv_speed <= 63,
	             "the mirrored command decays to the witnessed (v+64)>>7 floor");
	ok &= expect(std::abs(last_step) < 0.05f,
	             "the residual creep stays bounded");
	return ok;
}

bool run_steer_follows_received_register() {
	Rig r;
	make_rig(r);
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "boat spawned")) return false;
	// Received steer says +45 deg while moving: the rudder integrator must turn
	// the hull toward it (sign witness for the steer chain).
	const int32_t steer_target = 0x20000000; // +45 deg BAM
	stage(*boat, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(10.0f),
	      0, 16384, steer_target);
	for (int t = 0; t < 240; ++t) {
		if (t % 8 == 0) {
			// keep records coming so the command holds; target tracks a gentle
			// arc (position feed is secondary here — the heading is the check)
			stage(*boat, w::to_fixed(boat->position.x + 0.25f),
			      w::to_fixed(boat->position.y), w::to_fixed(10.0f), 0, 16384,
			      steer_target);
		}
		w::watercraft_client_tick(r.world, *boat, r.traits);
	}
	const int32_t heading = boat->veh.yaw_bam;
	std::fprintf(stderr, "[boat-steer] heading after 240 ticks = 0x%08x\n",
	             static_cast<uint32_t>(heading));
	return expect(heading > 0x04000000 && heading < 0x30000000,
	              "the hull turns toward the received steer register");
}

bool run_ground_vehicle_glides() {
	// The GROUND prediction leg: the shared chase + mirrored registers driving
	// tick_vehicle_motor's core (input block bypassed). A 15.6 m/s buggy —
	// beyond the chase-only sustain — must glide like the boat does.
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Ground;
	r.traits.player_speed = 20972; // ~20 m/s ground max
	r.traits.acceleration = 512;   // per-tick speed clamp: cruise in ~32 ticks
	r.traits.deceleration = 512;   // (the predicted twin ramps like retail's)
	r.world.env.water_z = 0; // dry land
	w::Entity *veh = r.world.registry.get(r.boat);
	if (!expect(veh != nullptr, "vehicle spawned")) return false;

	const int32_t step_fx = 16384;
	const int32_t x0 = w::to_fixed(100.0f);
	const int32_t y0 = w::to_fixed(200.0f);
	const int32_t z0 = w::to_fixed(10.0f);
	const int kWarm = 96, kMeasure = 96, kGap = 8;
	std::vector<int32_t> presented;
	for (int t = 0; t < kWarm + kMeasure; ++t) {
		if (t % kGap == 0) {
			stage(*veh, x0 + step_fx * t, y0, z0, 0, step_fx, 0);
		}
		w::ground_client_tick(r.world, *veh, r.traits);
		presented.push_back(w::to_fixed(veh->position.x));
	}
	int32_t max_step = 0;
	int stalled = 0;
	for (int t = kWarm; t < kWarm + kMeasure; ++t) {
		const int32_t d = std::abs(presented[t] - presented[t - 1]);
		if (d > max_step) max_step = d;
		if (d < step_fx / 4) ++stalled;
	}
	std::fprintf(stderr, "[ground-fast] true step=%d  max step=%d  stalled=%d/%d\n",
	             step_fx, max_step, stalled, kMeasure);
	bool ok = true;
	ok &= expect(max_step <= 2 * step_fx + 1024,
	             "a 15.6 m/s ground vehicle glides on the prediction leg");
	ok &= expect(stalled * 4 <= kMeasure,
	             "the predicted ground vehicle keeps moving between records");
	return ok;
}

bool run_bike_family_deltas() {
	// The cbik promotion (D-NET-196; cbik grill 2026-07-31): bikes ride the
	// ground core with four witnessed family deltas. Pin the two observable
	// off-contact ones — gravity 250/tick (ground: 324) and yaw STILL applied
	// while airborne (the ground core freezes it) [orig: @0x4865a6 vs
	// @0x48d009; yaw @0x486681..0x486697 vs the grounded gate @0x48d0d4].
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Bike;
	r.traits.player_speed = 20972;
	r.traits.acceleration = 512;
	r.traits.deceleration = 512;
	r.world.env.water_z = 0;
	w::Entity *veh = r.world.registry.get(r.boat);
	if (!expect(veh != nullptr, "bike spawned")) return false;

	// Flat terrain at ground 0; the bike flies at z=10 (beyond the 0.5 u
	// suspension margin), so the vertical pipeline runs off-contact.
	std::vector<uint16_t> heightmap(64 * 64, 0);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;

	// Steer hard right while flying: the live steer chain (servo -> wheel rate)
	// must keep turning the airborne bike; speed is held by the bike's own
	// contact-gated integration (off-contact, nothing changes it).
	const int32_t x0 = w::to_fixed(100.0f);
	const int32_t y0 = w::to_fixed(200.0f);
	const int32_t z0 = w::to_fixed(10.0f);
	const int32_t steer = 0x20000000; // +45 deg
	stage(*veh, x0, y0, z0, 0, 8192, steer);
	w::ground_client_tick(r.world, *veh, r.traits); // arm; airborne over the flat
	auto &m = veh->veh;
	if (!expect(!m.grounded, "the flying bike is off-contact")) return false;
	m.speed = 8192; // live speed the steer chain multiplies
	// Vertical: seed above the up-cap; one tick must clamp to 0x4000 then
	// subtract exactly the bike gravity 250 (never the ground 324).
	m.slide_z = 0x5000;
	const int32_t yaw_before = m.yaw_bam;
	w::ground_client_tick(r.world, *veh, r.traits);
	bool ok = expect(m.slide_z == 0x4000 - 250,
	                 "bike vertical = up-cap 0x4000 then gravity 250");
	const int32_t air_yaw_step = opennova::io::bam_sub(m.yaw_bam, yaw_before);
	ok &= expect(m.wheel_rate_bam != 0,
	             "the airborne bike produces a non-zero wheel yaw rate");
	ok &= expect(air_yaw_step == opennova::io::bam_sar(m.wheel_rate_bam, 2),
	             "the off-contact bike applies exactly one quarter yaw rate");
	for (int t = 0; t < 8; ++t) w::ground_client_tick(r.world, *veh, r.traits);
	std::fprintf(stderr, "[bike-air] slide_z pin ok=%d yaw %d -> %d\n", int(ok),
	             yaw_before, m.yaw_bam);
	ok &= expect(m.yaw_bam != yaw_before,
	             "the airborne bike still applies its yaw rate");

	// The ground family on the same airborne rig keeps 324, no cap, frozen yaw.
	Rig g;
	make_rig(g);
	g.traits.family = w::VehicleFamily::Ground;
	g.traits.player_speed = 20972;
	g.traits.acceleration = 512;
	g.traits.deceleration = 512;
	g.world.env.water_z = 0;
	g.world.terrain = &field;
	w::Entity *gveh = g.world.registry.get(g.boat);
	stage(*gveh, x0, y0, z0, 0, 8192, steer);
	w::ground_client_tick(g.world, *gveh, g.traits);
	if (!expect(!gveh->veh.grounded, "the flying buggy is off-contact")) return false;
	gveh->veh.speed = 8192;
	gveh->veh.slide_z = 0x5000;
	const int32_t gyaw = gveh->veh.yaw_bam;
	w::ground_client_tick(g.world, *gveh, g.traits);
	ok &= expect(gveh->veh.slide_z == 0x5000 - 324,
	             "ground vertical keeps 324 and no up-cap");
	for (int t = 0; t < 8; ++t) w::ground_client_tick(g.world, *gveh, g.traits);
	ok &= expect(gveh->veh.yaw_bam == gyaw,
	             "the airborne ground vehicle freezes its yaw");
	return ok;
}

bool run_ground_parked_rests_at_wheel_clearance() {
	// The ground/tracked contact solve (D-NET-196 B-facet): a parked
	// Ground-family row with resolved model boxes rests with its ORIGIN at
	// wheel height above terrain — the pad probes sit at box_z_lo + r, so the
	// solved Z holds ground - box_z_lo — instead of being gravity-dragged onto
	// the terrain by a zero-clearance clamp (the live joiner symptom: Strykers
	// sunk by exactly 1.10 with their spawn-posed addeweap guns floating above)
	// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 — pads @0x47C7DC..,
	// solver Z @0x46C822..0x46C894, Z select @0x47ECBB].
	std::vector<uint16_t> heightmap(64 * 64, 42 * 256); // flat terrain z = 42.0
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;

	// Stryker-like hull (16.16): wheel bottom 1.10 below the origin, beam 3.0
	// (pad radius r = 0.75), length 7.0.
	const int32_t clearance = 72090; // 1.10 u — the witnessed sunk amount
	const int32_t ground_fx = 42 << 16;
	const int32_t rest_fx = ground_fx + clearance;
	// (A zero-motion SUNKEN start is deliberately absent: it meets the
	// witnessed sleep fast-path gate — velocities zero, slide in (-350,-1],
	// planar pose unchanged, no occupant [orig: @0x47C244..0x47C349] — and
	// retail sleeps it too; retail simply can never reach that state because
	// the solve holds the hull from its first tick.)
	struct Probe { int32_t start_z; const char *label; };
	const Probe probes[] = {
		{rest_fx, "holds the wire rest height"},
		{rest_fx + 98304, "lands from a 1.5 u drop and rests at clearance"},
	};
	bool ok = true;
	for (const Probe &pr : probes) {
		Rig r;
		make_rig(r);
		r.world.env.water_z = 0; // dry land
		r.world.terrain = &field;
		r.traits.family = w::VehicleFamily::Ground;
		r.traits.player_speed = 20972;
		r.traits.acceleration = 512;
		r.traits.deceleration = 512;
		r.traits.torque = 7;
		r.traits.mass = 28;
		r.traits.max_slope = 30 * 11930464;  // deg tokens -> BAM like the parser
		r.traits.slip_slope = 45 * 11930464;
		r.traits.box_z_lo = -clearance;
		r.traits.box_z_hi = 92 << 10; // +1.44 deck
		r.traits.box_y_lo = -(3 << 15); // beam 3.0 -> r = 0.75
		r.traits.box_y_hi = 3 << 15;
		r.traits.box_x_lo = -(7 << 15); // length 7.0
		r.traits.box_x_hi = 7 << 15;
		r.traits.foot_x_lo = r.traits.box_x_lo;
		r.traits.foot_x_hi = r.traits.box_x_hi;
		r.traits.foot_y_lo = r.traits.box_y_lo;
		r.traits.foot_y_hi = r.traits.box_y_hi;
		w::Entity *veh = r.world.registry.get(r.boat);
		if (!expect(veh != nullptr, "parked ground vehicle spawned")) return false;
		veh->position.z = float(w::from_fixed(pr.start_z));
		if (pr.start_z != rest_fx) {
			// The retail-reachable mid-drop state: a hull that lost contact
			// carries the airborne flag + a clear contact byte from its last
			// solve. (A ZERO-MOTION midair hull instead meets the sleep gate
			// and hovers — the witnessed retail floating-placement artifact.)
			veh->flags |= w::kEntityFlagInAir;
			veh->veh.grounded = false;
		}
		// The retail host's parked record: origin at terrain + clearance, no
		// motion, re-sent every 8 ticks like a live 01TR join.
		for (int t = 0; t < 120; ++t) {
			if (t % 8 == 0)
				stage(*veh, w::to_fixed(100.0f), w::to_fixed(200.0f), rest_fx,
				      0, 0, 0);
			w::ground_client_tick(r.world, *veh, r.traits);
		}
		const int32_t z = w::to_fixed(veh->position.z);
		std::fprintf(stderr,
		             "[ground-rest %s] z=%d rest=%d ground=%d grounded=%d air=%d\n",
		             pr.label, z, rest_fx, ground_fx, int(veh->veh.grounded),
		             int((veh->flags & w::kEntityFlagInAir) != 0));
		// Within 1 cm of the wheel-clearance rest height — NOT clamped onto the
		// terrain (the pre-solve stand-in parked the origin at ground_fx).
		ok &= expect(std::abs(z - rest_fx) <= 0x290, pr.label);
		ok &= expect(veh->veh.grounded, "the parked hull reports wheel contact");
		ok &= expect((veh->flags & w::kEntityFlagInAir) == 0,
		             "the parked hull is not airborne");
		// Flat terrain: the attitude conform stays level.
		ok &= expect(std::abs(veh->veh.air_pitch_bam) < (1 << 22) &&
		                     std::abs(veh->veh.air_roll_bam) < (1 << 22),
		             "the flat-parked hull conforms level");
	}
	return ok;
}

bool run_tank_parked_rests_at_wheel_clearance() {
	// The wheeled (ctan) contact solve (D-NET-196 B-facet): a parked
	// Tank-family row rests with its ORIGIN at wheel height above terrain —
	// the same ground - box_z_lo invariant as the tracked solve, through the
	// 13-probe geometry and the slideDecay-absorbing Z select
	// [orig: Entity_ProcessWheeledVehiclePhysics @0x475DE0 — pads
	// @0x4762B8.., the positive-corner solver Z via
	// Entity_ComputeSuspensionAndOrientation @0x4698A0, the absorb select
	// @0x478BE3..0x478C06].
	std::vector<uint16_t> heightmap(64 * 64, 42 * 256); // flat terrain z = 42.0
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;

	// M1A1-like hull (16.16): track bottom 0.95 below the origin, beam 3.6
	// (pad radius r = 0.9), length 7.9.
	const int32_t clearance = 62259; // 0.95 u
	const int32_t ground_fx = 42 << 16;
	const int32_t rest_fx = ground_fx + clearance;
	struct Probe { int32_t start_z; const char *label; };
	const Probe probes[] = {
		{rest_fx, "tank holds the wire rest height"},
		{rest_fx + 98304, "tank lands from a 1.5 u drop and rests at clearance"},
	};
	bool ok = true;
	for (const Probe &pr : probes) {
		Rig r;
		make_rig(r);
		r.world.env.water_z = 0; // dry land
		r.world.terrain = &field;
		r.traits.family = w::VehicleFamily::Tank;
		r.traits.player_speed = 15000;
		r.traits.acceleration = 512;
		r.traits.deceleration = 512;
		r.traits.torque = 7;
		r.traits.mass = 60;
		r.traits.max_slope = 30 * 11930464;
		r.traits.slip_slope = 45 * 11930464;
		r.traits.box_z_lo = -clearance;
		r.traits.box_z_hi = 96 << 10; // +1.5 turret deck
		r.traits.box_y_lo = -(18 << 14); // beam 3.6 -> r = 0.9
		r.traits.box_y_hi = 18 << 14;
		r.traits.box_x_lo = -(79 << 12); // length ~7.9
		r.traits.box_x_hi = 79 << 12;
		r.traits.foot_x_lo = r.traits.box_x_lo;
		r.traits.foot_x_hi = r.traits.box_x_hi;
		r.traits.foot_y_lo = r.traits.box_y_lo;
		r.traits.foot_y_hi = r.traits.box_y_hi;
		w::Entity *veh = r.world.registry.get(r.boat);
		if (!expect(veh != nullptr, "parked tank spawned")) return false;
		veh->position.z = float(w::from_fixed(pr.start_z));
		if (pr.start_z != rest_fx) {
			veh->flags |= w::kEntityFlagInAir;
			veh->veh.grounded = false;
		}
		for (int t = 0; t < 120; ++t) {
			if (t % 8 == 0)
				stage(*veh, w::to_fixed(100.0f), w::to_fixed(200.0f), rest_fx,
				      0, 0, 0);
			w::ground_client_tick(r.world, *veh, r.traits);
		}
		const int32_t z = w::to_fixed(veh->position.z);
		std::fprintf(stderr,
		             "[tank-rest %s] z=%d rest=%d ground=%d grounded=%d air=%d\n",
		             pr.label, z, rest_fx, ground_fx, int(veh->veh.grounded),
		             int((veh->flags & w::kEntityFlagInAir) != 0));
		ok &= expect(std::abs(z - rest_fx) <= 0x290, pr.label);
		ok &= expect(veh->veh.grounded, "the parked tank reports wheel contact");
		ok &= expect((veh->flags & w::kEntityFlagInAir) == 0,
		             "the parked tank is not airborne");
		ok &= expect(std::abs(veh->veh.air_pitch_bam) < (1 << 22) &&
		                     std::abs(veh->veh.air_roll_bam) < (1 << 22),
		             "the flat-parked tank conforms level");
	}
	return ok;
}

bool run_bike_parked_rests_at_wheel_clearance() {
	// The light (cbik) contact solve (D-NET-196 B-facet): the bike's r is
	// height-derived — ((box_z_hi - box_z_lo) >> 1) - 0x4000 — and the Z
	// select is the mean of the two lifted wheel corners of the axle fit, so
	// the same rest invariant holds: origin at ground - box_z_lo
	// [orig: Entity_ProcessLightVehiclePhysics @0x479600 — wheels on the
	// foot centerline @0x479960.., the axle-fit Z (c0.z + c1.z) * 0.5 in
	// Entity_UpdateVehicleChassisOrientation @0x468A50].
	std::vector<uint16_t> heightmap(64 * 64, 42 * 256);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;

	// dcycle2-like frame (16.16): wheel bottom 0.55 below the origin, box
	// height 1.2 (r = 0.6 - 0.25 = 0.35), length 2.2, beam 0.8.
	const int32_t clearance = 36044; // 0.55 u
	const int32_t ground_fx = 42 << 16;
	const int32_t rest_fx = ground_fx + clearance;
	struct Probe { int32_t start_z; const char *label; };
	const Probe probes[] = {
		{rest_fx, "bike holds the wire rest height"},
		{rest_fx + 65536, "bike lands from a 1 u drop and rests at clearance"},
	};
	bool ok = true;
	for (const Probe &pr : probes) {
		Rig r;
		make_rig(r);
		r.world.env.water_z = 0;
		r.world.terrain = &field;
		r.traits.family = w::VehicleFamily::Bike;
		r.traits.player_speed = 20972;
		r.traits.acceleration = 512;
		r.traits.deceleration = 512;
		r.traits.torque = 7;
		r.traits.mass = 3;
		r.traits.max_slope = 30 * 11930464;
		r.traits.slip_slope = 45 * 11930464;
		r.traits.box_z_lo = -clearance;
		r.traits.box_z_hi = -clearance + (12 << 13); // height 1.2 -> r = 0.35
		r.traits.box_y_lo = -(4 << 13); // beam 0.8
		r.traits.box_y_hi = 4 << 13;
		r.traits.box_x_lo = -(11 << 13); // length 2.2
		r.traits.box_x_hi = 11 << 13;
		r.traits.foot_x_lo = r.traits.box_x_lo;
		r.traits.foot_x_hi = r.traits.box_x_hi;
		r.traits.foot_y_lo = r.traits.box_y_lo;
		r.traits.foot_y_hi = r.traits.box_y_hi;
		w::Entity *veh = r.world.registry.get(r.boat);
		if (!expect(veh != nullptr, "parked bike spawned")) return false;
		veh->position.z = float(w::from_fixed(pr.start_z));
		if (pr.start_z != rest_fx) {
			veh->flags |= w::kEntityFlagInAir;
			veh->veh.grounded = false;
		}
		for (int t = 0; t < 120; ++t) {
			if (t % 8 == 0)
				stage(*veh, w::to_fixed(100.0f), w::to_fixed(200.0f), rest_fx,
				      0, 0, 0);
			w::ground_client_tick(r.world, *veh, r.traits);
		}
		const int32_t z = w::to_fixed(veh->position.z);
		std::fprintf(stderr,
		             "[bike-rest %s] z=%d rest=%d grounded=%d air=%d ticks=%d\n",
		             pr.label, z, rest_fx, int(veh->veh.grounded),
		             int((veh->flags & w::kEntityFlagInAir) != 0),
		             veh->veh.light_rear_contact_ticks);
		ok &= expect(std::abs(z - rest_fx) <= 0x290, pr.label);
		ok &= expect(veh->veh.grounded,
		             "the parked bike reports contact after the rear-tick run");
		ok &= expect((veh->flags & w::kEntityFlagInAir) == 0,
		             "the parked bike is not airborne");
		ok &= expect(std::abs(veh->veh.air_pitch_bam) < (1 << 22) &&
		                     std::abs(veh->veh.air_roll_bam) < (1 << 22),
		             "the flat-parked bike conforms level");
	}
	return ok;
}

bool run_tank_family_deltas() {
	// The ctan promotion (D-NET-196): tanks ride the ground core with the
	// witnessed family deltas. Pin the observable off-contact trio — gravity
	// 250/tick with NO up-cap (bike caps at 0x4000; ground uses 324), the
	// airborne quarter-rate yaw that stays APPLIED (the ground core freezes
	// it), and the contact-gated speed integration (an airborne tank's speed
	// never chases the command) [orig: Entity_UpdateTankVehiclePhysics —
	// gravity @0x48a82c, yaw @0x48a9f7..0x48aa1d, integrate gate
	// @0x489f34..0x489f56].
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Tank;
	r.traits.player_speed = 15000;
	r.traits.acceleration = 512;
	r.traits.deceleration = 512;
	r.world.env.water_z = 0;
	w::Entity *veh = r.world.registry.get(r.boat);
	if (!expect(veh != nullptr, "tank spawned")) return false;

	std::vector<uint16_t> heightmap(64 * 64, 0);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;

	const int32_t x0 = w::to_fixed(100.0f);
	const int32_t y0 = w::to_fixed(200.0f);
	const int32_t z0 = w::to_fixed(10.0f);
	const int32_t steer = 0x20000000; // +45 deg
	stage(*veh, x0, y0, z0, 0, 8192, steer);
	w::ground_client_tick(r.world, *veh, r.traits); // arm; airborne over the flat
	auto &m = veh->veh;
	if (!expect(!m.grounded, "the flying tank is off-contact")) return false;
	m.speed = 8192;
	const int32_t speed_before = 8192;
	m.slide_z = 0x5000; // above the bike cap: the tank must NOT clamp it
	const int32_t yaw_before = m.yaw_bam;
	w::ground_client_tick(r.world, *veh, r.traits);
	bool ok = expect(m.slide_z == 0x5000 - 250,
	                 "tank vertical = gravity 250 with no up-cap");
	const int32_t air_yaw_step = opennova::io::bam_sub(m.yaw_bam, yaw_before);
	ok &= expect(m.wheel_rate_bam != 0,
	             "the airborne tank produces a non-zero yaw rate");
	ok &= expect(air_yaw_step == opennova::io::bam_sar(m.wheel_rate_bam, 2),
	             "the off-contact tank applies exactly one quarter yaw rate");
	ok &= expect(m.speed == speed_before,
	             "the airborne tank never integrates its speed command");
	std::fprintf(stderr, "[tank-air] slide_z=%d yaw %d -> %d speed=%d\n",
	             m.slide_z, yaw_before, m.yaw_bam, m.speed);
	return ok;
}

bool run_platform_solve_settles_at_waterline() {
	// The platform solve (the D-NET-196 boat water leg): with model boxes
	// resolved, a stationary boat converges onto its waterline from above AND
	// below, level, instead of holding a chase-frozen Z
	// [orig: Entity_ProcessPlatformPhysics @0x481870 §8/§10 — the light-boat
	// float height 0.65q and the corner-average Z].
	struct Probe { double start_z; const char *label; };
	const Probe probes[] = {{12.0, "from above"}, {8.0, "from below"}};
	bool ok = true;
	for (const Probe &pr : probes) {
		Rig r;
		make_rig(r);
		// Zodiac-like hull: beam 2.0, length 5.0, keel/deck -0.5..1.0 (16.16).
		set_zodiac_boxes(r.traits);
		w::Entity *boat = r.world.registry.get(r.boat);
		boat->position.z = float(pr.start_z);
		stage(*boat, w::to_fixed(100.0f), w::to_fixed(200.0f),
		      w::to_fixed(float(pr.start_z)), 0, 0, 0);
		for (int t = 0; t < 900; ++t)
			w::watercraft_client_tick(r.world, *boat, r.traits);
		const double z = double(boat->position.z);
		const double pitch_deg =
			double(boat->veh.air_pitch_bam) * (360.0 / 4294967296.0);
		std::fprintf(stderr, "[platform %s] final z=%.3f pitch=%.2f afloat=%d\n",
		             pr.label, z, pitch_deg, int(boat->veh.plat_afloat));
		// The afloat-FLAG latch at equilibrium rides the modelData box-pair
		// provenance (the spec's own tracked unknown — the v210/draft geometry
		// vs our collision-AABB box source). The SETTLE and LEVEL contracts
		// are the pinned behavior; the flag question is in the residual notes.
		ok &= expect(z > 8.5 && z < 11.5, "the hull settles into the waterline band");
		ok &= expect(std::abs(pitch_deg) < 15.0, "the settled hull sits near level");
	}
	return ok;
}

void prime_prediction_tick(w::Entity &boat, int32_t cmd_speed,
		int32_t vel_x = 0) {
	auto &m = boat.veh;
	m.net_predicted = true;
	m.net_interp_progress = 1; // no fresh-record chase on the measured tick
	m.net_recv_speed = cmd_speed;
	m.net_recv_steer_bam = 0;
	m.yaw_seeded = true;
	m.yaw_bam = 0;
	m.vel_x = vel_x;
	m.vel_y = 0;
}

bool run_afloat_latch_controls_drag() {
	// Contact drag consumes the PREVIOUS platform solve's afloat latch. Two
	// otherwise identical hulls therefore differ by exactly the witnessed 1/8
	// landed shed; model-less worlds retain an explicit water-plane fallback.
	Rig wet, landed, fallback;
	make_rig(wet);
	make_rig(landed);
	make_rig(fallback);
	set_zodiac_boxes(wet.traits);
	set_zodiac_boxes(landed.traits);
	w::Entity *wet_boat = wet.world.registry.get(wet.boat);
	w::Entity *landed_boat = landed.world.registry.get(landed.boat);
	w::Entity *fallback_boat = fallback.world.registry.get(fallback.boat);
	if (!expect(wet_boat && landed_boat && fallback_boat,
	            "afloat-drag boats spawned")) return false;
	prime_prediction_tick(*wet_boat, 0, 6400);
	prime_prediction_tick(*landed_boat, 0, 6400);
	prime_prediction_tick(*fallback_boat, 0, 6400);
	wet_boat->veh.plat_afloat = true;
	wet_boat->veh.plat_solve_valid = true;
	landed_boat->veh.plat_afloat = false;
	landed_boat->veh.plat_solve_valid = true;
	// fallback has no model boxes and starts with the default false latch.

	w::watercraft_client_tick(wet.world, *wet_boat, wet.traits);
	w::watercraft_client_tick(landed.world, *landed_boat, landed.traits);
	w::watercraft_client_tick(fallback.world, *fallback_boat, fallback.traits);

	bool ok = true;
	ok &= expect(wet_boat->veh.vel_x == 6202,
	             "afloat hull gets only the two exact 1/64 planar sheds");
	ok &= expect(landed_boat->veh.vel_x == 5427,
	             "landed hull additionally gets the exact (v+4)>>3 shed");
	ok &= expect(fallback_boat->veh.vel_x == 6202 &&
	             fallback_boat->veh.plat_afloat,
	             "model-less water world seeds the afloat fallback before drag");
	return ok;
}

bool run_first_prediction_seeds_platform_state() {
	// A resolved joiner hull has no preceding local platform tick. Its first
	// predicted frame must establish the previous-solve latch before contact
	// drag, producing the same velocity as an already-solved afloat hull.
	Rig first, warm;
	make_rig(first);
	make_rig(warm);
	set_zodiac_boxes(first.traits);
	set_zodiac_boxes(warm.traits);
	w::Entity *first_boat = first.world.registry.get(first.boat);
	w::Entity *warm_boat = warm.world.registry.get(warm.boat);
	if (!expect(first_boat && warm_boat,
	            "first-platform-state boats spawned")) return false;
	prime_prediction_tick(*first_boat, 0, 6400);
	prime_prediction_tick(*warm_boat, 0, 6400);
	warm_boat->veh.plat_afloat = true;
	warm_boat->veh.plat_solve_valid = true;

	w::watercraft_client_tick(first.world, *first_boat, first.traits);
	w::watercraft_client_tick(warm.world, *warm_boat, warm.traits);
	return expect(first_boat->veh.plat_solve_valid &&
	                      first_boat->veh.plat_afloat &&
	                      first_boat->veh.vel_x == warm_boat->veh.vel_x,
	              "first predicted hull seeds afloat state before drag");
}

bool run_platform_basis_preserves_roll_sign() {
	// The platform solve consumes the same retail Q22
	// Rz*Ry(-pitch)*Rx(+roll) basis as collision/bone transforms. With no
	// contact or water adjustment, fitting its own lever corners must preserve
	// a positive authored roll instead of reflecting it through zero.
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "roll-basis boat spawned")) return false;
	boat->position.z = 20.0f;
	boat->veh.yaw_seeded = true;
	boat->veh.yaw_bam = w::bam_from_degrees_wrapped(37.0);
	boat->veh.air_pitch_bam = w::bam_from_degrees_wrapped(11.0);
	boat->veh.air_roll_bam = w::bam_from_degrees_wrapped(19.0);
	w::watercraft_platform_solve(r.world, *boat, r.traits);
	return expect(boat->veh.air_roll_bam > 0,
	              "platform Q22 basis preserves positive roll sign");
}

bool run_water_rudder_wraps_min_speed() {
	// Retail NEG is a 32-bit wrapping register operation: INT_MIN remains
	// INT_MIN. Promoting before negation reverses the rudder-rate sign.
	Rig r;
	make_rig(r);
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "min-speed boat spawned")) return false;
	prime_prediction_tick(*boat, 0);
	boat->veh.speed = INT32_MIN;
	boat->veh.steer_state = 0x10000;
	boat->veh.plat_afloat = true;
	boat->veh.plat_solve_valid = true;
	r.traits.turn_rate = 0;
	r.traits.turn_rate2 = 0;
	r.traits.water_speed = 0;
	w::watercraft_client_tick(r.world, *boat, r.traits);
	const int32_t expected = static_cast<int32_t>(
			(static_cast<int64_t>(INT32_MIN) * (0x10000 >> 2) + 0x8000) >> 16);
	return expect(expected < 0 && boat->veh.wheel_rate_bam < 0,
	              "water rudder keeps x86 wrapped NEG semantics for INT_MIN speed");
}

bool run_airborne_watercraft_preserves_yaw_rate() {
	Rig r;
	make_rig(r);
	r.world.env.water_z = 0;
	w::Entity *boat = r.world.registry.get(r.boat);
	if (!expect(boat != nullptr, "airborne-rudder boat spawned")) return false;
	prime_prediction_tick(*boat, 0);
	boat->flags |= w::kEntityFlagInAir;
	boat->veh.plat_afloat = false;
	boat->veh.plat_solve_valid = true;
	boat->veh.speed = 1 << 16;
	const int32_t initial_rate = 0x100000;
	boat->veh.wheel_rate_bam = initial_rate;
	r.traits.turn_rate = 0x400000;
	r.traits.turn_rate2 = 0x100000;

	w::watercraft_client_tick(r.world, *boat, r.traits);
	int32_t expected = opennova::io::bam_sub(
			initial_rate,
			opennova::io::bam_sar(
					opennova::io::bam_add(initial_rate, 2), 2));
	expected = opennova::io::bam_sub(
			opennova::io::bam_sub(
					expected,
					opennova::io::bam_sar(
							opennova::io::bam_add(expected, 16), 5)),
			opennova::io::bam_sar(expected, 31));
	return expect(boat->veh.wheel_rate_bam == expected,
	              "airborne non-afloat hull retains its prior yaw rate through sheds");
}

bool run_prior_euler_drives_thrust_and_beach_stop() {
	// A bow-up hull produces positive vertical thrust from the PRIOR solve and
	// hits the witnessed deep-beach full stop. A level hull has fwd.z == 0 and
	// only sheds. An upside-down hull fails the full-Euler up.z thrust gate.
	RampField shore(false); // uniform terrain height 2.0
	Rig pitched, level, capsized, upright;
	for (Rig *r : {&pitched, &level, &capsized, &upright}) {
		make_rig(*r);
		set_zodiac_boxes(r->traits);
		w::Entity *boat = r->world.registry.get(r->boat);
		prime_prediction_tick(*boat, 8192, 6400);
		boat->veh.plat_afloat = true;
		boat->veh.plat_solve_valid = true;
	}
	pitched.world.env.water_z = 1 << 16;
	level.world.env.water_z = 1 << 16;
	pitched.world.terrain = &shore.field;
	level.world.terrain = &shore.field;
	pitched.world.registry.get(pitched.boat)->position = {100.0f, 0.0f, 10.0f};
	level.world.registry.get(level.boat)->position = {100.0f, 0.0f, 10.0f};
	pitched.world.registry.get(pitched.boat)->veh.air_pitch_bam =
			w::bam_from_degrees_wrapped(30.0);
	level.world.registry.get(level.boat)->veh.air_pitch_bam = 0;
	capsized.world.registry.get(capsized.boat)->veh.air_roll_bam =
			static_cast<int32_t>(0x80000000u);
	upright.world.registry.get(upright.boat)->veh.air_roll_bam = 0;
	// The roll-gate pair has no shore and starts at rest, isolating thrust.
	capsized.world.registry.get(capsized.boat)->veh.vel_x = 0;
	upright.world.registry.get(upright.boat)->veh.vel_x = 0;

	w::watercraft_client_tick(pitched.world,
			*pitched.world.registry.get(pitched.boat), pitched.traits);
	w::watercraft_client_tick(level.world,
			*level.world.registry.get(level.boat), level.traits);
	w::watercraft_client_tick(capsized.world,
			*capsized.world.registry.get(capsized.boat), capsized.traits);
	w::watercraft_client_tick(upright.world,
			*upright.world.registry.get(upright.boat), upright.traits);

	const auto &pitched_m = pitched.world.registry.get(pitched.boat)->veh;
	const auto &level_m = level.world.registry.get(level.boat)->veh;
	const auto &capsized_m = capsized.world.registry.get(capsized.boat)->veh;
	const auto &upright_m = upright.world.registry.get(upright.boat)->veh;
	bool ok = true;
	ok &= expect(pitched_m.vel_x == 0 && pitched_m.vel_y == 0 &&
	             pitched_m.wheel_rate_bam == 0,
	             "positive prior pitch triggers the exact deep-beach full stop");
	ok &= expect(level_m.vel_x != 0,
	             "level prior pitch keeps the beach full-stop gate closed");
	ok &= expect(capsized_m.vel_x == 0 && capsized_m.vel_y == 0,
	             "prior roll closes thrust when the hull up axis is inverted");
	ok &= expect(upright_m.vel_x > 0,
	             "upright full-Euler basis admits forward thrust");
	return ok;
}

bool run_platform_solve_precedes_yaw() {
	// Build an exact pre-solve state by running the same mover with unresolved
	// boxes. Applying the platform solve at old yaw is the retail oracle; applying
	// it at post-rate yaw is the former bug and must produce a different fit on
	// this X-ramp terrain.
	RampField ramp(true);
	Rig actual, staged, expected, wrong;
	for (Rig *r : {&actual, &staged, &expected, &wrong}) {
		make_rig(*r);
		r->world.env.water_z = 0;
		r->world.terrain = &ramp.field;
		w::Entity *boat = r->world.registry.get(r->boat);
		boat->position = {100.0f, 0.0f, 6.5f};
		prime_prediction_tick(*boat, 0);
		boat->veh.speed = 1 << 16;
		boat->veh.steer_state = static_cast<int32_t>(0xC0000000u);
	}
	set_zodiac_boxes(actual.traits);
	set_zodiac_boxes(expected.traits);
	set_zodiac_boxes(wrong.traits);
	actual.world.registry.get(actual.boat)->veh.plat_solve_valid = true;
	actual.traits.max_slope = w::bam_from_degrees_wrapped(60.0);
	actual.traits.slip_slope = w::bam_from_degrees_wrapped(50.0);
	expected.traits.max_slope = actual.traits.max_slope;
	expected.traits.slip_slope = actual.traits.slip_slope;
	wrong.traits.max_slope = actual.traits.max_slope;
	wrong.traits.slip_slope = actual.traits.slip_slope;

	w::watercraft_client_tick(staged.world,
			*staged.world.registry.get(staged.boat), staged.traits);
	w::Entity *staged_boat = staged.world.registry.get(staged.boat);
	const int32_t post_yaw = staged_boat->veh.yaw_bam;
	const int32_t yaw_rate = staged_boat->veh.wheel_rate_bam;
	const int32_t pre_solve_yaw = opennova::io::bam_sub(post_yaw, yaw_rate);
	for (Rig *r : {&expected, &wrong}) {
		w::Entity *boat = r->world.registry.get(r->boat);
		boat->position = staged_boat->position;
		boat->flags = staged_boat->flags;
		boat->veh = staged_boat->veh;
	}
	expected.world.registry.get(expected.boat)->veh.yaw_bam = pre_solve_yaw;
	w::watercraft_platform_solve(expected.world,
			*expected.world.registry.get(expected.boat), expected.traits);
	// wrong deliberately keeps post_yaw while fitting.
	w::watercraft_platform_solve(wrong.world,
			*wrong.world.registry.get(wrong.boat), wrong.traits);
	w::watercraft_client_tick(actual.world,
			*actual.world.registry.get(actual.boat), actual.traits);

	const w::Entity *actual_boat = actual.world.registry.get(actual.boat);
	const w::Entity *expected_boat = expected.world.registry.get(expected.boat);
	const w::Entity *wrong_boat = wrong.world.registry.get(wrong.boat);
	const bool oracle_distinguishes_order =
			expected_boat->veh.air_pitch_bam != wrong_boat->veh.air_pitch_bam ||
			expected_boat->veh.air_roll_bam != wrong_boat->veh.air_roll_bam;
	bool ok = true;
	ok &= expect(yaw_rate != 0 && post_yaw != pre_solve_yaw,
	             "order fixture produces a non-zero yaw application");
	ok &= expect(oracle_distinguishes_order,
	             "ramp fixture distinguishes pre-yaw and post-yaw platform fits");
	ok &= expect(actual_boat->veh.air_pitch_bam ==
	                     expected_boat->veh.air_pitch_bam &&
	             actual_boat->veh.air_roll_bam ==
	                     expected_boat->veh.air_roll_bam,
	             "watercraft tick solves the platform before applying yaw");
	ok &= expect(actual_boat->veh.yaw_bam == post_yaw,
	             "watercraft tick applies the decayed yaw rate after the solve");
	return ok;
}

bool run_vehicle_chase_wraps_bam_seam() {
	Rig r;
	make_rig(r);
	r.world.env.water_z = 0;
	r.traits.family = w::VehicleFamily::Ground;
	r.traits.turn_rate = 0;
	r.traits.turn_rate2 = 0;
	r.traits.player_speed = 0;
	r.traits.acceleration = 0;
	r.traits.deceleration = 0;
	w::Entity *veh = r.world.registry.get(r.boat);
	const int32_t start = static_cast<int32_t>(0x7FFFFFFEu);
	const int32_t target = static_cast<int32_t>(0x80000052u);
	veh->veh.yaw_seeded = true;
	veh->veh.yaw_bam = start;
	stage(*veh, w::to_fixed(veh->position.x), w::to_fixed(veh->position.y),
	      w::to_fixed(veh->position.z), target, 0, start);
	w::ground_client_tick(r.world, *veh, r.traits);
	const int32_t expected_step = opennova::io::bam_add(
			opennova::io::bam_sub(target, start), 10) / 20;
	const int32_t expected = opennova::io::bam_add(start, expected_step);
	return expect(expected < 0 && veh->veh.yaw_bam == expected,
	              "vehicle heading chase takes the wrapped shortest arc across the BAM seam");
}

bool run_ground_rudder_wraps_bam_seam() {
	Rig r;
	make_rig(r);
	r.world.env.water_z = 0;
	r.traits.family = w::VehicleFamily::Ground;
	r.traits.turn_rate = 0x01000000;
	r.traits.turn_rate2 = 0x00400000;
	r.traits.player_speed = 100 << 16;
	w::Entity *veh = r.world.registry.get(r.boat);
	if (!expect(veh != nullptr, "ground seam vehicle spawned")) return false;
	auto &m = veh->veh;
	m.net_predicted = true;
	m.net_interp_progress = 1;
	m.yaw_seeded = true;
	m.yaw_bam = static_cast<int32_t>(0x7FFFFF00u);
	m.net_recv_steer_bam = static_cast<int32_t>(0x80000100u);
	m.net_recv_speed = 1 << 16;
	m.speed = 1 << 16;
	m.grounded = true;
	const int32_t start = m.yaw_bam;
	w::ground_client_tick(r.world, *veh, r.traits);
	return expect(m.steer_state < 0 && m.wheel_rate_bam > 0 &&
	                      opennova::io::bam_sub(m.yaw_bam, start) > 0 &&
	                      opennova::io::bam_sub(m.net_recv_steer_bam,
	                                             m.yaw_bam) > 0,
	              "ground rudder takes the short positive arc across the BAM seam");
}

bool run_ground_and_bike_local_driver_input_gate() {
	bool ok = true;
	for (const w::VehicleFamily family :
	     {w::VehicleFamily::Ground, w::VehicleFamily::Bike}) {
		Rig local;
		make_rig(local);
		local.world.env.water_z = 0;
		local.traits.family = family;
		local.traits.player_control = true;
		local.traits.player_speed = 12000;
		w::Entity *local_vehicle = local.world.registry.get(local.boat);
		w::Entity *local_driver = mount_prediction_driver(local, true);
		if (!expect(local_vehicle && local_driver,
		            "local ground/bike prediction rig ready")) return false;
		local_driver->net_move_input = 0x08; // moving forward
		local_driver->yaw = 37;
		prime_prediction_tick(*local_vehicle, -7000);
		local_vehicle->veh.net_recv_steer_bam =
				w::bam_from_degrees_wrapped(-71.0);
		w::ground_client_tick(local.world, *local_vehicle, local.traits);
		// The controlling client stages its own command, then halves it
		// against the received register: [136] = ([136] + [177]) >> 1
		// [orig: ground @0x48bbef; bike @0x484dad].
		ok &= expect(local_vehicle->veh.cmd_speed ==
		                     opennova::io::bam_sar(opennova::io::bam_add(
		                             local.traits.player_speed, -7000), 1),
		             "local ground/bike driver blends current command with the wire mirror");
		ok &= expect(local_vehicle->veh.steer_target_bam ==
		                     w::bam_heading_from_mission_yaw_deg(37.0),
		             "local ground/bike driver retains current player steer");

		Rig remote;
		make_rig(remote);
		remote.world.env.water_z = 0;
		remote.traits.family = family;
		remote.traits.player_control = true;
		remote.traits.player_speed = 12000;
		w::Entity *remote_vehicle = remote.world.registry.get(remote.boat);
		w::Entity *remote_driver = mount_prediction_driver(remote, false);
		if (!expect(remote_vehicle && remote_driver,
		            "remote ground/bike prediction rig ready")) return false;
		remote_driver->net_move_input = 0x08;
		remote_driver->yaw = 37;
		const int32_t received_steer = w::bam_from_degrees_wrapped(-71.0);
		prime_prediction_tick(*remote_vehicle, -7000);
		remote_vehicle->veh.net_recv_steer_bam = received_steer;
		w::ground_client_tick(remote.world, *remote_vehicle, remote.traits);
		ok &= expect(remote_vehicle->veh.cmd_speed == -7000,
		             "remote ground/bike occupant still uses received speed");
		ok &= expect(remote_vehicle->veh.steer_target_bam == received_steer,
		             "remote ground/bike occupant still uses received steer");
	}
	return ok;
}

bool run_watercraft_local_driver_reconciliation_gate() {
	Rig local;
	make_rig(local);
	local.traits.player_control = true;
	// Retail-shaped: cbot defs author waterSpeed (+0x8EC) and no player_speed —
	// the boat player leg reads waterSpeed at every command site.
	local.traits.water_speed = 12000;
	w::Entity *local_boat = local.world.registry.get(local.boat);
	w::Entity *local_driver = mount_prediction_driver(local, true);
	if (!expect(local_boat && local_driver,
	            "local watercraft prediction rig ready")) return false;
	local_driver->net_move_input = 0x08;
	local_driver->yaw = 53;
	prime_prediction_tick(*local_boat, -2000);
	local_boat->veh.net_recv_steer_bam = w::bam_from_degrees_wrapped(-80.0);
	w::watercraft_client_tick(local.world, *local_boat, local.traits);
	bool ok = true;
	ok &= expect(local_boat->veh.cmd_speed == 5000,
	             "local watercraft averages current and received speed commands");
	ok &= expect(local_boat->veh.steer_target_bam ==
	                     w::bam_heading_from_mission_yaw_deg(53.0),
	             "local watercraft retains current player steer");

	Rig remote;
	make_rig(remote);
	remote.traits.player_control = true;
	remote.traits.water_speed = 12000;
	w::Entity *remote_boat = remote.world.registry.get(remote.boat);
	w::Entity *remote_driver = mount_prediction_driver(remote, false);
	if (!expect(remote_boat && remote_driver,
	            "remote watercraft prediction rig ready")) return false;
	remote_driver->net_move_input = 0x08;
	remote_driver->yaw = 53;
	const int32_t received_steer = w::bam_from_degrees_wrapped(-80.0);
	prime_prediction_tick(*remote_boat, -2000);
	remote_boat->veh.net_recv_steer_bam = received_steer;
	w::watercraft_client_tick(remote.world, *remote_boat, remote.traits);
	ok &= expect(remote_boat->veh.cmd_speed == -2000,
	             "remote watercraft occupant still uses received speed");
	ok &= expect(remote_boat->veh.steer_target_bam == received_steer,
	             "remote watercraft occupant still uses received steer");

	Rig wrapped;
	make_rig(wrapped);
	wrapped.traits.player_control = true;
	wrapped.traits.water_speed = std::numeric_limits<int32_t>::max();
	w::Entity *wrapped_boat = wrapped.world.registry.get(wrapped.boat);
	w::Entity *wrapped_driver = mount_prediction_driver(wrapped, true);
	if (!expect(wrapped_boat && wrapped_driver,
	            "wrapped watercraft prediction rig ready")) return false;
	wrapped_driver->net_move_input = 0x08;
	prime_prediction_tick(*wrapped_boat, std::numeric_limits<int32_t>::max());
	w::watercraft_client_tick(wrapped.world, *wrapped_boat, wrapped.traits);
	ok &= expect(wrapped_boat->veh.cmd_speed == -1,
	             "watercraft reconciliation wraps ADD then arithmetic-shifts like x86");
	return ok;
}
bool run_aircraft_local_pilot_input_gate() {
	// The AIR local-driver input map (D-NET-196 B-facet closure): the LOCAL
	// PILOT'S machine runs the occupant input block and reconciles BOTH air
	// commands with the received mirrors — ([544]+[708])>>1 fwd,
	// ([540]+[712])>>1 lateral — while a remote-piloted row keeps the
	// verbatim register mirror [orig: Entity_UpdateAircraftPhysics @0x490310,
	// the blend @0x491546..0x491568; the key-dir thrust table and analog arm
	// in the occupant block].
	Rig local;
	make_rig(local);
	local.traits.family = w::VehicleFamily::Helicopter;
	local.traits.player_control = true;
	local.traits.player_speed = 12000;
	w::Entity *heli = local.world.registry.get(local.boat);
	w::Entity *pilot = mount_prediction_driver(local, true);
	if (!expect(heli && pilot, "local air prediction rig ready")) return false;
	pilot->net_move_input = 0x08; // moving forward -> cmd = flight speed
	pilot->yaw = 41;
	prime_prediction_tick(*heli, -7000);
	heli->veh.net_recv_lat = 3000;
	heli->veh.net_recv_steer_bam = w::bam_from_degrees_wrapped(-66.0);
	heli->veh.net_engine_on = true; // replicated Flags 0x80: engine spun up
	w::aircraft_client_tick(local.world, *heli, local.traits);
	bool ok = true;
	ok &= expect(heli->veh.cmd_speed ==
	                     opennova::io::bam_sar(opennova::io::bam_add(
	                             local.traits.player_speed, -7000), 1),
	             "the local pilot blends forward command with the wire mirror");
	ok &= expect(heli->veh.cmd_lateral_speed ==
	                     opennova::io::bam_sar(3000, 1),
	             "the local pilot blends lateral command with the wire mirror");
	ok &= expect(heli->veh.steer_target_bam ==
	                     w::bam_heading_from_mission_yaw_deg(41.0),
	             "the local pilot's LOOK owns the steer target");

	Rig remote;
	make_rig(remote);
	remote.traits.family = w::VehicleFamily::Helicopter;
	remote.traits.player_control = true;
	remote.traits.player_speed = 12000;
	w::Entity *rheli = remote.world.registry.get(remote.boat);
	w::Entity *rpilot = mount_prediction_driver(remote, false);
	if (!expect(rheli && rpilot, "remote air prediction rig ready")) return false;
	rpilot->net_move_input = 0x08;
	rpilot->yaw = 41;
	const int32_t received_steer = w::bam_from_degrees_wrapped(-66.0);
	prime_prediction_tick(*rheli, -7000);
	rheli->veh.net_recv_lat = 3000;
	rheli->veh.net_recv_steer_bam = received_steer;
	rheli->veh.net_engine_on = true;
	w::aircraft_client_tick(remote.world, *rheli, remote.traits);
	ok &= expect(rheli->veh.cmd_speed == -7000,
	             "a remote-piloted aircraft keeps the received forward mirror");
	ok &= expect(rheli->veh.cmd_lateral_speed == 3000,
	             "a remote-piloted aircraft keeps the received lateral mirror");
	ok &= expect(rheli->veh.steer_target_bam == received_steer,
	             "a remote-piloted aircraft keeps the received steer mirror");
	return ok;
}

bool run_platform_roll_is_stable_and_gravity_witnessed() {
	// Review-pinned invariants: (1) the fit/builder pair is self-consistent
	// in ROLL (a heeled hull may not flip sign tick-over-tick - the pre-fix
	// extractor returned -R for a build with roll R, a 62 Hz shimmy); (2) an
	// out-of-water hull falls at the WATERCRAFT gravity 167/tick [orig:
	// @0x48EBD9], not the ground 324.
	Rig r;
	make_rig(r);
	r.traits.box_y_lo = -(1 << 16); r.traits.box_y_hi = 1 << 16;
	r.traits.box_x_lo = -(5 << 15); r.traits.box_x_hi = 5 << 15;
	r.traits.box_z_lo = -(1 << 15); r.traits.box_z_hi = 1 << 16;
	r.traits.foot_x_lo = r.traits.box_x_lo; r.traits.foot_x_hi = r.traits.box_x_hi;
	r.traits.foot_y_lo = r.traits.box_y_lo; r.traits.foot_y_hi = r.traits.box_y_hi;
	w::Entity *boat = r.world.registry.get(r.boat);
	stage(*boat, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(10.0f),
	      0, 0, 0);
	// Seed a heel and let the solve iterate at the waterline: roll must decay
	// monotonically-ish toward level, never alternate sign with constant
	// magnitude.
	boat->veh.air_roll_bam = 0x08000000; // ~11 deg heel
	int sign_flips = 0;
	int32_t prev = boat->veh.air_roll_bam;
	for (int t = 0; t < 120; ++t) {
		w::watercraft_client_tick(r.world, *boat, r.traits);
		const int32_t now = boat->veh.air_roll_bam;
		if ((now ^ prev) < 0 && std::abs(now) > 0x01000000) ++sign_flips;
		prev = now;
	}
	std::fprintf(stderr, "[platform-roll] final roll=%d flips=%d\n",
	             boat->veh.air_roll_bam, sign_flips);
	// A damped decay may cross zero once; the PRE-FIX inverted extractor
	// alternated every tick (~119 flips here). Pin the absence of sustained
	// alternation.
	bool ok = expect(sign_flips <= 2,
	                 "a heeled hull never flip-flops roll sign (fit/builder consistent)");
	ok &= expect(std::abs(boat->veh.air_roll_bam) < 0x04000000,
	             "the heel decays toward level on flat water");

	// Gravity: hoist the hull high above the water, one record, no commands.
	// After the airborne flag latches, slide_z must step by exactly -167.
	Rig g;
	make_rig(g);
	g.traits.box_y_lo = r.traits.box_y_lo; g.traits.box_y_hi = r.traits.box_y_hi;
	g.traits.box_x_lo = r.traits.box_x_lo; g.traits.box_x_hi = r.traits.box_x_hi;
	g.traits.box_z_lo = r.traits.box_z_lo; g.traits.box_z_hi = r.traits.box_z_hi;
	g.traits.foot_x_lo = g.traits.box_x_lo; g.traits.foot_x_hi = g.traits.box_x_hi;
	g.traits.foot_y_lo = g.traits.box_y_lo; g.traits.foot_y_hi = g.traits.box_y_hi;
	w::Entity *fly = g.world.registry.get(g.boat);
	fly->position.z = 40.0f;
	stage(*fly, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(40.0f),
	      0, 0, 0);
	w::watercraft_client_tick(g.world, *fly, g.traits); // latch not-afloat
	const int32_t s0 = fly->veh.slide_z;
	w::watercraft_client_tick(g.world, *fly, g.traits);
	const int32_t s1 = fly->veh.slide_z;
	std::fprintf(stderr, "[platform-grav] slide step=%d (want -167)\n", s1 - s0);
	ok &= expect(s1 - s0 == -167,
	             "an out-of-water hull falls at the witnessed 167/tick");
	return ok;
}


// The aircraft contact solve (vehicle-client-movers-re.md s6, ported): a
// descending heli's pads catch the terrain - the airborne flag clears, Z
// conforms to the chassis fit, and the hull sits level on flat ground; a
// flying hull keeps the flag and its own Z.
bool run_aircraft_contact_lands_and_conforms() {
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	r.traits.max_slope = w::bam_from_degrees_wrapped(60.0);
	r.traits.slip_slope = w::bam_from_degrees_wrapped(50.0);
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	std::vector<uint16_t> heightmap(64 * 64, 0);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;
	w::Entity *heli = r.world.registry.get(r.boat);
	if (!expect(heli != nullptr, "heli spawned")) return false;
	// Start airborne well above ground 0 and descend.
	heli->position.z = 6.0f;
	heli->flags |= w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(6.0f),
	      0, 0, 0);
	heli->veh.net_engine_on = false;
	w::aircraft_client_tick(r.world, *heli, r.traits);
	if (!expect((heli->flags & w::kEntityFlagInAir) != 0,
	            "the flying hull keeps the airborne flag")) return false;
	// Drive it down: force a descent rate each tick until contact.
	bool grounded = false;
	for (int t = 0; t < 400 && !grounded; ++t) {
		heli->veh.slide_z = -4000;
		heli->veh.net_alt_target = w::to_fixed(-10.0f);
		w::aircraft_client_tick(r.world, *heli, r.traits);
		grounded = (heli->flags & w::kEntityFlagInAir) == 0;
	}
	const double z = double(heli->position.z);
	const double pitch_deg =
		double(heli->veh.air_pitch_bam) * (360.0 / 4294967296.0);
	const double roll_deg =
		double(heli->veh.air_roll_bam) * (360.0 / 4294967296.0);
	std::fprintf(stderr, "[air-contact] grounded=%d z=%.3f pitch=%.2f roll=%.2f\n",
	             int(grounded), z, pitch_deg, roll_deg);
	bool ok = expect(grounded, "the descending heli grounds on its pads");
	// The chassis fit parks the hull at pad-contact height over ground 0:
	// center z ~ -box_z_lo above the terrain (the pad centers ride r above
	// it), well under the start height.
	ok &= expect(z > -1.0 && z < 3.0, "Z conforms to the chassis fit");
	ok &= expect(std::abs(pitch_deg) < 10.0 && std::abs(roll_deg) < 10.0,
	             "the grounded hull sits near level on flat terrain");
	return ok;
}

// Landing on the 1/16 ramp (slope along +X, hull facing +X): the corner-quad
// fit must put the slope into PITCH with roll near zero - a swapped
// forward/side pad mapping would land the slope in roll and fail here.
bool run_aircraft_contact_conforms_to_ramp() {
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	r.traits.max_slope = w::bam_from_degrees_wrapped(60.0);
	r.traits.slip_slope = w::bam_from_degrees_wrapped(50.0);
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	RampField ramp(true);
	r.world.terrain = &ramp.field;
	w::Entity *heli = r.world.registry.get(r.boat);
	// Terrain at x=100 is 100/16 = 6.25 m; start above it and descend.
	heli->position.z = 12.0f;
	heli->flags |= w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(12.0f),
	      0, 0, 0);
	bool grounded = false;
	for (int t = 0; t < 400 && !grounded; ++t) {
		heli->veh.slide_z = -4000;
		heli->veh.net_alt_target = w::to_fixed(-10.0f);
		w::aircraft_client_tick(r.world, *heli, r.traits);
		grounded = (heli->flags & w::kEntityFlagInAir) == 0;
	}
	// Let the parked conform settle: keep pressing down so the pad depths
	// reach their slope-shaped steady state.
	for (int t = 0; t < 60; ++t) {
		heli->veh.slide_z = -4000;
		heli->veh.net_alt_target = w::to_fixed(-10.0f);
		w::aircraft_client_tick(r.world, *heli, r.traits);
	}
	grounded = (heli->flags & w::kEntityFlagInAir) == 0;
	const double z = double(heli->position.z);
	double pitch_deg = double(heli->veh.air_pitch_bam) * (360.0 / 4294967296.0);
	double roll_deg = double(heli->veh.air_roll_bam) * (360.0 / 4294967296.0);
	if (pitch_deg > 180.0) pitch_deg -= 360.0;
	if (roll_deg > 180.0) roll_deg -= 360.0;
	std::fprintf(stderr, "[air-ramp] grounded=%d z=%.3f pitch=%.2f roll=%.2f\n",
	             int(grounded), z, pitch_deg, roll_deg);
	bool ok = expect(grounded, "the heli grounds on the ramp");
	ok &= expect(z > 5.5 && z < 8.5, "Z conforms near the ramp surface");
	ok &= expect(std::abs(pitch_deg) > 2.0 && std::abs(pitch_deg) < 6.0,
	             "the ramp slope lands in the pitch axis");
	ok &= expect(std::abs(roll_deg) < 1.5,
	             "roll stays level across the slope");
	return ok;
}

// The in-water flag (s6.5) across the r/2 hysteresis band. Zodiac boxes give
// r = beam/4 = 0.5 u, pad plane at model Z 0, hull-bottom term 0 - so the
// test height IS the hull Z, and the set-hold band is [water_z, water_z+0.25).
bool run_aircraft_water_flag_hysteresis() {
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 10 << 16;
	w::Entity *heli = r.world.registry.get(r.boat);
	heli->position.z = 9.9f; // pads under the plane
	heli->flags |= w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(9.9f),
	      0, 0, 0);
	heli->veh.slide_z = -1000; // outside the sleep window
	w::aircraft_client_tick(r.world, *heli, r.traits);
	bool ok = expect((heli->flags & 0x8000u) != 0u,
	                 "pads under the water plane set the in-water flag");
	// INSIDE the band: above the plane but within r/2 - only the hysteresis
	// keeps the flag held; a port without it clears here.
	heli->position.z = 10.1f;
	heli->veh.slide_z = -1000;
	w::aircraft_client_tick(r.world, *heli, r.traits);
	ok &= expect((heli->flags & 0x8000u) != 0u,
	             "inside the r/2 band the flag holds (the hysteresis)");
	// Past the band: the flag drops.
	heli->position.z = 10.4f;
	heli->veh.slide_z = -1000;
	w::aircraft_client_tick(r.world, *heli, r.traits);
	ok &= expect((heli->flags & 0x8000u) == 0u,
	             "clear of the band the flag drops");
	return ok;
}

// The s6.4 severity block: a sev-3 slope (steeper than max_slope) sheds the
// torque fraction, applies the distance-gated 0.25 speed cut, and shoves the
// hull planar away from the face.
bool run_aircraft_steep_slope_sheds_and_shoves() {
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	r.traits.max_slope = w::bam_from_degrees_wrapped(60.0);
	r.traits.slip_slope = w::bam_from_degrees_wrapped(50.0);
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	// Slope 3 along +X (~71.6 deg) - steeper than max_slope 60 -> sev 3.
	std::vector<uint16_t> heightmap(64 * 64);
	std::vector<int> sector_grid(256, 1);
	for (int y = 0; y < 64; ++y)
		for (int x = 0; x < 64; ++x)
			heightmap[y * 64 + x] = static_cast<uint16_t>(x * 768);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;
	w::Entity *heli = r.world.registry.get(r.boat);
	// Terrain at x=10 is 30 u; sit the hull against the face.
	heli->position.x = 10.0f;
	heli->position.y = 30.0f;
	heli->position.z = 30.4f;
	heli->flags &= ~w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(10.0f), w::to_fixed(30.0f), w::to_fixed(30.4f),
	      0, 0, 0);
	heli->veh.speed = 20000;
	heli->veh.slide_z = -1000;
	const float x0 = heli->position.x;
	w::aircraft_client_tick(r.world, *heli, r.traits);
	std::fprintf(stderr, "[air-steep] speed=%d dx=%.4f\n", heli->veh.speed,
	             double(heli->position.x - x0));
	bool ok = expect(heli->veh.speed < 20000 / 4 + 1,
	                 "the sev-3 face sheds + quarter-cuts the speed");
	ok &= expect(heli->position.x < x0,
	             "the planar force shoves the hull off the face (downhill)");
	return ok;
}

// Landing on a SIDE slope (1/16 along +Y, hull facing +X): the identity
// corner-depth pairing puts the slope in ROLL with pitch near zero, in the
// fit convention's sign (pinned by the boat basis legs; any within-side
// transposition or mirror of the pairing flips or zeroes this).
bool run_aircraft_contact_conforms_to_side_slope() {
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	r.traits.max_slope = w::bam_from_degrees_wrapped(60.0);
	r.traits.slip_slope = w::bam_from_degrees_wrapped(50.0);
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	std::vector<uint16_t> heightmap(512 * 512);
	std::vector<int> sector_grid(256, 1);
	for (int y = 0; y < 512; ++y)
		for (int x = 0; x < 512; ++x)
			heightmap[y * 512 + x] = static_cast<uint16_t>(y * 16);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 512;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;
	w::Entity *heli = r.world.registry.get(r.boat);
	// Terrain at y=200 is 12.5 u; start above and descend.
	heli->position.z = 18.0f;
	heli->flags |= w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(18.0f),
	      0, 0, 0);
	bool grounded = false;
	for (int t = 0; t < 400 && !grounded; ++t) {
		heli->veh.slide_z = -4000;
		heli->veh.net_alt_target = w::to_fixed(-10.0f);
		w::aircraft_client_tick(r.world, *heli, r.traits);
		grounded = (heli->flags & w::kEntityFlagInAir) == 0;
	}
	for (int t = 0; t < 60; ++t) {
		heli->veh.slide_z = -4000;
		heli->veh.net_alt_target = w::to_fixed(-10.0f);
		w::aircraft_client_tick(r.world, *heli, r.traits);
	}
	grounded = (heli->flags & w::kEntityFlagInAir) == 0;
	double pitch_deg = double(heli->veh.air_pitch_bam) * (360.0 / 4294967296.0);
	double roll_deg = double(heli->veh.air_roll_bam) * (360.0 / 4294967296.0);
	if (pitch_deg > 180.0) pitch_deg -= 360.0;
	if (roll_deg > 180.0) roll_deg -= 360.0;
	std::fprintf(stderr, "[air-side] grounded=%d pitch=%.2f roll=%.2f\n",
	             int(grounded), pitch_deg, roll_deg);
	bool ok = expect(grounded, "the heli grounds on the side slope");
	// Terrain rising toward +Y under a +X-facing hull conforms to NEGATIVE
	// roll in the fit/basis convention (the same pair the boat legs pin);
	// a mirrored corner-depth pairing lands at +roll and fails here.
	ok &= expect(roll_deg < -2.0 && roll_deg > -6.0,
	             "the side slope lands in the roll axis, convention sign");
	ok &= expect(std::abs(pitch_deg) < 1.5,
	             "pitch stays level along the contour");
	return ok;
}

// The s6.15 sleep fast-path: an at-rest hull undoes the vertical dribble
// and halves slideDecay instead of running the solve.
bool run_aircraft_sleep_fast_path() {
	// The s6.15 sleep fast-path, behaviorally: a landed, motion-less hull
	// (servo-neutral altitude target) holds its Z and bleeds slideDecay to
	// rest through the halving [orig: @0x47F059/@0x47F067] instead of
	// dribbling downward forever. (The mover's own pre-solve sheds run first
	// in retail too - the window tests the post-shed value.)
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.player_speed = 20972;
	r.traits.climb_speed = 2930;
	set_zodiac_boxes(r.traits);
	r.world.env.water_z = 0;
	std::vector<uint16_t> heightmap(64 * 64, 0);
	std::vector<int> sector_grid(256, 1);
	opennova::terrain::TerrainHeightField field;
	field.heightmap = heightmap.data();
	field.dim = 64;
	field.layout.sector_grid = sector_grid.data();
	field.layout.origin_x = 0;
	field.layout.origin_y = 0;
	r.world.terrain = &field;
	w::Entity *heli = r.world.registry.get(r.boat);
	heli->position.z = 0.5f; // the landed chassis height over ground 0
	heli->flags &= ~w::kEntityFlagInAir;
	stage(*heli, w::to_fixed(100.0f), w::to_fixed(200.0f), w::to_fixed(0.5f),
	      0, 0, 0);
	heli->veh.net_predicted = true;
	heli->veh.net_interp_progress = 1;
	heli->veh.net_alt_target = w::to_fixed(0.5f); // servo-neutral
	heli->veh.slide_z = -200;
	const float z0 = heli->position.z;
	for (int t = 0; t < 8; ++t)
		w::aircraft_client_tick(r.world, *heli, r.traits);
	std::fprintf(stderr, "[air-sleep] dz=%.4f slide=%d air=%d\n",
	             double(heli->position.z - z0), heli->veh.slide_z,
	             int((heli->flags & w::kEntityFlagInAir) != 0));
	bool ok = expect(std::abs(heli->position.z - z0) < 0.05f,
	                 "the at-rest hull holds its Z (no downward dribble)");
	ok &= expect(heli->veh.slide_z > -60 && heli->veh.slide_z <= 0,
	             "slideDecay bleeds to rest through the sleep halving");
	// The fast-path discriminator: at z=0.5 the pads sit exactly AT the
	// contact boundary, so the FULL solve would take the airborne branch and
	// set the flag - only the sleep early-return (before flag production)
	// keeps the parked hull grounded.
	ok &= expect((heli->flags & w::kEntityFlagInAir) == 0,
	             "the sleep path returns before the airborne flag flips");
	return ok;
}

bool run_aircraft_glides_and_holds_altitude() {
	// The AIR prediction leg (CHel/cpln): a 25 m/s helicopter — far beyond the
	// chase-only sustain — glides via the tilt/aero model, and an abandoned one
	// HOLDS altitude (the record-seeded servo target never decays).
	Rig r;
	make_rig(r);
	r.traits.family = w::VehicleFamily::Helicopter;
	r.traits.acceleration = 80;      // token*4 form feeds the tilt cap (<<12)
	r.traits.turn_rate = 0x600000;
	r.traits.climb_speed = 10255;    // ~9.8 m/s vertical clamp
	r.world.env.water_z = 0;
	w::Entity *heli = r.world.registry.get(r.boat);
	if (!expect(heli != nullptr, "heli spawned")) return false;
	heli->position.z = 60.0f; // airborne (no terrain -> ground unknown, airborne)
	heli->veh.net_engine_on = true;

	const int32_t step_fx = 26214; // 0.4 u/tick = 25 m/s
	const int32_t x0 = w::to_fixed(100.0f);
	const int32_t y0 = w::to_fixed(200.0f);
	const int32_t z0 = w::to_fixed(60.0f);
	const int kWarm = 128, kMeasure = 96, kGap = 8;
	std::vector<int32_t> presented;
	for (int t = 0; t < kWarm + kMeasure; ++t) {
		if (t % kGap == 0) {
			w::Entity::VehicleMotorState &m = heli->veh;
			m.net_smooth_target[0] = x0 + step_fx * t;
			m.net_smooth_target[1] = y0;
			m.net_smooth_target[2] = z0;
			m.net_smooth_heading = 0;
			m.net_recv_speed = step_fx;
			m.net_recv_lat = 0;
			m.net_recv_steer_bam = 0;
			m.net_engine_on = true;
			m.net_interp_progress = 0;
			m.net_predicted = true;
		}
		w::aircraft_client_tick(r.world, *heli, r.traits);
		presented.push_back(w::to_fixed(heli->position.x));
	}
	int32_t max_step = 0;
	int stalled = 0;
	for (int t = kWarm; t < kWarm + kMeasure; ++t) {
		const int32_t d = std::abs(presented[t] - presented[t - 1]);
		if (d > max_step) max_step = d;
		if (d < step_fx / 4) ++stalled;
	}
	std::fprintf(stderr,
	             "[air-fast] true step=%d  max step=%d  stalled=%d/%d  alt=%f\n",
	             step_fx, max_step, stalled, kMeasure,
	             double(heli->position.z));
	bool ok = true;
	ok &= expect(max_step <= 2 * step_fx + 2048,
	             "a 25 m/s aircraft glides on the air prediction leg");
	ok &= expect(stalled * 4 <= kMeasure,
	             "the predicted aircraft keeps moving between records");
	// Altitude hold: cruising level, Z stays near the record altitude.
	ok &= expect(std::abs(heli->position.z - 60.0f) < 8.0f,
	             "the altitude servo holds near the record-seeded target");
	// Abandonment: stop records entirely; the aircraft coasts planar but HOLDS
	// altitude (the servo target never decays).
	for (int t = 0; t < 600; ++t)
		w::aircraft_client_tick(r.world, *heli, r.traits);
	std::fprintf(stderr, "[air-hover] final alt=%f  recv_speed=%d\n",
	             double(heli->position.z), heli->veh.net_recv_speed);
	ok &= expect(std::abs(heli->position.z - 60.0f) < 10.0f,
	             "an abandoned aircraft predicts to a hover at record altitude");
	return ok;
}

// CHel/cpln select Entity_UpdateAircraftPhysics directly from the mover table;
// they do not pass through the ground dispatcher's items.def physics selector.
// Pin that distinction at the production AiSystem client pass: a physicsless
// Helicopter predicts from its received registers, while an otherwise-identical
// physicsless Ground row remains inert.
bool run_physicsless_air_dispatches_directly() {
	w::World world;
	world.registry.configure_pool(1, 8);
	world.env.water_z = 0;
	w::AiSystem ai;
	world.ai = &ai;

	w::Entity air_seed;
	air_seed.kind = w::EntityKind::Item;
	air_seed.item_id = 6001;
	air_seed.position = {100.0f, 200.0f, 60.0f};
	air_seed.yaw = 90;
	air_seed.health = 3000;
	air_seed.flags |= w::kEntityFlagInAir;
	const w::EntityHandle air_h = world.registry.spawn(1, air_seed);

	w::Entity ground_seed = air_seed;
	ground_seed.item_id = 6002;
	ground_seed.position = {120.0f, 200.0f, 60.0f};
	const w::EntityHandle ground_h = world.registry.spawn(1, ground_seed);

	w::VehicleTraits air_traits;
	air_traits.physics = 0;
	air_traits.family = w::VehicleFamily::Helicopter;
	air_traits.acceleration = 80;
	air_traits.turn_rate = 0x600000;
	air_traits.climb_speed = 10255;
	world.vehicle_traits.set(6001, air_traits);
	w::VehicleTraits ground_traits;
	ground_traits.physics = 0;
	ground_traits.family = w::VehicleFamily::Ground;
	ground_traits.player_speed = 26214;
	world.vehicle_traits.set(6002, ground_traits);

	w::Entity *air = world.registry.get(air_h);
	w::Entity *ground = world.registry.get(ground_h);
	if (!expect(air != nullptr && ground != nullptr,
	            "physicsless dispatch controls spawned")) return false;
	stage(*air, w::to_fixed(100.0f), w::to_fixed(200.0f),
	      w::to_fixed(60.0f), 0, 26214, 0);
	air->veh.net_recv_lat = 4096;
	air->veh.net_engine_on = true;
	stage(*ground, w::to_fixed(120.0f), w::to_fixed(200.0f),
	      w::to_fixed(60.0f), 0, 26214, 0);
	const w::Vec3 air_start = air->position;
	const w::Vec3 ground_start = ground->position;

	w::TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = false;
	for (uint32_t tick = 0; tick < 64; ++tick) {
		ctx.logic_tick = tick;
		ai.tick(world, ctx);
	}

	const float air_motion = std::fabs(air->position.x - air_start.x) +
			std::fabs(air->position.y - air_start.y) +
			std::fabs(air->position.z - air_start.z);
	bool ok = expect(air->veh.net_interp_progress > 0 && air_motion > 0.01f,
	                 "physicsless CHel executes client prediction through AiSystem");
	ok &= expect(air->veh.cmd_speed == 26214 &&
	                     air->veh.cmd_lateral_speed == 4096,
	             "aircraft prediction mirrors both received motion commands");
	ok &= expect(ground->position.x == ground_start.x &&
	                     ground->position.y == ground_start.y &&
	                     ground->position.z == ground_start.z &&
	                     ground->veh.net_interp_progress == 0 &&
	                     ground->veh.cmd_speed == 0,
	             "physicsless Ground remains inert behind its selector");
	return ok;
}

// CHel/cpln execute the shared aircraft mover directly, but neither callback
// calls Entity_ProcessMovementSoundEffects. Keep the client dispatcher from
// manufacturing the ground/water movement lanes merely because a physicsless
// air row is now eligible for prediction. The Ground and Watercraft controls
// pin the positive side of that family boundary in the same public tick.
bool run_client_family_sound_dispatch_scope() {
	w::World world;
	world.registry.configure_pool(1, 8);
	world.env.water_z = 0;
	w::AiSystem ai;
	world.ai = &ai;

	auto spawn_vehicle = [&](int item_id, float x) {
		w::Entity seed;
		seed.kind = w::EntityKind::Item;
		seed.item_id = item_id;
		seed.position = {x, 200.0f, 60.0f};
		seed.yaw = 90;
		seed.health = 3000;
		seed.flags |= w::kEntityFlagInAir;
		return world.registry.spawn(1, seed);
	};
	const w::EntityHandle heli_h = spawn_vehicle(6101, 100.0f);
	const w::EntityHandle plane_h = spawn_vehicle(6102, 120.0f);
	const w::EntityHandle ground_h = spawn_vehicle(6103, 140.0f);
	const w::EntityHandle water_h = spawn_vehicle(6104, 160.0f);

	auto install_traits = [&](int item_id, w::VehicleFamily family,
	                          int physics, const char *idle_set) {
		w::VehicleTraits traits;
		traits.physics = physics;
		traits.family = family;
		traits.player_speed = 26214;
		traits.water_speed = 26214;
		traits.acceleration = 80;
		traits.turn_rate = 0x600000;
		traits.climb_speed = 10255;
		traits.sound_loops[0] = idle_set;
		world.vehicle_traits.set(item_id, traits);
	};
	install_traits(6101, w::VehicleFamily::Helicopter, 0, "AIR_HELI_IDLE");
	install_traits(6102, w::VehicleFamily::Plane, 0, "AIR_PLANE_IDLE");
	install_traits(6103, w::VehicleFamily::Ground, 1, "GROUND_IDLE");
	install_traits(6104, w::VehicleFamily::Watercraft, 1, "WATER_IDLE");

	w::Entity *heli = world.registry.get(heli_h);
	w::Entity *plane = world.registry.get(plane_h);
	w::Entity *ground = world.registry.get(ground_h);
	w::Entity *water = world.registry.get(water_h);
	if (!expect(heli != nullptr && plane != nullptr && ground != nullptr &&
	                    water != nullptr,
	            "client family sound controls spawned")) return false;
	for (w::Entity *vehicle : {heli, plane, ground, water}) {
		stage(*vehicle, w::to_fixed(vehicle->position.x),
		      w::to_fixed(vehicle->position.y), w::to_fixed(vehicle->position.z),
		      0, 26214, 0);
		vehicle->veh.net_engine_on = true;
	}

	w::TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = false;
	ai.tick(world, ctx);

	bool saw_ground = false;
	bool saw_water = false;
	bool saw_air = false;
	for (const w::SoundEmitterEvent &event : world.sound_emitters.pending()) {
		if (event.source_handle == ground_h.packed && event.lane == 0 &&
		    event.set_name == "GROUND_IDLE") saw_ground = true;
		if (event.source_handle == water_h.packed && event.lane == 0 &&
		    event.set_name == "WATER_IDLE") saw_water = true;
		if (event.source_handle == heli_h.packed ||
		    event.source_handle == plane_h.packed) saw_air = true;
	}
	bool ok = expect(heli->veh.net_interp_progress > 0 &&
	                         plane->veh.net_interp_progress > 0,
	                 "physicsless CHel and cpln both execute client prediction");
	ok &= expect(saw_ground && saw_water && !saw_air,
	             "only witnessed ground/water families emit movement-sound lanes");
	return ok;
}

// ---- AUTHORITY legs (the host-side cbot mover, witnessed 2026-08-06;
// vehicle-client-movers-re.md §1.12; D-NET-161) ----

w::Entity *mount_ai_driver(Rig &r) {
	w::Entity *vehicle = r.world.registry.get(r.boat);
	if (!expect(vehicle != nullptr, "authority vehicle spawned")) return nullptr;
	w::Seat seat;
	seat.type = w::SeatType::Driver;
	seat.bone_index = 7;
	seat.source_name = "drvrx00";
	vehicle->seats.push_back(seat);
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.item_id = 2072;
	body.player_class = 0; // an AI body, not a player [orig: !(Flags & 0x100)]
	body.health = 150;
	body.health_max = 150;
	body.alive = true;
	const w::EntityHandle handle = r.world.registry.spawn(0, body);
	if (!expect(w::entity_process_vehicle_attach(r.world, handle, r.boat, 7),
	            "AI driver mounted")) return nullptr;
	return r.world.registry.get(handle);
}

// Brain + a nav node 200 u dead ahead (+x; mission yaw 90 = BAM 0).
int stage_boat_brain(Rig &r, w::AiSystem &sys, int32_t out_speed) {
	const int ai_idx = sys.attach(r.boat);
	w::AiEntity &ve = *sys.at(ai_idx);
	ve.pos[0] = 100 << 16;
	ve.pos[1] = 200 << 16;
	ve.pos[2] = 10 << 16;
	ve.heading = w::bam_heading_from_mission_yaw_deg(90.0);
	sys.nav.channels.resize(3);
	sys.nav.channels[2].count = 1;
	sys.nav.channels[2].entries[0] = 0;
	sys.nav.nodes.resize(1);
	sys.nav.nodes[0] = w::NavEntry{{2 << 16, 300 << 16, 200 << 16, 10 << 16, 0}};
	w::AiBrain &b = ve.brain;
	b.f[w::AiBrain::kCurState] = 16;
	b.f[w::AiBrain::kWpType] = 1;
	b.f[w::AiBrain::kWpChannel] = 2;
	b.f[w::AiBrain::kWpNode] = 0;
	b.f[w::AiBrain::kOutSpeed] = out_speed;
	return ai_idx;
}

// An AI controller in the ctrl seat drives the boat toward its node on open
// water, holding the waterline — the 00TRg Zodiac case [orig: the AI leg
// @0x48E247..0x48E756 feeding the shared core].
bool run_authority_ai_boat_drives_afloat() {
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.traits.player_control = true;
	w::AiSystem sys;
	const int ai_idx = stage_boat_brain(r, sys, 30 * 293); // PatrolSpeed 30
	w::AiBrain &b = sys.at(ai_idx)->brain;
	w::Entity *drv = mount_ai_driver(r);
	if (drv == nullptr) return false;
	w::Entity *boat = r.world.registry.get(r.boat);
	w::Entity *ctrl = w::resolve_vehicle_controller(r.world, *boat);
	bool ok = expect(ctrl != nullptr, "AI controller resolves");

	const float x0 = boat->position.x;
	bool drove = false;
	float max_dz = 0.0f;
	for (int i = 0; i < 300; ++i) {
		w::VehicleDriveCmd cmd;
		sys.watercraft_ai_drive(r.world, *boat, ctrl, r.traits, cmd);
		drove = drove || cmd.ai_drive;
		w::tick_watercraft_motor(r.world, *boat, r.traits, &cmd);
		w::AiEntity *ve = sys.for_handle(r.boat);
		ve->pos[0] = static_cast<int32_t>(boat->position.x * 65536.0f);
		ve->pos[1] = static_cast<int32_t>(boat->position.y * 65536.0f);
		ve->pos[2] = static_cast<int32_t>(boat->position.z * 65536.0f);
		ve->heading = boat->veh.yaw_bam;
		max_dz = std::max(max_dz, std::abs(boat->position.z - 10.0f));
	}
	ok &= expect(drove, "AI leg staged a drive command");
	ok &= expect(b.f[w::AiBrain::kCurState] == 16, "state 16 held (22->16 handback)");
	ok &= expect(boat->position.x - x0 > 10.0f, "boat drove toward the node");
	ok &= expect(std::abs(boat->position.y - 200.0f) < 30.0f, "no runaway lateral drift");
	ok &= expect(max_dz < 2.0f, "hull held the waterline while driving");
	if (!ok) std::fprintf(stderr, "  (drove %.1fu, max dz %.2fu)\n",
	                      boat->position.x - x0, max_dz);
	return ok;
}

// No controller: the brain parks (state 22) and the motor holds — the parked
// leg [orig: @0x48E7EE..0x48E81E].
bool run_authority_boat_parks_without_controller() {
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.traits.player_control = true;
	w::AiSystem sys;
	const int ai_idx = stage_boat_brain(r, sys, 30 * 293);
	w::AiBrain &b = sys.at(ai_idx)->brain;
	w::Entity *boat = r.world.registry.get(r.boat);
	w::VehicleDriveCmd cmd;
	sys.watercraft_ai_drive(r.world, *boat, nullptr, r.traits, cmd);
	bool ok = expect(!cmd.ai_drive, "no drive command without a controller");
	ok &= expect(b.f[w::AiBrain::kCurState] == 22 &&
	                     b.f[w::AiBrain::kPendState] == 22,
	             "parked stamp 22 (+ the pend mirror)");
	const float x0 = boat->position.x;
	for (int i = 0; i < 60; ++i)
		w::tick_watercraft_motor(r.world, *boat, r.traits, &cmd);
	ok &= expect(boat->veh.cmd_speed == 0, "parked hold zeroes the command");
	ok &= expect(std::abs(boat->position.x - x0) < 0.5f, "parked boat stays put");
	return ok;
}

// Authority capsize: past ~100 deg of roll the hull drains 200 hp/tick; a
// DEAD-flagged hull skips the whole tick [orig: @0x48DE84..0x48DECD /
// the Flags&2 jump @0x48DDFA].
bool run_authority_capsize_drain_and_dead_skip() {
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.traits.player_control = true;
	w::Entity *boat = r.world.registry.get(r.boat);
	boat->roll = 110; // seeds air_roll_bam = 110 * 11930464 > 0x471C7180
	boat->health = 1000;
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	bool ok = expect(boat->health == 800, "capsized hull drained 200 hp");

	boat->flags |= w::kEntityFlagDead;
	const float x0 = boat->position.x;
	const int hp0 = boat->health;
	boat->veh.vel_x = 1 << 16; // would integrate if the dead skip failed
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	bool skipped = boat->health == hp0 && boat->position.x == x0;

	// The real death path latches the dead bit on engine_flags (destruction.cpp
	// Flags |= 6) — the skip reads the combined view, so it must hold there too.
	boat->flags &= ~w::kEntityFlagDead;
	boat->engine_flags |= w::kEntityFlagDead;
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	skipped = skipped && boat->health == hp0 && boat->position.x == x0;
	ok &= expect(skipped, "dead hull skips input and integration (both flag fields)");
	return ok;
}

// A PLAYER driver commands through def waterSpeed — retail cbot defs author NO
// player_speed, so the ground field would command 0 and host boats could not be
// driven — and the boat consumes MoveOrder bits 6/7 as motion overrides, not
// the ground lean flags [orig: cmd reads +0x8EC @0x48E017/@0x48E034/@0x48E088;
// bit6 @0x48E005..0x48E00E; bit7 @0x48E010..0x48E028].
bool run_authority_player_drive_uses_waterspeed() {
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.traits.player_control = true;
	r.traits.water_speed = 5000;
	r.traits.player_speed = 0; // retail-shaped: every JO cbot def omits it
	w::Entity *drv = mount_ai_driver(r);
	if (drv == nullptr) return false;
	drv->player_class = 2;   // a human driver [orig: the Flags & 0x100 class]
	drv->flags |= 0x100u;
	drv->yaw = 90; // mission yaw 90 = BAM heading 0 = +x, matching the hull
	w::Entity *boat = r.world.registry.get(r.boat);
	boat->yaw = 90;
	w::Entity *ctrl = w::resolve_vehicle_controller(r.world, *boat);
	bool ok = expect(ctrl != nullptr, "player controller resolves");

	// Forward drive: dir 1 + the move bit.
	drv->net_move_input = 0x08u | 0x01u;
	const float x0 = boat->position.x;
	for (int i = 0; i < 300; ++i)
		w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	ok &= expect(boat->veh.cmd_speed == 5000,
	             "player move commands waterSpeed, not the absent player_speed");
	ok &= expect(boat->position.x - x0 > 5.0f, "player-driven boat moves");

	// bit6 forces dir=1 AND the move bit (throttle with no explicit move key);
	// no ground lean flag may appear.
	drv->net_move_input = 0x40u;
	boat->flags &= ~0x28u;
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	ok &= expect(boat->veh.cmd_speed == 5000, "bit6 throttles at waterSpeed");
	ok &= expect((boat->flags & 0x20u) == 0, "bit6 writes no ground lean flag on a boat");

	// bit7 forces cmd = waterSpeed with dir 7, skipping the move/analog split.
	drv->net_move_input = 0x80u;
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	ok &= expect(boat->veh.cmd_speed == 5000, "bit7 forces the waterSpeed command");
	ok &= expect((boat->flags & 0x8u) == 0, "bit7 writes no ground lean flag on a boat");
	return ok;
}

// A PLAYER driver whose head is under the water plane hands the boat to the
// AI leg: the player input block is skipped, so with no AI command staged the
// hull holds its registers [orig: the submerged-driver cut @0x48DFD3..0x48DFDF
// -> the AI leg @0x48E247]. A body that never derived an eye height keeps the
// wheel.
bool run_submerged_driver_hands_to_ai_leg() {
	Rig r;
	make_rig(r);
	set_zodiac_boxes(r.traits);
	r.traits.player_control = true;
	r.traits.water_speed = 5000;
	r.traits.player_speed = 0;
	w::Entity *drv = mount_ai_driver(r);
	if (drv == nullptr) return false;
	drv->player_class = 2;
	drv->flags |= 0x100u;
	drv->yaw = 90;
	w::Entity *boat = r.world.registry.get(r.boat);
	boat->yaw = 90;
	drv->net_move_input = 0x08u | 0x01u;
	bool ok = true;
	// Eye above the plane: the player leg commands waterSpeed.
	drv->eye_offset_z = 1 << 16;
	drv->position.z = static_cast<float>(r.world.env.water_z) / 65536.0f;
	ok &= expect(!w::watercraft_driver_submerged(r.world, *drv), "eye above the plane");
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	ok &= expect(boat->veh.cmd_speed == 5000, "surfaced driver commands waterSpeed");
	// Eye at/below the plane: the cut routes to the AI leg (none staged: hold).
	boat->veh.cmd_speed = 0;
	drv->position.z -= 1.0f;
	ok &= expect(w::watercraft_driver_submerged(r.world, *drv), "eye at the plane is submerged");
	w::tick_watercraft_motor(r.world, *boat, r.traits, nullptr);
	ok &= expect(boat->veh.cmd_speed == 0, "submerged driver's input is cut");
	// No derived eye height: never cut.
	drv->eye_offset_z = 0;
	ok &= expect(!w::watercraft_driver_submerged(r.world, *drv), "no eye height -> no cut");
	return ok;
}

// The AI leg caps command at def waterSpeed and steers Yaw + delta with no
// ground-style delta/8 term [orig: @0x48E260..0x48E279 / @0x48E3F0..0x48E3F5].
bool run_authority_ai_leg_caps_at_waterspeed() {
	Rig r;
	make_rig(r);
	r.traits.player_control = true;
	r.traits.water_speed = 5000;
	w::AiSystem sys;
	const int ai_idx = stage_boat_brain(r, sys, 30000); // out_speed above the cap
	w::AiBrain &b = sys.at(ai_idx)->brain;
	b.f[w::AiBrain::kCurState] = 22; // must hand back to 16
	w::Entity *drv = mount_ai_driver(r);
	if (drv == nullptr) return false;
	w::Entity *boat = r.world.registry.get(r.boat);
	boat->veh.yaw_bam = w::bam_heading_from_mission_yaw_deg(90.0);
	boat->veh.yaw_seeded = true;
	w::Entity *ctrl = w::resolve_vehicle_controller(r.world, *boat);
	bool ok = expect(ctrl != nullptr, "controller resolves");
	w::VehicleDriveCmd cmd;
	sys.watercraft_ai_drive(r.world, *boat, ctrl, r.traits, cmd);
	ok &= expect(cmd.ai_drive, "AI leg drives");
	ok &= expect(cmd.cmd_speed == 5000, "command capped at waterSpeed");
	ok &= expect(b.f[w::AiBrain::kCurState] == 16, "22 -> 16 handback");
	// Node dead ahead + aligned heading + zero velocity: budget refresh finds
	// ~zero error, so steer == heading exactly (no delta/8 residue).
	ok &= expect(cmd.steer_target_bam == boat->veh.yaw_bam,
	             "steer = heading + delta with no delta/8 term");
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_authority_ai_boat_drives_afloat();
	ok &= run_authority_boat_parks_without_controller();
	ok &= run_authority_capsize_drain_and_dead_skip();
	ok &= run_authority_ai_leg_caps_at_waterspeed();
	ok &= run_authority_player_drive_uses_waterspeed();
	ok &= run_submerged_driver_hands_to_ai_leg();
	ok &= run_fast_boat_glides();
	ok &= run_ground_vehicle_glides();
	ok &= run_bike_family_deltas();
	ok &= run_ground_parked_rests_at_wheel_clearance();
	ok &= run_tank_parked_rests_at_wheel_clearance();
	ok &= run_bike_parked_rests_at_wheel_clearance();
	ok &= run_tank_family_deltas();
	ok &= run_platform_solve_settles_at_waterline();
	ok &= run_afloat_latch_controls_drag();
	ok &= run_first_prediction_seeds_platform_state();
	ok &= run_platform_basis_preserves_roll_sign();
	ok &= run_water_rudder_wraps_min_speed();
	ok &= run_airborne_watercraft_preserves_yaw_rate();
	ok &= run_prior_euler_drives_thrust_and_beach_stop();
	ok &= run_platform_solve_precedes_yaw();
	ok &= run_vehicle_chase_wraps_bam_seam();
	ok &= run_ground_rudder_wraps_bam_seam();
	ok &= run_ground_and_bike_local_driver_input_gate();
	ok &= run_watercraft_local_driver_reconciliation_gate();
	ok &= run_aircraft_local_pilot_input_gate();
	ok &= run_platform_roll_is_stable_and_gravity_witnessed();
	ok &= run_aircraft_glides_and_holds_altitude();
	ok &= run_aircraft_contact_lands_and_conforms();
	ok &= run_aircraft_contact_conforms_to_ramp();
	ok &= run_aircraft_contact_conforms_to_side_slope();
	ok &= run_aircraft_water_flag_hysteresis();
	ok &= run_aircraft_steep_slope_sheds_and_shoves();
	ok &= run_aircraft_sleep_fast_path();
	ok &= run_physicsless_air_dispatches_directly();
	ok &= run_client_family_sound_dispatch_scope();
	ok &= run_abandoned_boat_coasts_to_rest();
	ok &= run_steer_follows_received_register();
	if (!ok) {
		std::fprintf(stderr, "watercraft_client_motor: FAILED\n");
		return EXIT_FAILURE;
	}
	std::printf("watercraft_client_motor: OK\n");
	return EXIT_SUCCESS;
}
