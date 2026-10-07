#pragma once

// Portable static terrain-shadow collector for the composed terrain page
// cache. Mission placement supplies stable caster/geometry identities, the
// entity position and the model sphere; this module owns witnessed
// admission, the sun-extended tile test, selected shadow LOD, all-ROBJ
// ordering, and a deterministic contribution stamp. The device binding
// remains responsible for resolving geometry refs and rasterizing/compositing
// them.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250; admission
// @0x60D421..0x60D450; sphere/tile test @0x60D465..0x60D54F; selected
// LOD/all-ROBJ submit @0x60D881..0x60D971]

#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_light_epoch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::terrain {

// Binding-owned stable model identity plus the two render-LOD populations the
// retail collector can select. Each output draw adds an explicit ROBJ index.
struct TerrainStaticShadowGeometrySource {
	uint64_t geometry_key = 0;
	std::array<uint16_t, 2> render_object_counts{};
};

// Per-selected-ROBJ resolution state shared by the Godot binding and the
// portable tests. A hierarchy-only ROBJ with no authored surfaces is an exact
// no-op. Once a surface is authored, losing any of it (including malformed
// indices or UVs needed by an alpha-dependent material) makes the silhouette
// inexact and must be attributed/omitted by the binding.
struct TerrainStaticShadowRenderObjectCoverage {
	uint32_t authored_surface_count = 0;
	uint32_t valid_surface_count = 0;
	bool malformed_indices = false;
	bool missing_required_uvs = false;

	constexpr bool is_valid_empty() const noexcept {
		return authored_surface_count == 0 && valid_surface_count == 0 &&
				!malformed_indices && !missing_required_uvs;
	}

	constexpr bool is_complete() const noexcept {
		return authored_surface_count == valid_surface_count &&
				!malformed_indices && !missing_required_uvs;
	}
};

// Accepts both authored strip conventions witnessed in NOVA assets: indices
// relative to the strip's vertex window, or absolute indices into the LOD
// vertex buffer. Every buffer/window bound is validated before dereferencing.
bool terrain_static_shadow_strip_indices_are_valid(
		const uint16_t *indices, uint32_t index_buffer_count,
		int32_t index_offset, uint16_t index_count,
		uint32_t vertex_buffer_count, int32_t vertex_offset,
		int32_t vertex_count) noexcept;

struct TerrainStaticShadowCandidate {
	// BMS id is retained for diagnostics/suppression. caster_key is the stable
	// binding lookup identity and may differ for runtime husks/replacements.
	int32_t bms_id = 0;
	uint64_t caster_key = 0;
	uint32_t collector_order = 0;
	int32_t entity_kind = 0;
	uint32_t entity_attrib = 0;
	uint32_t item_attrib = 0;
	uint32_t item_attrib2 = 0;
	bool active = true;
	// The entity's planar position in mission 16.16 (entity+4 X, entity+8 Y;
	// Godot x and -z) and its model's sphere radius (model+0x14, the husk
	// model once swapped) — the only caster inputs the tile test reads
	// [orig: @0x60D45E, @0x60D475, @0x60D50A..0x60D531].
	std::array<int32_t, 2> position_fixed{};
	int32_t model_radius_fixed = 0;
	TerrainStaticShadowGeometrySource geometry{};
	uint64_t transform_revision = 0;
};

// A mission-plane rectangle of terrain in 16.16 (x east, y north = Godot -z),
// inclusive: the pages that overlap it.
struct TerrainStaticShadowReach {
	int32_t min_x = 0;
	int32_t min_y = 0;
	int32_t max_x = 0;
	int32_t max_y = 0;

	bool operator==(const TerrainStaticShadowReach &other) const noexcept {
		return min_x == other.min_x && min_y == other.min_y &&
				max_x == other.max_x && max_y == other.max_y;
	}
};

// Every page a caster's static shadow can land in, whatever the sun. The tile
// test admits a caster whose sphere overlaps the tile grown toward the light by
// t * r, t = l * 0.5 / l_vertical with the vertical clamped to 0x4000, so for a
// unit light |t| <= 2 and no page farther than 3r from the caster's position
// on either axis takes its silhouette [orig: Terrain_CollectAndRenderTileModels
// @0x60D250: vertical clamp @0x60d315..0x60d32c, slope @0x60d35d..0x60d386,
// extension @0x60d47c..0x60d490, compares @0x60d500..0x60d54f].
TerrainStaticShadowReach terrain_static_shadow_caster_reach(
		const TerrainStaticShadowCandidate &candidate) noexcept;

struct TerrainStaticShadowLightDirection {
	float x = 0.0f;
	float y = 1.0f;
	float z = 0.0f;
};

struct TerrainStaticShadowPageInput {
	TerrainTilePageKey page{};
	// The raw surface-to-light direction in presentation axes (the
	// environment getter tuple through the (g2, g1, g0) reduction). The tile
	// test reads its 16.16 twin, the mission-axis Environment_GetLightDirection-
	// Fixed tuple, with the vertical clamped to 0x4000.
	TerrainStaticShadowLightDirection surface_to_light{};
	// Cache identity shared with the terrain DOT3 pass. The binding derives
	// these bytes from the raw environment getter tuple through the one portable
	// (g2,g0,g1) quantizer; raw direction remains the projection input above.
	TerrainTileLightEpoch light_epoch = kDefaultTerrainTileLightEpoch;
};

struct TerrainStaticShadowGeometryRef {
	uint64_t geometry_key = 0;
	uint8_t lod_index = 0;
	uint16_t render_object_index = 0;

	bool operator==(const TerrainStaticShadowGeometryRef &other) const noexcept {
		return geometry_key == other.geometry_key && lod_index == other.lod_index &&
				render_object_index == other.render_object_index;
	}
};

struct TerrainStaticShadowProjectionDraw {
	int32_t bms_id = 0;
	uint64_t caster_key = 0;
	uint32_t collector_order = 0;
	uint64_t transform_revision = 0;
	TerrainStaticShadowGeometryRef geometry{};

	bool operator==(const TerrainStaticShadowProjectionDraw &other) const noexcept {
		return bms_id == other.bms_id && caster_key == other.caster_key &&
				collector_order == other.collector_order &&
				transform_revision == other.transform_revision &&
				geometry == other.geometry;
	}
};

struct TerrainStaticShadowPageJob {
	TerrainTilePageKey page{};
	std::vector<TerrainStaticShadowProjectionDraw> draws;
	TerrainTileContentStamp content{};
};

class TerrainStaticShadowCollector {
public:
	// Replaces the mission/husk snapshot. Input order is deliberately irrelevant:
	// compile() recreates retail's pool-2-before-pool-1 stable collector order.
	void replace(std::vector<TerrainStaticShadowCandidate> candidates);

	std::size_t candidate_count() const noexcept { return candidate_count_; }
	std::size_t admitted_count() const noexcept { return admitted_.size(); }
	// The casters the admission keeps, in collector order.
	const std::vector<TerrainStaticShadowCandidate> &admitted() const noexcept {
		return admitted_;
	}

	// Invalid page levels return an empty job. A valid result is device-neutral:
	// one ordered reference per selected-LOD ROBJ, with no assumed RT format,
	// projection matrix, filtering, or composite strength.
	TerrainStaticShadowPageJob compile(
			const TerrainStaticShadowPageInput &input) const;

private:
	std::size_t candidate_count_ = 0;
	std::vector<TerrainStaticShadowCandidate> admitted_;
};

} // namespace opennova::terrain
