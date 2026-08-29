// Asset-gated offline repro of the 05TRcoop frozen-patrol bunker pin:
// the mission routes a patrol group INSIDE "Concrete Bunker 2" (item 1359,
// graphic Cbunker2, instance at (159.9, 319.7)), and on a live host the
// clumped soldiers press against its collision forever — the resolver's
// correction cancels the walk (root step ~0.09u/tick, correction its
// negative; probe instrument 2026-08-20). Retail soldiers enter the bunker.
//
// This harness boots 05TRcoop through the engine's own mission kernel (ADR
// 0042 d3) - the REAL Cbunker2 collision attached at the mission pose by the
// same resolve_collision_instances the shipping game runs, every static in
// its neighbourhood with it - and walks a capsule from the live pin position
// toward the authored node, resolving each step against the kernel's
// collision world. It reports the volume inventory and whether the capsule
// reaches the interior. The pipeline preconditions are asserted (the model
// converts, the instance promotes and attaches, the ground probe finds the
// slab); the walk-in verdict itself stays a printed diagnostic while the
// divergence is open - flip it to an assert once the walk-in works.
// Gated on OPENNOVA_JO_DIR (reports Skipped without a JO install).
#include <formats/mission/bms.h>

#include <runtime/simassets/model_builders.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/collision_detail.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/retail_mission_files.h"
#include "common/retail_mission_open.h"
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

