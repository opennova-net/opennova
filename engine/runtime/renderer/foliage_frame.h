#pragma once

// Portable foliage frame compilation (ADR 0033 R2). This is the seam between
// the foliage placement runtime (engine/formats/foliage — deterministic
// candidate generation, gates, LRU caches) and an embedding renderer. One
// compile turns the frame request into a typed draw list: the silhouette
// anchor gate, submission grouping, per-identity vertex builds (only for
// identities that became resident this frame), per-submission uniform state,
// and both retail wind clocks live here; the embedder keeps GPU uploads,
// retained scenario-instance RID pooling, and material binding.
// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
//  Foliage_GenerateModelTileInstances @ 0x600980;
//  Foliage_RenderDetailPatches @ 0x60a659..0x60a694]

#include <formats/foliage/runtime.h>
#include <runtime/renderer/render_order.h>

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace opennova::renderer {

// One definition slot's source geometry, extracted once at configure time
// (the embedder flattens its mesh resource; no live mesh dependency here).
struct FoliageSourceVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct FoliageSlotGeometry {
	std::vector<FoliageSourceVertex> vertices;
	std::vector<int32_t> indices;
	float center_x = 0.0f;
	float center_z = 0.0f;
	float radius = 0.0f;
	bool valid = false;
};

// The world queries vertex expansion needs beyond foliage::WorldSamplers:
// the terrain-atlas UV projection for detail UV2. Returns false when the
// world position has no atlas mapping (UV2 falls back to zero).
struct FoliageExpansionSamplers {
	std::function<bool(float world_x, float world_z, float &r_u, float &r_v)>
			terrain_uv_at;
};

// Orthonormal camera state, column-major view matrix — the MODEL tier's own
// view-depth gate is compiler math, not a scene-graph query.
struct FoliageViewInput {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float cam_z = 0.0f;
	float view[16] = {};
	// Anchor world positions: the visible, crouched/prone, terrain-standing
	// person entities the embedder's occlusion frame admitted (retail's
	// BySide list); the compiler applies the MODEL walk's own view-depth
	// floor and each anchor's water side.
	std::vector<std::array<float, 3>> silhouette_anchors;
	std::vector<opennova::foliage::DetailCell> detail_cells;
	// Env_WaterHeightFixed in world units: the detail passes and the BySide
	// waves split by it.
	float water_height = 0.0f;
	// The detail tier's sway phase inputs: the wall clock in milliseconds
	// (retail GetTickCount) and the weather oscillator's Env_WaveOscRing[0]
	// [orig: Foliage_SetupVertexShaderConstants @ 0x60074a..0x60076f].
	uint32_t time_ms = 0;
	int32_t wind_osc_ring0 = 0;
	// The local player's thermal view (foliage::FrameRequest::thermal_view).
	bool thermal_view = false;
};

// The detail tier's c24.x sway phase: the ms clock x 0.003 plus the weather
// oscillator's ring slot 0 / 655360 (`fild` the GetTickCount word, `fmul`
// flt_7DE9D4 = 0.003; `fild Env_WaveOscRing`, `fmul` flt_7DE9D0 =
// 0x35CCCCCD = 1/655360; `faddp`), uploaded as c24 = (phase, 1, 0, -) with
// c25 = (0.03, ...) (flt_7C9B90) for Foliage_WindSwayVS:
// `mad r0.w, v0.x, c24.y, c24.x` (v0.x = the pre-wind vertex's render x,
// the Godot Z relative to the patch's sector origin) -> polynomial sine ->
// `mad r1.z, sin*bend, c25.x, v0.z` (render z = Godot X).
// [orig: Foliage_SetupVertexShaderConstants @ 0x60074a..0x6007b4;
// Foliage_WindSwayVS literal @ 0x7de648]. The clock term folds modulo 2 pi
// so a long session keeps the sine's float precision — the sine is
// periodic, nothing observable moves.
float foliage_detail_wind_phase(uint32_t time_ms, int32_t wind_osc_ring0);

// The Godot-Z origin of a detail patch's D3D world translation: the 512-unit
// sector of the cell's Z-min (the collector stores FB20 << 9 per patch and
// the draw translates by it), so the sway's v0.x is sector-local.
float foliage_detail_wind_sector_origin_z(uint32_t cell_key);

// GridPlacementVS's c9.x wind term for one MODEL draw: sin(angle) x 0.08
// (the double fmul by 0.079999998), angle = the pre-incremented draw
// counter x 0.001. [orig: Foliage_UploadModelTileVSConstants
// @ 0x60108e..0x6010cf]
float foliage_model_wind_offset(double angle);

// The camera's side of the water: retail compares the camera z against
// Env_WaterHeightFixed with setnl (camera >= water is above).
// [orig: Terrain_RenderWorldScene @ 0x5c93a1..0x5c93b0]
bool foliage_camera_above_water(float camera_y, float water_height);

// The BySide wave a person entity rides: entity z - 1.0 below the water is
// the below-water wave, drawn first (the far wave) while the camera is at or
// above the water; the other wave is the camera side. The MODEL masks drawn
// inside a wave and every person the wave queues share this side.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dd2 (z - 0x10000),
// @ 0x5c7dfd..0x5c7e18 (side); Terrain_RenderWorldScene
// @ 0x5c953e..0x5c955f (far wave first)]
bool foliage_entity_far_side(float entity_y, float camera_y, float water_height);

// Where each foliage draw sits in the transparent ladder (render_order.h).
// The detail passes take their own rungs [orig: Foliage_RenderDetailPatchesPass
// (0) @ 0x5c95c5, (1) @ 0x5c9665]. The MODEL depth masks are immediate draws
// inside the BySide entity waves: the far wave's inside BySide(far, 0)
// @ 0x5c955f, before the far-side alpha flush @ 0x5c9596; the camera wave's
// inside BySide(camera, 0) @ 0x5c9638, after the water pass @ 0x5c95dc and
// before Scar_DrawBatches @ 0x5c9658 [orig: Terrain_RenderWorldScene].
// They lead the first rung drawn after them, sorted ahead of everything in
// it by a sorting offset beyond any view depth.
inline constexpr int kFoliageMaskFarSideRung = kRungAlphaFarSide;
inline constexpr int kFoliageMaskCameraSideRung = kRungScars;
inline constexpr float kFoliageMaskSortingOffset = -1.0e6f;
// The near secondary LOW draw follows its HIGH draw of the same geometry
// [orig: Foliage_RenderDetailPatches: the primary draw @ 0x60a653, the
// secondary @ 0x60a659..0x60a694]; an embedder that
// depth-sorts within a rung needs the pair's equal depths ordered, so the
// secondary sorts a hair nearer.
inline constexpr float kFoliageSecondaryLowSortingOffset = 1.0e-3f;

enum class FoliageTier : uint8_t {
	Detail = 0,
	Silhouette = 1,
};

// One resident mesh to (re)build this frame: a slice of the draw list's vertex/
// index arrays. Identities not listed are already resident with the embedder
// from earlier draw lists. Index values are relative to first_vertex.
struct FoliageMeshBuild {
	FoliageTier tier = FoliageTier::Detail;
	uint8_t slot = 0;
	uint32_t cell_key = 0;
	uint64_t revision = 0;
	uint32_t first_vertex = 0;
	uint32_t vertex_count = 0;
	uint32_t first_index = 0;
	uint32_t index_count = 0;
	int32_t instance_count = 0;
};

// D3DFVF-shaped detail expansion output: the terrain gradient normal, the
// source UV, the terrain-atlas UV2, and the source-height bend byte.
struct FoliageVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float nx = 0.0f;
	float ny = 0.0f;
	float nz = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	float u2 = 0.0f;
	float v2 = 0.0f;
	float bend = 0.0f; // source-height bend byte / 255
};

