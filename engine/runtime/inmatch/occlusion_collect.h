#pragma once

// One view's entity collect over the view the kernel's OcclusionWorld has
// selected (ex the GDExtension's Simulation::collect_view_verdicts): the
// placed non-building entities, the decoded wire rows and the local player,
// each through the entity render gates, and the MODEL foliage tier's anchors
// the collected persons carry. The main scene's collect and the weapon Inset
// pass's both run it, each over its own frame words (world/occlusion.h
// OcclusionView); the latches it ticks are the entities' own, whichever view
// collects. The lists are that collect's alone: it empties them before it
// walks and its pass draws only what they hold, so an embedder reconciles per
// frame only the rows a view's lists hold. [orig: Terrain_CollectVisibleEntities_0
// @ 0x5c6f20 / Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60;
// Terrain_CollectVisibleEntities @ 0x5c9160 - the counts zeroed:
// g_SectorPersonCount @ 0x5c916b, g_VisibleBuildingCount @ 0x5c9171,
// g_SectorEntityCount @ 0x5c9177]
// Each pass then draws its own lists, and a drawn model's presentation writers
// run in that pass's draw (Render_SectorEntity and the bone callbacks under it):
// the main scene and the weapon Inset each re-collect and walk through the one
// scene routine, and the water mirror's reflected subscene walks its own collect's
// lists (collect_mirror_view below). A row any view the frame draws collected is
// therefore one whose writers run that frame; one every view dropped runs none.
// The model's own render-time work rides the same draw: the def's world callback
// runs only past the sub-pixel reject, and its bone callbacks pose the model
// through the entity submit, the material generators evaluating at the batch
// flush after it, so an undrawn model is neither posed nor evaluated (its clocks
// and the sim's phases run on). [orig: Render_SectorEntity @ 0x5c4190 - the
// world callback @ 0x5c4335 past the sub-pixel reject @ 0x5c42de;
// BoneCallback_gnrc_World @ 0x4e2860 - Render_SubmitEntity @ 0x4e29a8;
// CRenderBatchQueue_SortAndFlush @ 0x5dae40]
// [orig: Terrain_RenderWorldScene @ 0x5c93a0 - the collect @ 0x5c94f0, the
//  sector models @ 0x5c94f5, the sector entities @ 0x5c94fa; its callers
//  Render_ProcessMainSceneFrame @ 0x5ca8ec and Render_WeaponInsetScene
//  @ 0x5c9de9; the mirror's collect from Render_MainScene @ 0x5c16fc]

#include <runtime/inmatch/present_rows.h> // PresentRowsContext
#include <runtime/world/entity.h>
#include <runtime/world/occlusion_camera.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::inmatch {

// Each decoded wire row's persistent three-ray latch, keyed by its handle (the
// placed rows keep theirs on the entity). Owned by the embedder across frames.
using WireOcclusionLatchMap = std::unordered_map<uint16_t, uint8_t>;

// The walk for the camera `cam` (its frame words already built by
// OcclusionWorld::build_frame), in the collectors' order: the statics (the
// static prox table's tail, the pool-2 rows that are not Building-type defs, in
// slot order), then pool 0, then pool 1, each pool in slot order with its placed
// rows, decoded wire rows and the local player each at its own slot (a joiner's
// local player at its self handle), so the re-arms take the shared PRNG stream's
// draws in retail's order (occlusion_collect.cpp carries the witness).
// `r_culled_bms` takes the placed rows' dropped BMS ids, a placed pool-2 row the
// table does not list among them (the walk never admits it, so no pass draws it),
// so a placed row is admitted exactly when it is absent; `r_culled_wire` the
// dropped wire handles, `r_anchors` the MODEL foliage tier's anchors (mission
// units: the collected persons with a stance bit and no groundEntity), and
// `r_local_collected` (when given) whether the walk admitted the local player's
// body, which its presenter draws and poses by (first person then skips the
// draw alone; false with no local player). The context's server is not read
// and its materializer may be null; a null runtime collects the placed rows and
// the local player alone.
void collect_view_verdicts(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		const world::OcclusionFrameCamera &cam, std::vector<int32_t> &r_culled_bms,
		std::vector<int32_t> &r_culled_wire, std::vector<world::Vec3> &r_anchors,
		bool *r_local_collected = nullptr);