// `--from x,y[,z]`, `--to x,y`, `--column x,y`: replay any live pin's mission
// coordinates without a rebuild (docs/divergence-ledger.md D-COL rows). The
// ctest registration passes none and runs the historical spawn probe.
static const char *arg_value(int argc, char **argv, const char *flag) {
	for (int i = 1; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
	return nullptr;
}

static void parse_point(const char *text, double *x, double *y, double *z) {
	if (text == nullptr) return;
	double px = *x, py = *y, pz = z != nullptr ? *z : 0.0;
	const int n = std::sscanf(text, "%lf,%lf,%lf", &px, &py, &pz);
	if (n >= 2) { *x = px; *y = py; }
	if (n >= 3 && z != nullptr) *z = pz;
}

int main(int argc, char **argv) {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying Cbunker2)");

	// The mission pose for the east-base instance (SSN-less static) and every
	// static around it, through the kernel's own boot: promote, then the
	// collision resolve over the sim's model cache. No WAC, no player: the
	// scene under the capsule stays exactly as authored.
	testrig::RetailMissionRig rig;
	std::string error, served_by;
	if (!retail::open_mission(rig, install, "05TRcoop.bms", error, served_by))
		return retail::skip("05TRcoop.bms on the OPENNOVA_JO_DIR mount (base or an expansion) "
		                    "or loose under OPENNOVA_JO_ASSETS");
	testrig::BootOptions options;
	options.playable = false;
	options.wac = false;
	options.listen_server = false;
	if (!expect(rig.boot(options, error), "05TRcoop boots through the mission kernel")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	w::World &world = rig.world;
	w::CollisionWorld &cw = rig.collision;
	simassets::SimModelCache &cache = rig.models;

	// The real model, through the real pipeline.
	const Threedi3di3 *m3 = cache.model_for("Cbunker2");
	if (m3 == nullptr || m3->collision == nullptr) {
		return retail::skip("Cbunker2.3di on the OPENNOVA_JO_DIR mount");
	}
	w::CollisionModel model;
	if (!expect(simassets::collision_model_from_3di(m3->collision, model,
	                                                simassets::model_has_collision(*m3)),
	            "Cbunker2 collision converts")) {
		return 1;
	}
	expect(!model.sections.empty() && !model.volumes.empty(),
	       "Cbunker2 carries collision sections and volumes");

	// Volume inventory: what the resolver walks (type counts + z spans per
	// section) — the entrance question is whether a gap exists at floor level.
	std::printf("Cbunker2: %zu sections, %zu volumes\n", model.sections.size(),
	            model.volumes.size());
	for (size_t si = 0; si < model.sections.size(); ++si) {
		const w::CollisionSection &sec = model.sections[si];
		std::printf("  section %zu: volumes=%d z=[%.2f..%.2f] x=[%.2f..%.2f] "
		            "y=[%.2f..%.2f]\n",
		            si, sec.volume_count, sec.min_z / 65536.0, sec.max_z / 65536.0,
		            sec.min_x / 65536.0, sec.max_x / 65536.0, sec.min_y / 65536.0,
		            sec.max_y / 65536.0);
		for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
			const w::CollisionVolume &vol = model.volumes[sec.volume_start + vi];
			std::printf("    vol %d: type=%d planes=%d\n", vi, vol.type,
			            vol.plane_count);
		}
	}

	// The east-base instance (SSN-less static): item 1359 with x > 0.
	w::EntityHandle bunker{};
	world.registry.for_each([&](const w::Entity &e) {
		if (e.item_id == 1359 && e.position.x > 0.0f) bunker = e.handle;
	});
	w::Entity *be = world.registry.get(bunker);
	if (!expect(be != nullptr, "the east-base Cbunker2 instance promoted")) return 1;
	std::printf("bunker instance: pos=(%.1f, %.1f, %.1f) yaw=%d\n",
	            be->position.x, be->position.y, be->position.z, int(be->yaw));
	// The kernel's collision resolve attached the bunker and every static
	// around it (towers etc.) through the same pipeline the game runs.
	expect(rig.collision_attached > 0, "the collision instances attached");
	{
		int nearby = 0;
		for (const w::CollisionWorld::DebugInstance &inst :
				rig.collision_instances(be->position, 40.0f, 256)) {
			if (inst.handle == bunker) continue;
			++nearby;
		}
		std::printf("collision instances: %d attached, %d within 40 u of the bunker\n",
		            rig.collision_attached, nearby);
	}
	expect(!rig.collision_instances(be->position, 1.0f, 8).empty(),
	       "the bunker itself carries an attached collision instance");

	w::Entity s;
	s.kind = w::EntityKind::Organic;
	s.net_id = 70001;
	s.alive = true;
	// Endpoints are argv-overridable so any live pin can be replayed without a
	// rebuild: --from x,y[,z] and --to x,y (mission coordinates).
	double from_x = 159.7, from_y = 323.8, from_z = 35.1;
	parse_point(arg_value(argc, argv, "--from"), &from_x, &from_y, &from_z);
	s.position = {static_cast<float>(from_x), static_cast<float>(from_y),
	              static_cast<float>(from_z)};
	const w::EntityHandle soldier = world.registry.spawn(0, s);
	if (!expect(soldier.valid(), "a pool-0 slot for the walking capsule")) return 1;
	for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

	// Walk toward the authored node (the bunker interior) at the live root
	// step (~0.09u/tick), resolving every step like the infantry tick.
	double nx = 159.8, ny = 319.7;
	parse_point(arg_value(argc, argv, "--to"), &nx, &ny, nullptr);
	int32_t pos[3] = {fx(from_x), fx(from_y), fx(from_z)};
	int32_t vel[3] = {0, 0, 0};
	int16_t health = 100;
	w::CollisionWorld::ResolveState state;
	double closest = 1e9;
	for (int t = 0; t < 1200; ++t) {
		const double px = pos[0] / 65536.0, py = pos[1] / 65536.0;
		const double dx = nx - px, dy = ny - py;
		const double d = std::sqrt(dx * dx + dy * dy);
		closest = std::min(closest, d);
		if (d < 0.5) break;
		pos[0] += fx(0.09 * dx / d);
		pos[1] += fx(0.09 * dy / d);
		cw.resolve_entity(world, soldier, state, pos, vel, vel[2], 0, fx(1.8), 0,
		                  0, /*is_player=*/false, /*is_authority=*/true, t,
		                  /*anim=*/149, 0u, health);
		if (w::Entity *se = world.registry.get(soldier)) {
			se->position = {static_cast<float>(pos[0] / 65536.0),
			                static_cast<float>(pos[1] / 65536.0),
			                static_cast<float>(pos[2] / 65536.0)};
		}
		if ((t % 16) == 0) cw.build_tick_tables(world);
		if ((t % 200) == 0)
			std::printf("  t=%d pos=(%.2f, %.2f, %.2f) dist=%.2f contactItem=%d\n",
			            t, pos[0] / 65536.0, pos[1] / 65536.0, pos[2] / 65536.0, d,
			            cw.dbg_last_contact_item);
	}
	// ---- Column dump: which type-1 volumes contain the spawn column's local
	// XY, and where do their tops sit in WORLD z? (Yaw-only rotation leaves z
	// untouched, so world top = bunker z + local max_z.) Uses the exact
	// target_view matrix path (bam heading from mission yaw).
	{
		const Threedi3di3 *bm = cache.model_for("Cbunker2");
		w::CollisionModel cm;
		simassets::collision_model_from_3di(bm->collision, cm,
		                                    simassets::model_has_collision(*bm));
		int32_t bp[3] = {fx(be->position.x), fx(be->position.y),
		                 fx(be->position.z)};
		const int32_t heading = w::bam_heading_from_mission_yaw_deg(
				static_cast<double>(be->yaw));
		const w::CollisionMatrix mat =
				w::collision_matrix_from_heading(heading, bp);
		w::CollisionMatrix inv;
		mat.invert_into(inv);
		// Column to inspect: --column x,y (defaults to the historical spawn probe).
		double colx = 155.3, coly = 318.2;
		parse_point(arg_value(argc, argv, "--column"), &colx, &coly, nullptr);
		const int32_t col[3] = {fx(colx), fx(coly), fx(35.1)};
		std::printf("column (%.1f, %.1f):\n", colx, coly);
		int32_t local[3];
		w::detail::transform_translate_then_rotate(inv.m, col, local);
		std::printf("spawn column local=(%.2f, %.2f, %.2f)\n", local[0] / 65536.0,
		            local[1] / 65536.0, local[2] / 65536.0);
		for (size_t si = 0; si < cm.sections.size(); ++si) {
			const w::CollisionSection &sec = cm.sections[si];
			for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
				const w::CollisionVolume &vol = cm.volumes[sec.volume_start + vi];
				if (local[0] < vol.min_x || local[0] > vol.max_x ||
				    local[1] < vol.min_y || local[1] > vol.max_y)
					continue;
				std::printf("  s%zu v%d type=%d localZ=[%.2f..%.2f] "
				            "worldZ=[%.2f..%.2f]\n",
				            si, vi, vol.type, vol.min_z / 65536.0,
				            vol.max_z / 65536.0,
				            be->position.z + vol.min_z / 65536.0,
				            be->position.z + vol.max_z / 65536.0);
			}
		}
	}

	// ---- Raw BVOL dump at the column: does the RAW 3DI carry a volume whose
	// top sits at local ~-7.66 (world 35.1) that the conversion drops?
	{
		const int32_t lx = fx(4.64), ly = fx(1.46); // the column, model-local
		std::printf("raw BVOLs containing the column XY (of %zu total):\n",
		            m3->collision->volume_count);
		for (size_t i = 0; i < m3->collision->volume_count; ++i) {
			const ThreediBoundingVolume &v = m3->collision->volumes[i];
			if (lx < v.min_x_fp16 || lx > v.max_x_fp16 || ly < v.min_y_fp16 ||
			    ly > v.max_y_fp16)
				continue;
			std::printf("  raw[%zu] type=%d flags=0x%x localZ=[%.2f..%.2f] "
			            "planes=%d\n",
			            i, v.collidable_type, unsigned(v.flags),
			            v.min_z_fp16 / 65536.0, v.max_z_fp16 / 65536.0,
			            v.plane_count);
		}
	}

	// ---- Direct ground-probe check: from just above retail's interior spawn
	// point, does raycast_ground find the bunker's floor slab (retail z 35.1)?
	{
		w::Entity rp_seed;
		rp_seed.kind = w::EntityKind::Organic;
		rp_seed.net_id = 72001;
		rp_seed.alive = true;
		rp_seed.position = {155.3f, 318.2f, 36.0f};
		const w::EntityHandle rp = world.registry.spawn(0, rp_seed);
		for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);
		const int32_t probe_pos[3] = {fx(155.3), fx(318.2), fx(36.0)};
		w::EntityHandle hit;
		const int32_t g = cw.raycast_ground(world, rp, probe_pos, 0, 0, 0,
		                                    fx(4.0), &hit);
		std::printf("ground probe at (155.3, 318.2, 36.0): ground=%.2f hit=%u "
		            "(expect ~35.1 on the slab)\n",
		            g / 65536.0, unsigned(hit.packed));
		expect(g != INT32_MIN, "the ground probe finds support under the interior spawn");
	}

	// ---- Yaw-convention sweep: at which collision yaw is retail's interior
	// spawn point (155.3, 318.2, 35.1) FREE SPACE? The mission authors placed
	// soldiers there; if only a different yaw frees it, the static-collision
	// heading conversion is the divergence (render uses its own basis, so a
	// flipped collision rotation is visually invisible).
	for (const int16_t try_yaw : {int16_t(-90), int16_t(90), int16_t(0),
	                              int16_t(180)}) {
		w::Entity *bmut = world.registry.get(bunker);
		const int16_t saved_yaw = bmut->yaw;
		bmut->yaw = try_yaw;
		w::CollisionWorld cwy;
		{
			const Threedi3di3 *bm = cache.model_for("Cbunker2");
			w::CollisionModel cm;
			simassets::collision_model_from_3di(bm->collision, cm,
			                                    simassets::model_has_collision(*bm));
			cwy.assign_entity(bunker, cwy.add_model(std::move(cm)),
			                  bmut->registry_spawn_id);
		}
		w::Entity probe_seed;
		probe_seed.kind = w::EntityKind::Organic;
		probe_seed.net_id = 71000 + try_yaw;
		probe_seed.alive = true;
		probe_seed.position = {155.3f, 318.2f, 35.1f};
		const w::EntityHandle probe = world.registry.spawn(0, probe_seed);
		for (int i = 0; i < 17; ++i) cwy.build_tick_tables(world);
		int32_t ppos[3] = {fx(155.3), fx(318.2), fx(35.1)};
		int32_t pvel[3] = {0, 0, 0};
		int16_t phealth = 100;
		w::CollisionWorld::ResolveState pstate;
		for (int t = 0; t < 30; ++t)
			cwy.resolve_entity(world, probe, pstate, ppos, pvel, pvel[2], 0,
			                   fx(1.8), 0, 0, false, true, t, 149, 0u, phealth);
		std::printf("  yaw %d: probe moved to (%.2f, %.2f, %.2f) drift=%.2f "
		            "contactItem=%d\n",
		            int(try_yaw), ppos[0] / 65536.0, ppos[1] / 65536.0,
		            ppos[2] / 65536.0,
		            std::sqrt(std::pow(ppos[0] / 65536.0 - 155.3, 2) +
		                      std::pow(ppos[1] / 65536.0 - 318.2, 2)),
		            cwy.dbg_last_contact_item);
		bmut->yaw = saved_yaw;
	}

	// ---- Spawn-settle probe: drop a capsule at retail's spawn point INSIDE
	// the bunker (slot 1: 155.3, 318.2, z 35.1 on the floor slab) with the
	// infantry gravity step, resolving each tick. Case A mirrors the live
	// boot: the first 16 logic ticks run WITHOUT candidate slices (the
	// documented cadence), so the resolver sees terrain only. Case B has
	// slices from tick 0. Retail keeps the soldier ON the slab (z 35.1).
	for (int with_slices = 0; with_slices <= 1; ++with_slices) {
		w::Entity d;
		d.kind = w::EntityKind::Organic;
		d.net_id = 70002 + with_slices;
		d.alive = true;
		d.position = {155.3f, 318.2f, 35.1f};
		const w::EntityHandle drop = world.registry.spawn(0, d);
		w::CollisionWorld cw2;
		// Re-register the scene in a fresh collision world so the slice
		// cadence restarts (17 builds arm the slices).
		{
			const Threedi3di3 *bm = cache.model_for("Cbunker2");
			w::CollisionModel cm;
			simassets::collision_model_from_3di(bm->collision, cm,
			                                    simassets::model_has_collision(*bm));
			cw2.assign_entity(bunker, cw2.add_model(std::move(cm)),
			                  be->registry_spawn_id);
		}
		const int prebuilds = with_slices ? 17 : 1;
		for (int i = 0; i < prebuilds; ++i) cw2.build_tick_tables(world);
		int32_t dpos[3] = {fx(155.3), fx(318.2), fx(35.1)};
		int32_t dvel[3] = {0, 0, 0};
		int16_t dhealth = 100;
		w::CollisionWorld::ResolveState dstate;
		for (int t = 0; t < 120; ++t) {
			dvel[2] -= 416; // kGravityStep
			if (dvel[2] < -33280) dvel[2] = -33280;
			dpos[2] += 2 * dvel[2];
			const int32_t fc = cw2.resolve_entity(
					world, drop, dstate, dpos, dvel, dvel[2], 0, fx(1.8), 0, 0,
					false, true, t, 149, 0u, dhealth);
			if (fc <= 0) { dpos[2] -= fc; dvel[2] = 0; }
			cw2.build_tick_tables(world);
			if (t == 15 || t == 60 || t == 119)
				std::printf("  settle[%s] t=%d pos=(%.2f, %.2f, %.2f)\n",
				            with_slices ? "slices@0" : "sliceless16", t,
				            dpos[0] / 65536.0, dpos[1] / 65536.0,
				            dpos[2] / 65536.0);
		}
	}

	std::printf("closest approach to the node: %.2f u\n", closest);
	if (closest < 0.5) {
		std::printf("bunker walk-in: the capsule reached the interior node\n");
	} else {
		std::printf("bunker walk-in: PINNED outside (closest %.2f u) — the live "
		            "divergence reproduces offline\n",
		            closest);
	}
	if (failures == 0) std::printf("bunker walk-in: the pipeline preconditions hold\n");
	return failures ? 1 : 0;
}
