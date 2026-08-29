// Asset-gated offline repro of the 00TRg transport-unload pin.
//
// On a live host (user session probe, 2026-08-22) 13 of 22 AI that dismount a
// "Drivable Transport Truck" (item 101294, graphic DTruck1) never get more than
// ~1 u away: post-dismount path 1.0-1.8 u, and the resolver pushes them ~0.09 u
// EVERY tick forever (probe cdx/cdy = inf.dbg_res_dx/dy) while the AI that get
// clear take exactly 0. The two groups are identical in every AI-layer respect
// (anim 149 at move_mode 3), so the translation is being cancelled in collision.
//
// The geometry, read from the mission and the item (concept 6.4p): the six
// passenger seats sit in the REAR cargo bed at local y -2.03..-5.14, x +-1.05,
// z 2.54, and the authored destination (ch 11 node 0) lies FORWARD of the
// truck. A dismounted body therefore has to walk the vehicle's whole length,
// through it. EntityCommands::dismount does not reposition the occupant, and
// neither does retail (@0x4355f0 / @0x4359d0, both read).
//
// This harness loads the REAL DTruck1 collision through the real pipeline
// (ResourceIndex -> SimModelCache -> collision_model_from_3di), places the
// truck at the origin facing +y, drops a capsule at a rear passenger seat and
// walks it forward at the live root step, resolving every tick exactly as the
// infantry tick does. The pipeline preconditions are asserted (the model
// converts, its six rear passenger user points read off the model, all six
// bodies mount and dismount through the real seat machinery); the escape
// verdict itself stays a printed diagnostic while the divergence is open.
// Gated on OPENNOVA_JO_DIR (reports Skipped without a JO install).
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/model_builders.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>
#include "common/retail_paths.h"

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

