#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <formats/foliage/foliage.h>

namespace opennova::foliage {

// Portable render intent for both retail foliage tiers. Callers provide the
// visible terrain cells / sector anchors and world samplers; the module hides
// deterministic candidate generation, key decoding, quadrant walks, and the
// eight-sample silhouette ground fit.
//
// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
// Foliage_GenerateModelTileInstances @ 0x600980;
// Foliage_UpdateModelTiles @ 0x601f50]

struct Point2 {
	float x = 0.0f;
	float z = 0.0f;
};

struct GroundCorner {
	float x = 0.0f;
	float z = 0.0f;
	float height = 0.0f;
};

struct RuntimeSlot {
	bool enabled = false;
	uint8_t attrib_flags = 0;
	// max(halfExtentX, halfExtentZ); the silhouette footprint is 0.75x this.
	float model_radius = 1.0f;
	// Retail sizes the persistent detail-cell pool from the complete source
	// model vertex count. One is a compatibility default for bindings that do
	// not yet provide mesh metadata and therefore selects the retail max (128).
	uint32_t source_vertex_count = 1;
};

// The MODEL tier accepts at most 21 instances per tile: the accepted count
// at the cache entry's +0x554 against 21 ends the candidate walk; the
// GridPlacementVS blocks c[12+4i] fill the vs_1_1 file to c95 at 21.
// [orig: Foliage_GenerateModelTileInstances @ 0x600e75]
inline constexpr int kModelTileInstanceCap = 21;

struct DetailCell {
	// Exact retail packed cell key. It is both the deterministic seed and the
	// encoded 16-unit cell origin: HIGH15 = X-min, LOW15 = Z-min on the Godot
	// plane (retail's -camera_y sector axis); bit 31 marks a flat sector. Its
	// key consumes a cache slot but generates empty geometry.
	uint32_t key = 0;
	float camera_distance = 0.0f;
	// The collected leaf's maximum terrain height: the detail passes split
	// patches by it against the water height.
	float max_height = 0.0f;
	// The cell's minimum in the 1024 terrain source atlas (the leaf node's
	// integer x/z: the routed quadrant origin plus the sector-local offset).
	// Retail's key halves carry it in their low ten bits and the generator
	// samples the detail foliage map there, not at the world position.
	// [orig: quadtree_node_init_recursive @ 0x6082fc..0x608302 (node x/z);
	// Terrain_CollectNearFoliagePatches @ 0x603f69..0x603f8a (key);
	// generate_foliage_instances_0 @ 0x5ffddb..0x5ffdee (& 0x3FF)]
	int32_t atlas_x = 0;
	int32_t atlas_z = 0;
};

struct SilhouetteAnchor {
	Point2 position{};
	float view_depth = 0.0f;
	float camera_distance = 0.0f;
	// The anchor's BySide wave: true when the entity lies on the water side
	// away from the camera (its masks precede everything the far-side wave
	// and the later passes draw).
	bool far_side = false;
};

struct FrameRequest {
	std::array<RuntimeSlot, FOLIAGE_MAX_DEFS> slots{};
	std::vector<DetailCell> detail_cells;
	std::vector<SilhouetteAnchor> silhouette_anchors;
	// The local player's thermal view (the scene core's fourth argument):
	// every detail patch draws the primary LOW pass at one tenth fade.
	bool thermal_view = false;
	// Env_WaterHeightFixed in world units and the camera's side of it: the
	// two detail passes split patches by their maximum height against it.
	float water_height = 0.0f;
	bool camera_below_water = false;
};

struct WorldSamplers {
	// Detail grass samples the flat, 1024-wrapped foliage map at the
	// candidate's ATLAS position (DetailCell::atlas_x/atlas_z plus the local
	// offsets; z on the Godot plane). The recovered
	// Terrain_GetSurfaceTypeAtFixedPoint name is a misnomer; its backing
	// buffer is the authored foliage map remapped to definition slots.
	std::function<uint32_t(int32_t atlas_x_fixed, int32_t atlas_z_fixed)>
	    detail_foliage_mask_at;
	// MODEL silhouettes use the sector-routed foliage-map sampler before the
	// same definition-slot bit gate.
	std::function<uint32_t(int32_t world_x_fixed, int32_t world_z_fixed)>
	    model_foliage_mask_at;
	std::function<float(float world_x, float world_z)> height_at;
	std::function<bool(float world_x, float world_z, float range)> path_blocked;
};

enum class DetailPass : uint8_t {
	HighAlphaTest,
	LowAlphaTest,
};

// The detail pass a patch draws in: retail's Foliage_RenderFarPatchesPass
// formatType 0 (the water side away from the camera, before the water
// surface) or 1 (the camera's side, after the camera-side entity wave).
enum class DetailWaterPass : uint8_t {
	FarSide = 0,
	CameraSide = 1,
};

struct DetailInstance {
	uint8_t slot = 0;
	uint8_t candidate = 0;
	uint32_t cell_key = 0;
	// Stable while the resident cell geometry is reused. Changes when the
	// keyed slot is allocated/replaced so bindings can retain a built mesh.
	uint64_t cache_revision = 0;
	// Shared by every instance in one draw submission; always distinct for
	// repeated submissions of the same resident key.
	uint64_t submission_id = 0;
	Point2 center{};
	float yaw_radians = 0.0f;
	float alpha = 0.0f;
	uint8_t alpha_reference = 0;
	DetailPass pass = DetailPass::HighAlphaTest;
	// True only for the near (<33) LOW resubmission that follows the HIGH
	// pass. Retail draws it at the same c6 fade under strict D3DCMP_LESS, so
	// it only lands where the HIGH pass rejected alpha; bindings emulate the
	// equality rule by discarding texels above the HIGH reference.
	// [orig: Foliage_RenderFarPatches @ 0x60a659..0x60a694;
	// Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912]
	bool near_secondary = false;
	DetailWaterPass water_pass = DetailWaterPass::CameraSide;
};

struct SilhouetteInstance {
	uint8_t slot = 0;
	uint8_t candidate = 0;
	uint8_t quadrant = 0;
	uint32_t cell_key = 0;
	// Stable until a miss or geometry-changing phase refresh; an unchanged
	// phase refresh preserves the resident revision.
	uint64_t cache_revision = 0;
	// Identifies this draw submission independently of cached geometry.
	uint64_t submission_id = 0;
	Point2 center{};
	float yaw_radians = 0.0f;
	std::array<GroundCorner, 4> corners{};
	// Corners use bit order (A+, B+): 0=--, 1=+-, 2=-+, 3=++.
	// Fold is (E_A, T_A, E_B, T_B), consumed directly by GridPlacementVS.
	std::array<float, 4> fold{};
	// Eight-sample fitted ground height at normalized model center.
	float center_height = 0.0f;
	// The rotated footprint offsets (A', B') of the four corners before the
	// key bases are added: the instance block GridPlacementVS reads carries
	// these locals, and the constant upload adds keyHi / -keyLo in float.
	std::array<float, 4> corner_local_a{};
	std::array<float, 4> corner_local_b{};
	uint8_t alpha_reference = 0;
	// The anchor's BySide wave (SilhouetteAnchor::far_side).
	bool far_side = false;
};

struct CacheIdentity {
	uint8_t slot = 0;
	uint32_t key = 0;
	uint64_t revision = 0;
};

struct FrameOutput {
	// Stable detail order: input cell, slot, pass, then candidate. Near cells
	// submit HighAlphaTest before the identity-matched LowAlphaTest pass.
	// Silhouettes additionally walk quadrants (+x,+z), (-x,+z), (+x,-z), (-x,-z).
	std::vector<DetailInstance> detail;
	std::vector<SilhouetteInstance> silhouettes;
	// Generated identities become resident. Evicted identities may still be
	// referenced by submissions in this FrameOutput; release them only after
	// consuming the complete frame.
	std::vector<CacheIdentity> detail_generated;
	std::vector<CacheIdentity> detail_evicted;
	std::vector<CacheIdentity> model_generated;
	std::vector<CacheIdentity> model_evicted;
};

struct CacheStats {
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t regenerations = 0;
	uint64_t evictions = 0;
	uint64_t residents = 0;
	// Number of actual resident draw submissions, not generated instances.
	// A near detail resident counts twice: ordered high then low.
	uint64_t submissions = 0;
};

struct RuntimeStats {
	CacheStats detail{};
	CacheStats model{};
	// Deterministic work counter for MODEL resident-key lookup. One step is one
	// candidate cache entry examined (or one indexed lookup once ported).
	uint64_t model_key_lookup_steps = 0;
	uint32_t terrain_scene_counter = 0;
};

struct FdMipChain {
	int width = 0;
	int height = 0;
	// Retail levels include mip zero and stop at a minimum dimension of four.
	// The packed payload continues through 2x2/1x1 for Godot Image parity.
	int retail_level_count = 0;
	std::vector<uint8_t> rgba;
};

class Runtime {
public:
	FrameOutput render_frame(const FrameRequest &request,
	                         const WorldSamplers &world);