// One draw submission, in draw-list order (the runtime's stable submission
// order). The referenced identity is either built this frame (mesh_builds)
// or resident with the embedder from an earlier draw_list.
struct FoliageDrawCommand {
	FoliageTier tier = FoliageTier::Detail;
	uint8_t slot = 0;
	uint32_t cell_key = 0;
	uint64_t revision = 0;
	uint64_t submission_id = 0;
	// Detail only: the alpha-test pass and the near-secondary LOW rule.
	opennova::foliage::DetailPass pass =
			opennova::foliage::DetailPass::HighAlphaTest;
	bool near_secondary = false;
	float fade = 1.0f;
	float alpha_reference = 0.0f;   // 0..1
	float high_pass_cutoff = 0.0f;  // 180/255 on the near-secondary LOW draw
	float wind_phase = 0.0f;        // per-tier retail clock, resolved here
	// Detail only: foliage_detail_wind_sector_origin_z of the cell.
	float wind_sector_origin_z = 0.0f;
	// The water side the draw belongs to: detail pass formatType 0 / the
	// far-side BySide wave's masks (true), else the camera side.
	bool far_side = false;
	// The transparent-ladder rung and the depth-sort bias inside it.
	int render_rung = 0;
	float sorting_offset = 0.0f;
	// MODEL only: the submission's GridPlacementVS instance blocks
	// (draw_list.model_instances[first_instance .. + instance_count]), the
	// c9.x wind term sin(counter * 0.001) * 0.08 of this draw, and a
	// conservative world AABB of the placed geometry.
	uint32_t first_instance = 0;
	uint32_t instance_count = 0;
	float wind_offset = 0.0f;
	float aabb_min[3] = {};
	float aabb_max[3] = {};
};

