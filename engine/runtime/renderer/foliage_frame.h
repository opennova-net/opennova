#pragma once

// Portable foliage frame compilation (ADR 0033 R2). This is the seam between
// the foliage placement runtime (engine/formats/foliage — deterministic
// candidate generation, gates, LRU caches) and an embedding renderer. One
// compile turns the frame request into a typed draw list: the silhouette
// anchor gate, submission grouping, per-identity vertex builds (only for
// identities that became resident this frame), per-submission uniform state,
// and both retail wind clocks live here; the embedder keeps GPU uploads,
// draw-node pooling, and material binding.
// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
//  Foliage_GenerateModelTileInstances @ 0x600980;
//  Foliage_RenderFarPatches @ 0x60a659..0x60a694]

#include <formats/foliage/runtime.h>

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

// Orthonormal camera state plus the projection, column-major — the anchor
// gate (view depth >= the MODEL schedule floor, frustum membership) is
// compiler math, not a scene-graph query.
struct FoliageViewInput {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float cam_z = 0.0f;
	float view[16] = {};
	float proj[16] = {};
	// Anchor world positions (crouched/prone infantry on terrain); the
	// compiler applies the depth/frustum gate.
	std::vector<std::array<float, 3>> silhouette_anchors;
	std::vector<opennova::foliage::DetailCell> detail_cells;
	// True when the embedder supplies no frustum (a preview without a real
	// projection); anchors then gate on view depth alone.
	bool no_frustum = false;
};

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

// D3DFVF-shaped expansion output. Detail vertices carry the terrain gradient
// normal, the source UV, the terrain-atlas UV2, and the source-height bend
// byte; silhouette vertices carry (half_height, 0) in UV2 and zero normal /
// bend (GridPlacementVS derives its shape from the fold, already applied to
// the position here).
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
	int64_t model_mesh_hits = 0;
	int64_t model_mesh_uploads = 0;
	opennova::foliage::RuntimeStats runtime{};
};

struct FoliageDrawList {
	uint64_t frame_id = 0;
	std::vector<FoliageVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<FoliageMeshBuild> mesh_builds;
	std::vector<FoliageDrawCommand> commands;
	// Identities whose meshes the embedder must release AFTER consuming every
	// command in this draw list (a regenerated identity may have been submitted
	// earlier in the same frame).
	std::vector<opennova::foliage::CacheIdentity> detail_evicted;
	std::vector<opennova::foliage::CacheIdentity> model_evicted;
	FoliageFrameDebugCounters debug{};
};

// Deep in-process module wrapping foliage::Runtime: one call gates anchors,
// runs the placement runtime, expands vertices for identities that became
// resident this frame, forms per-submission commands with their uniform
// state, advances both wind clocks, and mirrors the runtime's eviction
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
	// The compiler's mirror of which identities the embedder holds meshes
	// for, with the built geometry's emptiness (an empty build never draws,
	// and never advances the model wind counter).
	struct ResidentMesh {
		bool empty = true;
		int64_t instances = 0;
		int64_t vertices = 0;
	};

	bool expand_detail_instance(
			const opennova::foliage::DetailInstance &instance,
			const opennova::foliage::WorldSamplers &world,
			const FoliageExpansionSamplers &expansion, size_t vertex_base);
	bool expand_silhouette_instance(
			const opennova::foliage::SilhouetteInstance &instance,
			size_t vertex_base);

	opennova::foliage::Runtime runtime_;
	std::array<opennova::foliage::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS>
			slots_{};
	std::array<FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS> geometry_{};
	std::unordered_map<MeshKey, ResidentMesh, MeshKeyHash> resident_;
	FoliageDrawList draw_list_;
	uint64_t compile_index_ = 0;
	int64_t model_wind_counter_ = 0;
};

}  // namespace opennova::renderer