	// Clears both persistent tier caches and restarts the retail scene phase.
	void reset();
	const RuntimeStats &get_stats() const { return stats_; }

	// [orig: terrain_tile_init_buffers @ 0x5ff920]
	// Up to 128 cells fit while n * (36 * source vertices) < 0xffff.
	static size_t detail_cache_capacity(uint32_t source_vertex_count);

	// Build retail's complete ":fd" chain for power-of-two inputs whose
	// dimensions are at least four. Mip zero preserves source RGB and
	// replaces only alpha with the wrapped 3x3 filter. Lower retail levels blend
	// progressively toward 0x808080 while always retaining base-chain alpha.
	// The result is packed base-to-1x1 for direct Godot Image::set_data use.
	// Invalid input leaves r_chain untouched.
	// [orig: Foliage_LoadDefAssets @ 0x601260]
	bool build_fd_rgba_mip_chain(const uint8_t *pixels, int width, int height,
	                             FdMipChain &r_chain) const;

private:
	static constexpr size_t kModelCacheCapacity = 1000;

	struct DetailCacheEntry {
		bool valid = false;
		uint32_t key = 0;
		uint32_t last_use = 0;
		uint64_t revision = 0;
		std::vector<DetailInstance> instances;
	};

	struct ModelCacheEntry {
		bool valid = false;
		uint32_t key = 0;
		uint32_t last_use = 0;
		uint64_t revision = 0;
		std::vector<SilhouetteInstance> instances;
	};

	std::array<std::vector<DetailCacheEntry>, FOLIAGE_MAX_DEFS> detail_cache_{};
	std::array<std::array<ModelCacheEntry, kModelCacheCapacity>,
	           FOLIAGE_MAX_DEFS> model_cache_{};
	std::array<std::unordered_map<uint32_t, size_t>, FOLIAGE_MAX_DEFS>
	    model_cache_index_{};
	std::array<RuntimeSlot, FOLIAGE_MAX_DEFS> cached_slots_{};
	std::array<bool, FOLIAGE_MAX_DEFS> slot_configured_{};
	uint32_t terrain_scene_counter_ = 0;
	uint64_t next_cache_revision_ = 0;
	uint64_t next_submission_id_ = 0;
	RuntimeStats stats_{};
};

} // namespace opennova::foliage