// One definition slot's MODEL-tier source mesh, normalized the way
// Foliage_FillInstancedModelBuffers writes the instanced VB: x and z mapped
// over the bound square to [0,1] (retail source x, i.e. the imported x
// negated), y halved; GridPlacementVS places it per instance.
// [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20..0x5ffae7]
struct FoliageModelVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct FoliageSlotModelMesh {
	std::vector<FoliageModelVertex> vertices;
	std::vector<uint32_t> indices;
	float max_half_height = 0.0f;
	float min_half_height = 0.0f;
	bool valid = false;
};

// One GridPlacementVS instance block, the four constant rows c[12+4i] ..
// c[15+4i] exactly as Foliage_UploadModelTileVSConstants uploads them:
// rows[0..3] render x (Godot Z) of the four corners, rows[4..7] their
// ground heights, rows[8..11] render z (Godot X), rows[12..15] the fold
// (E_A, T_A, E_B, T_B). [orig: Foliage_UploadModelTileVSConstants
// @ 0x6010e6..0x601209]
struct FoliageModelInstance {
	float rows[16] = {};
};

struct FoliageFrameDebugCounters {
	uint64_t compile_index = 0;
	int64_t detail_cells = 0;
	int64_t silhouette_anchors_input = 0;
	int64_t silhouette_anchors_visible = 0;
	int64_t runtime_detail_intents = 0;
	int64_t runtime_silhouette_intents = 0;
	int64_t detail_high_instances = 0;
	int64_t detail_low_instances = 0;
	int64_t silhouette_instances = 0;
	int64_t detail_vertices = 0;
	int64_t silhouette_vertices = 0;
	int64_t render_batches = 0;
	int64_t detail_mesh_hits = 0;
	int64_t detail_mesh_uploads = 0;
	opennova::foliage::RuntimeStats runtime{};
};

