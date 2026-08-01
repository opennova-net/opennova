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
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void make_rig(Rig &r) {
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
		r.traits.box_y_lo = -(1 << 16); r.traits.box_y_hi = 1 << 16;
		r.traits.box_x_lo = -(5 << 15); r.traits.box_x_hi = 5 << 15;
		r.traits.box_z_lo = -(1 << 15); r.traits.box_z_hi = 1 << 16;
		r.traits.foot_x_lo = r.traits.box_x_lo; r.traits.foot_x_hi = r.traits.box_x_hi;
		r.traits.foot_y_lo = r.traits.box_y_lo; r.traits.foot_y_hi = r.traits.box_y_hi;
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