// One building row's verdict in the frame the kernel's OcclusionWorld has
// selected: its bms id and handle, the packed visible<<32 | raw section mask
// (world/occlusion_feed.h pack_building_visibility) and the def's forced
// sections the part draw ORs over the raw mask.
struct BuildingVerdict {
	int32_t bms_id = 0;
	uint16_t handle = 0;
	int64_t packed = 0;
	int64_t forced = 0;
};

// Every Building-type def row with a placed identity and an occlusion instance
// or a collision model (every retail building batch member) and its verdict in
// the selected frame, in registry order; the pool-2 decorations and foliage
// take the entity collect instead (world::building_def_row carries the split).
void collect_building_verdicts(const world::World &world, const world::CollisionWorld &collision,
		const world::OcclusionWorld &occlusion, std::vector<BuildingVerdict> &r_out);

// What a water mirror's collect leaves for its reflected subscene to draw: the
// building verdicts over its section masks, the placed rows' and the wire rows'
// drops (the complement of its entity and person lists), the MODEL foliage
// anchors its person list carries (the reflected subscene's BySide waves update
// the tiles around them), and the mask build's g_BlinkWaterVisible. The
// subscene then collects its own death pieces over the view it draws from
// (world::mirror_reflected_view; the embedder runs OcclusionWorld::
// collect_death_piece_draws there at the max-quality water detail). The lists
// are this collect's alone: each collect empties them before it walks, and the
// subscene draws only what they hold, so an embedder reconciles per frame only
// the rows a mirror's lists hold (a dropped row draws in no mirror).
// [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0 - the counts
//  zeroed: g_SectorPersonCount @ 0x5c90b7, g_VisibleBuildingCount @ 0x5c90bd,
//  g_SectorEntityCount @ 0x5c90c9]
// [orig: Water_RenderReflectedWorldScene @ 0x5c8510 - Terrain_RenderSectorModels
//  @ 0x5c8576 over the collect's batch and masks, Terrain_RenderSectorEntities
//  @ 0x5c857b, Terrain_RenderSectorEntitiesBySide @ 0x5c8590 / @ 0x5c8599 with
//  their Foliage_UpdateModelTiles calls @ 0x5c7edb..0x5c7f11; the water detail
//  test @ 0x5c85ad, DeathPiece_CollectVisible @ 0x5c85b7, DeathPiece_RenderVisible
//  @ 0x5c85c1]
struct MirrorViewLists {
	std::vector<BuildingVerdict> buildings;
	std::vector<int32_t> culled_bms;
	std::vector<int32_t> culled_wire;
	std::vector<world::Vec3> anchors;
	bool local_collected = false; // the local player's body admitted
	bool water_visible = false;
};

// The water mirror's collect ahead of a scene's (the main scene's or the weapon
// Inset's): the building walk and the section masks over its eye and square view
// (OcclusionWorld::mirror_water_visible), then over those masks the same entity walk
// as a scene's, every row through the gates under the mirror's Flags match (the
// Reflective bit while the eye is at or above the water, none below), so each
// collected row's latch ticks or re-arms on the shared PRNG stream as in the scene's
// collect; the building verdicts are read while the mirror's frame words stand.
// Returns the mask build's g_BlinkWaterVisible; the shared `r_wire_latch` is the
// scene collect's. [orig: Terrain_CollectVisibleEntitiesForReflection
// @ 0x5c90a0 - the statics' walk @ 0x5c913d, pools 0 / 1's @ 0x5c9145 / @ 0x5c914e]
bool collect_mirror_view(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		const world::OcclusionFrameCamera &cam, MirrorViewLists &r_lists);

} // namespace opennova::inmatch