int32_t fx(double v) { return static_cast<int32_t>(v * 65536.0); }

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying the truck models)");
	const char *dir = install.c_str();
	opennova::ResourceIndex index;
	if (!index.scan(dir)) return retail::skip("a scannable resource index under OPENNOVA_JO_DIR");
	simassets::SimModelCache cache;
	cache.set_index(&index);
	const Threedi3di3 *m3 = cache.model_for("DTruck1");
	if (m3 == nullptr || m3->collision == nullptr)
		return retail::skip("DTruck1.3di (with collision) on the OPENNOVA_JO_DIR mount");
	w::CollisionModel model;
	if (!expect(simassets::collision_model_from_3di(m3->collision, model,
	                                                simassets::model_has_collision(*m3)),
	            "DTruck1 collision converts")) {
		return 1;
	}
	expect(!model.sections.empty() && !model.volumes.empty(),
	       "DTruck1 carries collision sections and volumes");
	std::printf("DTruck1: %zu sections, %zu volumes\n", model.sections.size(),
	            model.volumes.size());
	std::printf("  model AABB: x=[%.2f..%.2f] y=[%.2f..%.2f] z=[%.2f..%.2f]\n",
	            model.min[0] / 65536.0, model.max[0] / 65536.0,
	            model.min[1] / 65536.0, model.max[1] / 65536.0,
	            model.min[2] / 65536.0, model.max[2] / 65536.0);

	// The authored seat USER POINTS straight off the model -- the source our
	// extraction reads. Tests whether the z=2.54 our seat table reports is the
	// model's own or something we introduce.
	// SECTION BOUNDS: where the hull sits relative to the model ORIGIN. If the
	// height range runs ~0..+3 the origin is the vehicle BASE; if it straddles
	// zero it is the CENTRE. That decides whether the live truck at z=13.06
	// against ground 15.0 is sunk or normal.
	for (size_t si = 0; si < model.sections.size(); ++si) {
		const w::CollisionSection &sc = model.sections[si];
		std::printf("    SEC %zu x=[%.2f..%.2f] y=[%.2f..%.2f] z=[%.2f..%.2f] off=(%.2f,%.2f,%.2f)\n",
		            si, sc.min_x/65536.0, sc.max_x/65536.0, sc.min_y/65536.0,
		            sc.max_y/65536.0, sc.min_z/65536.0, sc.max_z/65536.0,
		            sc.offset[0]/65536.0, sc.offset[1]/65536.0, sc.offset[2]/65536.0);
	}
	// Volume TYPES: our ground probe accepts terrain or a type-1 CB solid as
	// standing support, so whether the bed is type 1 decides if a dismounted
	// body stands on the truck or falls through to terrain.
	{
		int t1 = 0, other = 0;
		for (size_t vi = 0; vi < model.volumes.size(); ++vi) {
			const int vt = int(model.volumes[vi].type);
			std::printf("    VOL %zu type=%d\n", vi, vt);
			if (vt == 1) ++t1; else ++other;
		}
		std::printf("    -> type1=%d other=%d\n", t1, other);
	}
	std::printf("  DTruck1 user points: %zu\n", m3->user_point_count);
	int passenger_points = 0;
	for (size_t ui = 0; ui < m3->user_point_count; ++ui) {
		const ThreediUserPoint &up = m3->user_points[ui];
		std::string low(up.name);
		for (char &c : low) c = char(::tolower((unsigned char)c));
		if (low.rfind("sitex",0) != 0 && low.rfind("ctrlx",0) != 0 &&
		    low.rfind("drvrx",0) != 0 && low.rfind("usegun",0) != 0)
			continue;
		if (low.rfind("sitex", 0) == 0) ++passenger_points;
		float p3[3];
		threedi_user_point_position(&up, p3);
		std::printf("    USRP %-10s model=(%.3f, %.3f, %.3f)\n", up.name,
		            p3[0], p3[1], p3[2]);
	}
	// The six rear passenger seats the hand-built truck below carries are the
	// model's own `sitex` user points (the source the seat extraction reads).
	expect(passenger_points >= 6, "DTruck1 authors at least six sitex passenger points");

	// A SOLDIER's real bound radius, against the 0x10000 (1.0 u) that D-COL-3
	// hardcodes into the peer-repulsion threshold. Eindo11 is the 00TRg
	// passenger model (item 102086, graphic Eindo11).
	if (const Threedi3di3 *sm = cache.model_for("Eindo11")) {
		if (sm->collision != nullptr) {
			w::CollisionModel smodel;
			if (simassets::collision_model_from_3di(sm->collision, smodel,
			        simassets::model_has_collision(*sm))) {
				std::printf("  Eindo11 fallback_bound_radius = %.3f u  (hardcoded 1.000)\n",
				            double(smodel.fallback_bound_radius_q16) / 65536.0);
			}
		} else {
			std::printf("  Eindo11 has NO collision block\n");
		}
	} else {
		std::printf("  Eindo11 model not found\n");
	}

	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);

	w::Entity truck{};
	truck.kind = w::EntityKind::Item;
	truck.item_id = 1294;
	truck.net_id = 4098;
	truck.alive = true;
	truck.position = {0.0f, 0.0f, 0.0f};
	truck.yaw = 0;
	// Seats, from the authored user points. The real pipeline fills these via
	// simassets::extract_item_seat_specs; this harness builds the truck by hand,
	// so without them mount() finds no seat and the dismount path is never
	// exercised (which silently made an earlier run of this test meaningless).
	{
		const double sx6[6] = {-1.068, 1.054, -1.068, 1.054, -1.068, 1.054};
		const double sy6[6] = {-2.039, -5.124, -3.569, -3.595, -5.138, -2.026};
		for (int k = 0; k < 6; ++k) {
			w::Seat st{};
			st.type = w::SeatType::Passenger;
			st.retail_slot = uint8_t(k);
			st.seat_local = {static_cast<float>(sx6[k]), static_cast<float>(sy6[k]),
			                 2.539f};
			truck.seats.push_back(st);
		}
	}
	const w::EntityHandle th = world.registry.spawn(1, truck);
	w::Entity *te = world.registry.get(th);
	if (te == nullptr) { std::fprintf(stderr, "FAIL: truck spawn\n"); return 1; }

	// A flat synthetic height field at z=0 so the dismount ground-drop has a
	// ground to find. Raw16 = height_units * 256, so 0 == height 0.
	std::vector<uint16_t> heightmap(64 * 64, 0);
	opennova::terrain::TerrainHeightField hf;
	hf.heightmap = heightmap.data();
	hf.dim = 64;
	world.terrain = &hf;

	{
		int32_t probe[3] = {fx(-1.07), fx(-2.04), fx(2.54)};
		const w::GroundClearance gcz{};
		const int32_t g = w::calc_average_ground_height(hf, probe, 0x50000, gcz);
		std::printf("  synthetic ground sample at (-1.07,-2.04): %s\n",
		            g == INT32_MIN ? "NO COVERAGE" : std::to_string(g / 65536.0).c_str());
	}
	w::CollisionWorld cw;
	const int32_t mid = cw.add_model(std::move(model));
	cw.assign_entity(th, mid, te->registry_spawn_id);

	// A rear passenger seat, straight off the authored seat table (concept
	// 6.4p): local (1.05, -5.14, 2.54). Truck faces +y at the origin, so the
	// seat's world position is the same as its local one.
	// The SEAT height is what reproduces the live pin. A body left in the cargo
	// bed walks into the back of the cab and never gets out; the same walk
	// started at ground level clears the truck in two resolver ticks. That
	// contrast IS the defect: our dismount leaves the occupant standing in the
	// bed instead of on the ground beside the vehicle. The live probe's `gc`
	// matching the AI's `y` does NOT prove they are on terrain -- the bed is
	// solid to us, so it reads as their ground.
	const double sx = 1.05, sy = -5.14;
	const double sz = 2.54;
	// The authored destination lies FORWARD of the truck. The live case is
	// 28-31 u ahead (ch 11 node 0 at (381.2, 406.2) from a dismount at
	// (377, 375)); 31 u forward reproduces it without the mission loaded.
	const double tx = 0.0;
	const double ty = 31.0;

	// SIX bodies, one per rear passenger seat -- the live failure is a PILE-UP,
	// not a single body against a hull. The user session shows all six dismounted
	// passengers collapsing onto ONE identical coordinate (371.41, -342.30) and
	// locking, while the one AI that dismounted 8 u clear of the pile walked away.
	// A single-capsule harness cannot show that. Flat ground: the pin is the
	// pile-up, not a slope case.
	const double slope = 0.0;
	const int body_count = 6;
	struct Body { w::EntityHandle h; int32_t pos[3]; int32_t vel[3]; bool done; };
	std::vector<Body> bodies;
	const double seat_x[6] = {-1.068, 1.054, -1.068, 1.054, -1.068, 1.054};
	const double seat_y[6] = {-2.039, -5.124, -3.569, -3.595, -5.138, -2.026};
	for (int bi = 0; bi < body_count && bi < 6; ++bi) {
		w::Entity s{};
		s.kind = w::EntityKind::Organic;
		s.net_id = uint16_t(70002 + bi);
		s.alive = true;
		s.position = {static_cast<float>(seat_x[bi]),
		              static_cast<float>(seat_y[bi]), static_cast<float>(sz)};
		Body b{};
		b.h = world.registry.spawn(0, s);
		b.pos[0] = fx(seat_x[bi]); b.pos[1] = fx(seat_y[bi]); b.pos[2] = fx(sz);
		b.done = false;
		bodies.push_back(b);
	}
	// Mount ALL first, then dismount ALL: mounting and dismounting one at a time
	// frees seat 0 each round, so every body would take the same seat and land on
	// the same spot -- which is not the live case (six distinct seats).
	{
		int mounted_n = 0, dropped_n = 0;
		for (size_t bi = 0; bi < bodies.size(); ++bi)
			if (world.commands.mount(uint16_t(70002 + bi), truck.net_id)) ++mounted_n;
		for (size_t bi = 0; bi < bodies.size(); ++bi)
			if (world.commands.dismount(uint16_t(70002 + bi))) ++dropped_n;
		std::printf("  mounted %d, dismounted %d\n", mounted_n, dropped_n);
		expect(mounted_n == body_count, "every body takes a distinct rear seat");
		expect(dropped_n == body_count, "every body dismounts through the real seat machinery");
		for (size_t bi = 0; bi < bodies.size(); ++bi) {
			if (const w::Entity *se = world.registry.get(bodies[bi].h)) {
				bodies[bi].pos[0] = fx(se->position.x);
				bodies[bi].pos[1] = fx(se->position.y);
				bodies[bi].pos[2] = fx(se->position.z);
				std::printf("    body %zu at (%.2f,%.2f,%.2f)\n", bi,
				            se->position.x, se->position.y, se->position.z);
			}
		}
	}
	for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

	int16_t health = 100;
	w::CollisionWorld::ResolveState state;
	double closest = 1e9;
	int pushed_ticks = 0;
	for (int t = 0; t < 1200; ++t) {
		for (Body &b : bodies) {
			if (b.done) continue;
			const double px = b.pos[0] / 65536.0, py = b.pos[1] / 65536.0;
			const double dx = tx - px, dy = ty - py;
			const double d = std::sqrt(dx * dx + dy * dy);
			closest = std::min(closest, d);
			if (d < 0.5) { b.done = true; continue; }
			b.pos[0] += fx(0.09 * dx / d);
			b.pos[1] += fx(0.09 * dy / d);
			const int32_t bx = b.pos[0], by = b.pos[1];
			cw.resolve_entity(world, b.h, state, b.pos, b.vel, b.vel[2], 0, fx(1.8),
			                  0, 0, /*is_player=*/false, /*is_authority=*/true, t,
			                  /*anim=*/149, 0u, health);
			if (bx != b.pos[0] || by != b.pos[1]) ++pushed_ticks;
			// SYNTHETIC ground ramp (`slope` = units of rise per unit of +y; flat here).
			// NOT a parity model -- the repro has no real terrain, and the live
			// drop site IS sloped: bms 17's ground reads 12.9 -> 14.4 -> 16.6 ->
			// 18.7 -> 23.5 -> 26.6 as it walks away, while the pinned six sit at a
			// flat 15.0. This only tests whether a slope beside the truck is the
			// missing ingredient.
			if (slope != 0.0) {
				const double gy = b.pos[1] / 65536.0;
				b.pos[2] = fx(slope * gy);
			}
			if (w::Entity *se = world.registry.get(b.h))
				se->position = {static_cast<float>(b.pos[0] / 65536.0),
				                static_cast<float>(b.pos[1] / 65536.0),
				                static_cast<float>(b.pos[2] / 65536.0)};
		}
		if ((t % 16) == 0) cw.build_tick_tables(world);
		if ((t % 300) == 0) {
			std::printf("  t=%4d", t);
			for (const Body &b : bodies)
				std::printf("  (%6.2f,%6.2f)", b.pos[0] / 65536.0, b.pos[1] / 65536.0);
			std::printf("  contactItem=%d\n", cw.dbg_last_contact_item);
		}
	}
	int escaped = 0;
	for (const Body &b : bodies) if (b.done) ++escaped;
	std::printf("bodies=%d escaped=%d  (resolver pushed on %d body-ticks)\n",
	            int(bodies.size()), escaped, pushed_ticks);
	std::printf("closest approach to the forward node: %.2f u  (resolver pushed on "
	            "%d ticks)\n", closest, pushed_ticks);
	if (closest < 1.0)
		std::printf("truck dismount: the capsule walked clear of the truck\n");
	else
		std::printf("truck dismount: PINNED — the capsule never reached the node "
		            "(closest %.2f u). This is the live 00TRg defect.\n", closest);
	if (failures == 0) std::printf("truck dismount: the pipeline preconditions hold\n");
	return failures ? 1 : 0;
}
