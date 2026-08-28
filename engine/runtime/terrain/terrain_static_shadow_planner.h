#pragma once

// Portable static-shadow page planner: owns the caster snapshot (admission +
// collector candidates), the per-page receiver height minimum, page-job
// compilation with the config-stamped content identity, draw classification,
// and the raster-input build. The Godot adapter is reduced to marshalling
// placer records, decoding alpha textures, and converting diagnostics.
// Plans are memoized per page under a state epoch: an unchanged epoch makes
// plan() a lookup (no caster walk) and rasterize() reuse the compiled job.
// The caster snapshot is an immutable set shared by pointer, so copying a
// planner (the adapter's worker snapshot) shares the casters and copies only
// the per-instance memo caches; replace_casters publishes a fresh set.
// Material animation is sampled from the planner's tick only when a page is
// classified or rasterized — exactly retail, which evaluates the tile models'
// materials inside the tile render, never per frame for resident tiles.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250; PROJSHAD submit
// @0x60D960..0x60D97D; see docs/terrain/terrain-re.md]

#include <runtime/terrain_query/height_field.h>

#include <runtime/terrain/terrain_static_shadow.h>
#include <runtime/terrain/terrain_static_shadow_alpha.h>
#include <runtime/terrain/terrain_static_shadow_geometry.h>
#include <runtime/terrain/terrain_static_shadow_raster.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_light_epoch.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::terrain {

struct TerrainStaticShadowPlannerCaster {
	int32_t bms_id = 0;
	int32_t entity_kind = 0;
	int32_t entity_index = 0;
	int32_t team = -1;
	// Retail's signed 96-slot global CTRL value bus as captured for this
	// submission. Static sector casters publish TEX_TEAM; every other slot is
	// retained explicitly so controlled material inputs can be wired without
	// changing the planner contract.
	opennova::renderer::ControlRegisterValues control_values{};
	uint32_t entity_attrib = 0;
	uint32_t item_attrib = 0;
	uint32_t item_attrib2 = 0;
	bool active = false;
	// The authored graphic name, carried for unsupported-draw attribution.
	std::string graphic;
	// Presentation-world transform, rows-major: rows[0][0..2], rows[1][0..2],
	// rows[2][0..2], origin[0..2].
	std::array<float, 12> world_transform{};
	std::shared_ptr<const TerrainStaticShadowResolvedGeometry> geometry;
	// Terrain height under the caster origin; retail subtracts it before
	// rendering the selected shadow LOD.
	float ground_y = 0.0f;
	// Opaque stable identity of the source document (the adapter passes its
	// instance id); participates in the candidate transform revision.
	uint64_t caster_identity = 0;
};

struct TerrainStaticShadowPagePlanResult {
	bool valid = false;
	bool raster_required = false;
	TerrainTileContentStamp content{};
};

struct TerrainStaticShadowUnsupportedAttribution {
	TerrainTilePageKey page{};
	int32_t bms_id = 0;
	uint64_t caster_key = 0;
	std::string graphic;
	uint8_t lod_index = 0;
	uint16_t render_object_index = 0;
	int material_index = -1;
	uint32_t issues = kTerrainStaticShadowUnsupportedNone;
};

struct TerrainStaticShadowPlannerDiagnostics {
	uint64_t frame_plan_count = 0;
	uint64_t frame_plan_failures = 0;
	uint64_t frame_plan_compiles = 0;
	uint64_t frame_pages_with_draws = 0;
	uint64_t frame_projection_draws = 0;
	uint64_t frame_raster_count = 0;
	uint64_t frame_triangles = 0;
	uint64_t frame_alpha_test_triangles = 0;
	std::array<uint64_t, 4> frame_blend_triangles{};
	uint64_t frame_unsupported_draw_count = 0;
	uint64_t frame_unsupported_attribution_truncated = 0;
	std::vector<TerrainStaticShadowUnsupportedAttribution>
			frame_unsupported_attribution;
	bool frame_has_projected_bounds = false;
	float frame_min_u = 0.0f;
	float frame_min_v = 0.0f;
	float frame_max_u = 0.0f;
	float frame_max_v = 0.0f;
	uint64_t frame_receiver_cache_hits = 0;
	uint64_t frame_receiver_cache_misses = 0;
	bool snapshot_exact = true;
	std::size_t caster_count = 0;
};

class TerrainStaticShadowPlanner {
public:
	TerrainStaticShadowPlanner();

	void set_enabled(bool enabled);
	bool is_enabled() const { return enabled_; }
	// Sorts and dedups; the canonical id list feeds the config stamp.
	void set_suppressed_bms_ids(std::vector<int32_t> ids);
	const std::vector<int32_t> &suppressed_bms_ids() const {
		return suppressed_ids_;
	}
	void set_light(const TerrainStaticShadowLightDirection &world_light,
			const TerrainTileLightEpoch &light_epoch);
	// Frame-shared Render_ShaderTickMs. Stored only: animated material state
	// (AlphaGen, the UV transform, diffuse flipbook frame) is evaluated from
	// this tick when a page is classified or rasterized, so advancing time
	// never walks the casters, never changes the state revision, and never
	// alters resident page content identity — retail cache hits key only on
	// the spatial tile, and animations are sampled when a tile is actually
	// recomposed. The adapter carries the requesting frame's tick with each
	// composition job and sets it on the worker's planner copy.
	void set_material_time(uint32_t time_ms) { material_time_ms_ = time_ms; }
	uint32_t material_time_ms() const { return material_time_ms_; }
	// The receiver height field must stay valid until replaced or cleared.
	// A terrain revision change clears the per-page receiver-minimum cache.
	void set_receiver_terrain(const TerrainHeightField &field,
			uint64_t terrain_revision);
	void clear_receiver_terrain();
	// Replaces the caster snapshot. Casters carry resolved geometry; a
	// missing geometry on an admitted caster (resolution failed) is declared
	// through admitted_geometry_missing so planning fails closed exactly as
	// the adapter's snapshot build did. An identical snapshot (same records,
	// same admission verdict) is a no-op that preserves cached plans.
	void replace_casters(std::vector<TerrainStaticShadowPlannerCaster> casters,
			bool admitted_geometry_missing);

