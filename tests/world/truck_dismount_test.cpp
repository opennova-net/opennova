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
// infantry tick does. Diagnostic/report-only while the divergence is open, in
// the bunker_walkin_test tradition: it prints whether the capsule escapes.
// Gated on OPENNOVA_JO_DIR (skip-as-pass without a JO install).
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "resource_index/resource_index.h"
#include "simassets/collision_resolve.h"
#include "simassets/model_builders.h"
#include "simassets/sim_model_cache.h"
#include "world/collision.h"
#include "world/world.h"

namespace {

using namespace opennova;
namespace w = opennova::world;

int32_t fx(double v) { return static_cast<int32_t>(v * 65536.0); }

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (dir == nullptr || *dir == '\0') {
		std::printf("truck dismount: SKIP (OPENNOVA_JO_DIR not set)\n");
		return 0;
	}
	opennova::ResourceIndex index;
	if (!index.scan(dir)) {
		std::printf("truck dismount: SKIP (resource index scan failed)\n");
		return 0;
	}
	simassets::SimModelCache cache;
	cache.set_index(&index);
	const Threedi3di3 *m3 = cache.model_for("DTruck1");
	if (m3 == nullptr || m3->collision == nullptr) {
		std::printf("truck dismount: SKIP (DTruck1.3di not found/parsed)\n");
		return 0;
	}
	w::CollisionModel model;
	if (!simassets::collision_model_from_3di(m3->collision, model,
	                                         simassets::model_has_collision(*m3))) {
		std::fprintf(stderr, "FAIL: DTruck1 collision did not convert\n");
		return 1;
	}
	std::printf("DTruck1: %zu sections, %zu volumes\n", model.sections.size(),
	            model.volumes.size());
	std::printf("  model AABB: x=[%.2f..%.2f] y=[%.2f..%.2f] z=[%.2f..%.2f]\n",
	            model.min[0] / 65536.0, model.max[0] / 65536.0,
	            model.min[1] / 65536.0, model.max[1] / 65536.0,
	            model.min[2] / 65536.0, model.max[2] / 65536.0);

	// The authored seat USER POINTS straight off the model -- the source our
	// extraction reads. Tests whether the z=2.54 our seat table reports is the
	// model's own or something we introduce.
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
	for (size_t ui = 0; ui < m3->user_point_count; ++ui) {
		const ThreediUserPoint &up = m3->user_points[ui];
		std::string low(up.name);
		for (char &c : low) c = char(::tolower((unsigned char)c));
		if (low.rfind("sitex",0) != 0 && low.rfind("ctrlx",0) != 0 &&
		    low.rfind("drvrx",0) != 0 && low.rfind("usegun",0) != 0)
			continue;
		float p3[3];
		threedi_user_point_position(&up, p3);
		std::printf("    USRP %-10s model=(%.3f, %.3f, %.3f)\n", up.name,
		            p3[0], p3[1], p3[2]);
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
	const w::EntityHandle th = world.registry.spawn(1, truck);
	w::Entity *te = world.registry.get(th);
	if (te == nullptr) { std::fprintf(stderr, "FAIL: truck spawn\n"); return 1; }

	w::CollisionWorld cw;
	const int32_t mid = cw.add_model(std::move(model));
	cw.assign_entity(th, mid, te->registry_spawn_id);

	// A rear passenger seat, straight off the authored seat table (concept
	// 6.4p): local (1.05, -5.14, 2.54). Truck faces +y at the origin, so the
	// seat's world position is the same as its local one.
	// Default is the SEAT height, which is what reproduces the live pin. A body
	// left in the cargo bed walks into the back of the cab and never gets out;
	// the same walk started at ground level (NW_SEAT_Z=0) clears the truck in two
	// resolver ticks. That contrast IS the defect: our dismount leaves the
	// occupant standing in the bed instead of on the ground beside the vehicle.
	// The live probe's `gc` matching the AI's `y` does NOT prove they are on
	// terrain -- the bed is solid to us, so it reads as their ground.
	const char *seatz = std::getenv("NW_SEAT_Z");
	const double sx = 1.05, sy = -5.14;
	const double sz = (seatz != nullptr && seatz[0] != 0) ? std::atof(seatz) : 2.54;
	// The authored destination lies FORWARD of the truck. The live case is
	// 28-31 u ahead (ch 11 node 0 at (381.2, 406.2) from a dismount at
	// (377, 375)); 31 u forward reproduces it without the mission loaded.
	const double tx = 0.0, ty = 31.0;

	w::Entity s{};
	s.kind = w::EntityKind::Organic;
	s.net_id = 70002;
	s.alive = true;
	s.position = {static_cast<float>(sx), static_cast<float>(sy),
	              static_cast<float>(sz)};
	const w::EntityHandle soldier = world.registry.spawn(0, s);
	for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

	int32_t pos[3] = {fx(sx), fx(sy), fx(sz)};
	int32_t vel[3] = {0, 0, 0};
	int16_t health = 100;
	w::CollisionWorld::ResolveState state;
	double closest = 1e9;
	int pushed_ticks = 0;
	for (int t = 0; t < 1200; ++t) {
		const double px = pos[0] / 65536.0, py = pos[1] / 65536.0;
		const double dx = tx - px, dy = ty - py;
		const double d = std::sqrt(dx * dx + dy * dy);
		closest = std::min(closest, d);
		if (d < 0.5) break;
		// The live root step measured on the pinned AI.
		pos[0] += fx(0.09 * dx / d);
		pos[1] += fx(0.09 * dy / d);
		const int32_t before_x = pos[0], before_y = pos[1];
		cw.resolve_entity(world, soldier, state, pos, vel, vel[2], 0, fx(1.8), 0,
		                  0, /*is_player=*/false, /*is_authority=*/true, t,
		                  /*anim=*/149, 0u, health);
		if (before_x != pos[0] || before_y != pos[1]) ++pushed_ticks;
		if (w::Entity *se = world.registry.get(soldier)) {
			se->position = {static_cast<float>(pos[0] / 65536.0),
			                static_cast<float>(pos[1] / 65536.0),
			                static_cast<float>(pos[2] / 65536.0)};
		}
		// Variant: the carrier DRIVES ON after the drop (NW_TRUCK_LEAVES=<tick>).
		// The user reports retail's trucks continue past the drop point; if the
		// hull vacating is what frees the body, this run clears and the stationary
		// one does not.
		static const char *leave_at = std::getenv("NW_TRUCK_LEAVES");
		if (leave_at != nullptr && leave_at[0] != 0 && t == std::atoi(leave_at)) {
			if (w::Entity *tv = world.registry.get(th)) {
				tv->position.y += 400.0f;
				std::printf("  t=%d TRUCK DRIVES ON (carrier moved away)\n", t);
			}
			for (int k = 0; k < 17; ++k) cw.build_tick_tables(world);
		}
		if ((t % 16) == 0) cw.build_tick_tables(world);
		if ((t % 100) == 0)
			std::printf("  t=%4d pos=(%6.2f, %6.2f, %5.2f) dist=%6.2f contactItem=%d\n",
			            t, pos[0] / 65536.0, pos[1] / 65536.0, pos[2] / 65536.0, d,
			            cw.dbg_last_contact_item);
	}
	std::printf("closest approach to the forward node: %.2f u  (resolver pushed on "
	            "%d ticks)\n", closest, pushed_ticks);
	if (closest < 1.0)
		std::printf("truck dismount: the capsule walked clear of the truck\n");
	else
		std::printf("truck dismount: PINNED — the capsule never reached the node "
		            "(closest %.2f u). This is the live 00TRg defect.\n", closest);
	return 0;
}
