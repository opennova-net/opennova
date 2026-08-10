// The mission collision/occlusion resolution sweep (ADR 0031, re-opening the
// S7b "render-coupled asset resolution" leg): registry iteration, items.def
// graphic -> collision/occlusion model resolution through the sim's own
// parse-once cache, husk/KZ/piece metadata, bound radii, and the negative
// demand cache. The embedding shell owns the state's lifetime and supplies
// the retained DefItemsFile; everything else here is engine state already
// (world::CollisionWorld / OcclusionWorld, SimModelCache, the pose provider).
#pragma once

#include <def/def.h>
#include <mission/placement_traits.h> // the visual-item policy home
#include <simassets/sim_collision_pose.h>
#include <simassets/sim_model_cache.h>
#include <world/collision.h>
#include <world/occlusion.h>
#include <world/world.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::simassets {

// items.def "Player #1, Single player" -> US01/US01.adm: the visual stand-in
// definition for the runtime player type when the database carries it.
// Canonical home: engine/runtime/mission placement_traits.h.
inline constexpr int kPlayerVisualItemId = mission::kPlayerVisualItemId;

// Last-wins lookup over duplicate definition ids — the same load-order
// overwrite the id-keyed item map exposed (see simassets item_traits).
const DefItemDef *find_item_def(const DefItemsFile &items, int item_id);

// Runtime item type -> items.def id, with the player's visual stand-in.
int visual_item_id_for_runtime_type(int item_id, const DefItemsFile &items);

// The first-husk piece-model metadata retail derives at death time (section
// count, per-section centers, section 0's z extents).
struct CollisionHuskPieceInfo {
	int32_t sections = 0;
	std::vector<world::Vec3> centers;
	float rest_min_z = 0.0f;
	float rest_max_z = 0.0f;
};

// Mission-lifetime collision graphic caches. The initial mission sweep and
// demand resolution for late-spawned players (and the joiner's wire ghosts)
// share these exact model ids; repeated sweeps only attach instances and
// never duplicate the model registry.
struct CollisionResolveState {
	std::unordered_map<std::string, int32_t> model_by_graphic;
	std::unordered_map<std::string, int32_t> occlusion_by_graphic;
	std::unordered_map<std::string, float> radius_by_graphic;
	// First-stage husk KZ points in mission-local axes. Kept independently
	// from the collision-model cache because a husk graphic may already have
	// been registered as another entity's main graphic.
	std::unordered_map<std::string, std::vector<world::Vec3>>
			husk_kz_points_by_graphic;
	std::unordered_map<std::string, CollisionHuskPieceInfo>
			husk_pieces_by_graphic;
	// Negative demand cache: one unresolved entity is attempted at most once
	// per mission unless the shell explicitly asks for another full sweep.
	std::unordered_map<uint16_t, uint64_t> resolution_attempted;

	void clear();
};

// The engine systems one resolve writes into.
struct CollisionResolveDeps {
	world::CollisionWorld &collision;
	world::OcclusionWorld &occlusion;
	SimCollisionPoseProvider &pose;
	SimModelCache &models;
};

// Register-or-look-up ONE graphic key: collision model (+ native pose
// registration), occlusion model, bound radius. The shared leg between the
// registry sweep below and the joiner's wire-ghost resolution — one
// implementation, one cache.
int32_t collision_model_for_graphic(CollisionResolveState &state,
		const CollisionResolveDeps &deps, const std::string &graphic_key);

// The full registry sweep: every non-marker entity resolves its graphic (and
// husk chain) into collision/occlusion instances, bound radius, vehicle probe
// boxes, KZ blast points, and death-piece metadata. Idempotent per spawn id.
// Returns the number of entities attached to a collision model.
int resolve_collision_instances(world::World &world, const DefItemsFile &items,
		CollisionResolveState &state, const CollisionResolveDeps &deps);

} // namespace opennova::simassets
