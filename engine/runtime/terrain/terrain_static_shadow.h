#pragma once

// Portable static terrain-shadow collector for the composed terrain page
// cache. Mission placement supplies stable caster/geometry identities and
// world bounds; this module owns witnessed admission, sun-swept page culling,
// selected shadow LOD, all-ROBJ ordering, and a deterministic contribution
// stamp. The device binding remains responsible for resolving geometry refs
// and rasterizing/compositing them.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250; admission
// @0x60D421..0x60D450; projected bound/page intersection
// @0x60D465..0x60D54F; selected LOD/all-ROBJ submit
// @0x60D881..0x60D971]

#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_light_epoch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::terrain {

struct TerrainStaticShadowBounds {
	float min_x = 0.0f;
	float min_y = 0.0f;
	float min_z = 0.0f;
	float max_x = 0.0f;
	float max_y = 0.0f;
	float max_z = 0.0f;

	bool valid() const noexcept;
};

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
	TerrainStaticShadowBounds world_bounds{};
	TerrainStaticShadowGeometrySource geometry{};
	uint64_t transform_revision = 0;
};

struct TerrainStaticShadowLightDirection {
	float x = 0.0f;
	float y = 1.0f;
	float z = 0.0f;
};

struct TerrainStaticShadowPageInput {
	TerrainTilePageKey page{};
	// Normalized surface-to-light direction. Like retail, the collector clamps
	// the vertical projection divisor to 0.25 before expanding the footprint.
	TerrainStaticShadowLightDirection surface_to_light{};
	// Cache identity shared with the terrain DOT3 pass. The binding derives
	// these bytes from the raw environment getter tuple through the one portable
	// (g2,g0,g1) quantizer; raw direction remains the projection input above.
	TerrainTileLightEpoch light_epoch = kDefaultTerrainTileLightEpoch;
	// Conservative receiver plane for broad-phase projection. Raster bindings
	// can use the actual page geometry; this value only decides candidate/page
	// intersection and therefore should be the page's minimum terrain height.
	float receiver_height = 0.0f;
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
