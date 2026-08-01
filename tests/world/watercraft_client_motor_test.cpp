// The joiner-side watercraft prediction (net-re §5.38e B-facet, D-NET-196):
// chase + register-mirror + thrust/drag/keel prediction between subrate wire
// records [orig: Entity_UpdateWatercraftPhysics @0x48D480, client-executed
// subset]. The payoff contract: a remote boat at 15.6 m/s — ABOVE the
// chase-only sustain limit (~12.5 m/s), where the chase alone snap-cycles —
// glides with bounded per-tick steps because the mirrored speed/steer
// registers drive local physics between records. Plus the stale-record
// coast-down (an abandoned boat predicts to a stop).

#include "terrain/height_field.h"
#include "world/vehicle_motor.h"
#include "world/angle.h"
#include "world/geom.h"
#include "world/vehicle_attach.h"
#include "world/world.h"

#include <io/bam.h>

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

// Mimic the sim's staging edge (mirror_client_view_mission_entities): a fresh
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


bool run_platform_solve_settles_at_waterline() {
	// The platform solve (D-NET-196 residual, water leg): with model boxes
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
		ok &= expect(local_vehicle->veh.cmd_speed == local.traits.player_speed,
		             "local ground/bike driver uses current player command, not wire mirror");
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
	local.traits.player_speed = 12000;
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
	remote.traits.player_speed = 12000;
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
	wrapped.traits.player_speed = std::numeric_limits<int32_t>::max();
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

} // namespace

int main() {
	bool ok = true;
	ok &= run_fast_boat_glides();
	ok &= run_ground_vehicle_glides();
	ok &= run_bike_family_deltas();
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
	ok &= run_aircraft_glides_and_holds_altitude();
	ok &= run_abandoned_boat_coasts_to_rest();
	ok &= run_steer_follows_received_register();
	if (!ok) {
		std::fprintf(stderr, "watercraft_client_motor: FAILED\n");
		return EXIT_FAILURE;
	}
	std::printf("watercraft_client_motor: OK\n");
	return EXIT_SUCCESS;
}