	bool has_receiver_terrain() const { return receiver_valid_; }
	bool snapshot_exact() const { return casters_->exact; }
	std::size_t candidate_count() const {
		return casters_->collector.candidate_count();
	}
	std::size_t admitted_count() const {
		return casters_->collector.admitted_count();
	}
	std::size_t caster_count() const { return casters_->records.size(); }
	// Monotonic identity for the immutable planner state consumed by page
	// compilation. Diagnostic resets, sub-byte light motion, identical caster
	// snapshots, repeated receiver clears, and material time do not change it.
	uint64_t state_revision() const { return state_revision_; }

	void reset_frame_diagnostics();
	const TerrainStaticShadowPlannerDiagnostics &diagnostics() const {
		return diagnostics_;
	}

	TerrainStaticShadowPagePlanResult plan(const TerrainTilePageKey &page);
	bool rasterize(const TerrainTilePageKey &page,
			TerrainStaticShadowAlphaPage &page_alpha);

private:
	// The adopted caster snapshot. Immutable once published: every planner copy
	// (and every worker holding one) shares it by pointer.
	struct CasterSet {
		std::unordered_map<uint64_t, TerrainStaticShadowPlannerCaster> records;
		TerrainStaticShadowCollector collector;
		bool exact = true;
	};
	// Per-call memo of evaluated material states, keyed by caster: one page
	// job references a caster once per selected ROBJ, and classification and
	// rasterization must see one evaluation per caster within a call.
	using CasterMaterialStates = std::vector<TerrainStaticShadowMaterialState>;
	using MaterialStateTable =
			std::unordered_map<uint64_t, CasterMaterialStates>;
	struct CachedPlan {
		TerrainStaticShadowPageJob job;
		std::vector<uint8_t> supported;
		uint64_t supported_draws = 0;
		bool valid = false;
	};
	struct PageKeyEq {
		bool operator()(const TerrainTilePageKey &a,
				const TerrainTilePageKey &b) const noexcept;
	};
	struct PageKeyHash {
		std::size_t operator()(const TerrainTilePageKey &key) const noexcept;
	};

	void bump_epoch();
	void update_config_stamp();
	std::optional<float> page_receiver_minimum(const TerrainTilePageKey &page);
	bool compile(const TerrainTilePageKey &page,
			TerrainStaticShadowPageJob &job);
	const CasterMaterialStates &caster_material_states(
			MaterialStateTable &table, uint64_t caster_key,
			const TerrainStaticShadowPlannerCaster &caster) const;
	struct DrawSupport {
		bool structurally_valid = true;
		bool supported = true;
		int material_index = -1;
		uint32_t issues = kTerrainStaticShadowUnsupportedNone;
	};
	DrawSupport classify_draw(const TerrainStaticShadowProjectionDraw &draw,
			MaterialStateTable &material_states) const;
	bool classify_page_job(const TerrainStaticShadowPageJob &job,
			std::vector<uint8_t> *r_supported, bool record_unsupported);
	void record_unsupported_draw(const TerrainTilePageKey &page,
			const TerrainStaticShadowProjectionDraw &draw,
			const DrawSupport &support);
	const CachedPlan *plan_for(const TerrainTilePageKey &page);

	bool enabled_ = true;
	std::vector<int32_t> suppressed_ids_;
	TerrainStaticShadowLightDirection world_light_{};
	TerrainTileLightEpoch light_epoch_ = kDefaultTerrainTileLightEpoch;
	uint32_t material_time_ms_ = 0;
	uint64_t config_stamp_ = 0;
	TerrainHeightField receiver_field_{};
	bool receiver_valid_ = false;
	uint64_t terrain_revision_ = 0;
	std::shared_ptr<const CasterSet> casters_;
	// Stamp of the last adopted caster snapshot; 0 = none adopted yet.
	uint64_t caster_set_stamp_ = 0;
	std::unordered_map<TerrainTilePageKey, float, PageKeyHash, PageKeyEq>
			receiver_minimum_cache_;
	std::unordered_map<TerrainTilePageKey, CachedPlan, PageKeyHash, PageKeyEq>
			plan_cache_;
	TerrainStaticShadowPlannerDiagnostics diagnostics_;
	uint64_t state_revision_ = 1;
};

// The world-space caster bounds for a resolved geometry under a rows-major
// 3x4 transform (8-corner sweep of the authored local bounds).
TerrainStaticShadowBounds terrain_static_shadow_transformed_bounds(
		const TerrainStaticShadowResolvedGeometry &geometry,
		const std::array<float, 12> &world_transform);

// The stable caster key the collector draws carry.
uint64_t terrain_static_shadow_caster_key(int32_t entity_kind,
		int32_t entity_index, int32_t bms_id);

} // namespace opennova::terrain
