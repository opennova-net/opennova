#pragma once

// One view's entity collect over the view the kernel's OcclusionWorld has
// selected (ex the GDExtension's Simulation::collect_view_verdicts): the
// placed non-building entities, the decoded wire rows and the local player,
// each through the entity render gates, and the MODEL foliage tier's anchors
// the collected persons carry. The main scene's collect and the weapon Inset
// pass's both run it, each over its own frame words (world/occlusion.h
// OcclusionView); the latches it ticks are the entities' own, whichever view
// collects. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
// Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60]

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
// OcclusionWorld::build_frame): `r_culled_bms` takes the placed rows' dropped
// BMS ids, `r_culled_wire` the dropped wire handles, `r_anchors` the MODEL
// foliage tier's anchors (mission units: the collected persons with a stance
// bit and no groundEntity); `r_handles` is the caller's reused scratch. The
// context's server is not read and its materializer may be null; a null
// runtime collects the placed rows and the local player alone.
void collect_view_verdicts(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		std::vector<world::EntityHandle> &r_handles, const world::OcclusionFrameCamera &cam,
		std::vector<int32_t> &r_culled_bms, std::vector<int32_t> &r_culled_wire,
		std::vector<world::Vec3> &r_anchors);

} // namespace opennova::inmatch