struct FoliageDrawList {
	uint64_t frame_id = 0;
	std::vector<FoliageVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<FoliageMeshBuild> mesh_builds;
	std::vector<FoliageDrawCommand> commands;
	// The MODEL commands' instance blocks, in command order.
	std::vector<FoliageModelInstance> model_instances;
	// Identities whose meshes the embedder must release AFTER consuming every
	// command in this draw list (a regenerated identity may have been submitted
	// earlier in the same frame).
	std::vector<opennova::foliage::CacheIdentity> detail_evicted;
	FoliageFrameDebugCounters debug{};
};

// Deep in-process module wrapping foliage::Runtime: one call gates anchors,
// runs the placement runtime, expands detail vertices for identities that
// became resident this frame, packs the MODEL tier's GridPlacementVS
// instance blocks, forms per-submission commands with their uniform state,
// advances both wind clocks, and mirrors the runtime's detail eviction
// lifecycle. The returned draw list remains valid until the next compile call.
class FoliageFrameCompiler {
public:
	// The MODEL-tier view-depth floor for silhouette anchors — the near bound
	// of the distant-model schedule [orig: the >= 38 tier split,
	// docs/foliage/foliage-re.md §two tiers].
	static constexpr float kSilhouetteDepthGate = 38.0f;

	void configure_slots(
			const std::array<opennova::foliage::RuntimeSlot,
					opennova::FOLIAGE_MAX_DEFS> &slots,
			const std::array<FoliageSlotGeometry,
					opennova::FOLIAGE_MAX_DEFS> &geometry);

	const FoliageDrawList &compile(const FoliageViewInput &view,
			const opennova::foliage::WorldSamplers &world,
			const FoliageExpansionSamplers &expansion);

	// Clears the placement runtime, the resident-identity mirror, and both
	// wind clocks (the embedder clears its mesh cache alongside).
	void reset();

	const FoliageDrawList &last_draw_list() const { return draw_list_; }
	// The slot's normalized MODEL mesh (rebuilt by configure_slots; the
	// generation moves with every configure).
	const FoliageSlotModelMesh &model_mesh(int slot) const {
		return model_meshes_[static_cast<size_t>(slot)];
	}
	uint64_t model_mesh_generation() const { return model_mesh_generation_; }

private:
	struct MeshKey {
		uint8_t tier = 0;
		uint8_t slot = 0;
		uint32_t key = 0;
		uint64_t revision = 0;
		bool operator==(const MeshKey &o) const {
			return tier == o.tier && slot == o.slot && key == o.key &&
					revision == o.revision;
		}
	};
	struct MeshKeyHash {
		size_t operator()(const MeshKey &k) const noexcept {
			size_t h = std::hash<uint64_t>{}(k.revision);
			h ^= std::hash<uint32_t>{}(k.key) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
			h ^= std::hash<uint16_t>{}(
						 static_cast<uint16_t>((k.tier << 8) | k.slot)) +
					0x9e3779b9u + (h << 6u) + (h >> 2u);
			return h;
		}
	};
	// The compiler's mirror of which detail identities the embedder holds
	// meshes for, with the built geometry's emptiness (an empty build never
	// draws).
	struct ResidentMesh {
		bool empty = true;
		int64_t instances = 0;
		int64_t vertices = 0;
	};

	bool expand_detail_instance(
			const opennova::foliage::DetailInstance &instance,
			const opennova::foliage::WorldSamplers &world,
			const FoliageExpansionSamplers &expansion, size_t vertex_base);

	opennova::foliage::Runtime runtime_;
	std::array<opennova::foliage::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS>
			slots_{};
	std::array<FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS> geometry_{};
	std::array<FoliageSlotModelMesh, opennova::FOLIAGE_MAX_DEFS> model_meshes_{};
	uint64_t model_mesh_generation_ = 0;
	std::unordered_map<MeshKey, ResidentMesh, MeshKeyHash> resident_;
	FoliageDrawList draw_list_;
	uint64_t compile_index_ = 0;
	int64_t model_wind_counter_ = 0;
};

}  // namespace opennova::renderer
