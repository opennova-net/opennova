// Asset-gated offline repro of the 05TRcoop frozen-patrol bunker pin:
// the mission routes a patrol group INSIDE "Concrete Bunker 2" (item 1359,
// graphic Cbunker2, instance at (159.9, 319.7)), and on a live host the
// clumped soldiers press against its collision forever — the resolver's
// correction cancels the walk (root step ~0.09u/tick, correction its
// negative; probe instrument 2026-08-20). Retail soldiers enter the bunker.
//
// This harness loads the REAL Cbunker2 collision through the real pipeline
// (ResourceIndex -> SimModelCache -> collision_model_from_3di), places it at
// the mission pose, and walks a capsule from the live pin position toward the
// authored node, resolving each step. It reports the volume inventory and
// whether the capsule reaches the interior. Diagnostic/report-only while the
// divergence is open; flip the verdict to an assert once the walk-in works.
// Gated on OPENNOVA_JO_DIR (skip-as-pass without a JO install).
#include "mission/bms.h"
#include "mission/promote.h"

#include "resource_index/resource_index.h"
#include "simassets/model_builders.h"
#include "simassets/sim_model_cache.h"
#include "world/ai.h"
#include "world/collision.h"
#include "world/world.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

int32_t fx(double v) { return static_cast<int32_t>(v * 65536.0); }

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (dir == nullptr || *dir == '\0') {
		std::printf("bunker walk-in: SKIP (OPENNOVA_JO_DIR not set)\n");
		return 0;
	}

	// The real model, through the real pipeline.
	opennova::ResourceIndex index;
	if (!index.scan(dir)) {
		std::printf("bunker walk-in: SKIP (resource index scan failed)\n");
		return 0;
	}
	simassets::SimModelCache cache;
	cache.set_index(&index);
	const Threedi3di3 *m3 = cache.model_for("Cbunker2");
	if (m3 == nullptr || m3->collision == nullptr) {
		std::printf("bunker walk-in: SKIP (Cbunker2.3di not found/parsed)\n");
		return 0;
	}
	w::CollisionModel model;
	if (!simassets::collision_model_from_3di(m3->collision, model,
	                                         simassets::model_has_collision(*m3))) {
		std::fprintf(stderr, "FAIL: Cbunker2 collision did not convert\n");
		return 1;
	}

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

	// The mission pose for the east-base instance (SSN-less static): promote
	// 05TRcoop and find item 1359 with x > 0.
	const std::string path = std::string(dir) + "/05TRcoop.bms";
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::printf("bunker walk-in: SKIP (no 05TRcoop.bms)\n");
		return 0;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                           std::istreambuf_iterator<char>());
	bms::File m;
	std::string error;
	if (!bms::parse(bytes.data(), bytes.size(), m, error)) {
		std::fprintf(stderr, "FAIL: 05TRcoop parse: %s\n", error.c_str());
		return 1;
	}
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	mission::promote_mission(m, world, ai, {});

	w::EntityHandle bunker{};
	world.registry.for_each([&](const w::Entity &e) {
		if (e.item_id == 1359 && e.position.x > 0.0f) bunker = e.handle;
	});
	w::Entity *be = world.registry.get(bunker);
	if (be == nullptr) {
		std::fprintf(stderr, "FAIL: east-base Cbunker2 instance not promoted\n");
		return 1;
	}
	std::printf("bunker instance: pos=(%.1f, %.1f, %.1f) yaw=%d\n",
	            be->position.x, be->position.y, be->position.z, int(be->yaw));

	// Collision world: the one instance, plus the soldier.
	w::CollisionWorld cw;
	const int32_t mid = cw.add_model(std::move(model));
	cw.assign_entity(bunker, mid, be->registry_spawn_id);

	w::Entity s;
	s.kind = w::EntityKind::Organic;
	s.net_id = 70001;
	s.alive = true;
	s.position = {158.11f, 322.44f, 34.2f}; // soldier 232's live pin position
	const w::EntityHandle soldier = world.registry.spawn(0, s);
	for (int i = 0; i < 17; ++i) cw.build_tick_tables(world);

	// Walk toward the authored node (the bunker interior) at the live root
	// step (~0.09u/tick), resolving every step like the infantry tick.
	const double nx = 159.8, ny = 319.7;
	int32_t pos[3] = {fx(158.11), fx(322.44), fx(34.2)};
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
			std::printf("  t=%d pos=(%.2f, %.2f, %.2f) dist=%.2f\n", t,
			            pos[0] / 65536.0, pos[1] / 65536.0, pos[2] / 65536.0, d);
	}
	std::printf("closest approach to the node: %.2f u\n", closest);
	if (closest < 0.5) {
		std::printf("bunker walk-in: the capsule reached the interior node\n");
	} else {
		std::printf("bunker walk-in: PINNED outside (closest %.2f u) — the live "
		            "divergence reproduces offline\n",
		            closest);
	}
	return 0;
}
