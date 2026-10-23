#pragma once

// The static-shadow page pass in the form the device draws it. Retail's CPU collects the
// casters and sets each one's matrices, then submits it once (Render_SubmitEntity, mode 2)
// and flushes the batch queue (CRenderBatchQueue_SortAndFlush) into the temp target: the
// casters' vertices are the device's, its vertex stage applying the caster's world matrix,
// the shadow ortho's row and the material's UV transform. A TerrainStaticShadowDeviceDraw is
// one caster surface's batch with the state it draws under, in the collector / ROBJ / strip
// order the CPU raster takes; the walk that lists them is the CPU's whole share of a page.
// rasterize_terrain_static_shadow_device_job evaluates the same job on the CPU: the device
// pass's oracle, and the raster wherever no device runs.
// [orig: Terrain_CollectAndRenderTileModels @0x60D250 - the temp target GTexRT_SelectThunk
//  (the call @0x60D5BF), per caster Render_SubmitEntity @0x5DAD80 (the call @0x60D971, mode 2)
//  and CRenderBatchQueue_SortAndFlush @0x5DAE40 (the call @0x60D97D), GTexRT_RestoreThunk
//  (the call @0x60DA4F)]

#include <runtime/terrain/terrain_static_shadow.h>
#include <runtime/terrain/terrain_static_shadow_alpha.h>
#include <runtime/terrain/terrain_static_shadow_geometry.h>
#include <runtime/terrain/terrain_static_shadow_raster.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace opennova::terrain {

struct TerrainStaticShadowDeviceDraw {
	// The caster's resolved geometry (it owns the surface, its material and the alpha
	// pyramids the draw names), and the surface: surfaces[lod_index][surface_index], an
	// unindexed triangle soup of surface.indices.size() / 3 triangles.
	std::shared_ptr<const TerrainStaticShadowResolvedGeometry> geometry;
	uint8_t lod_index = 0;
	uint32_t surface_index = 0;
	uint32_t triangle_count = 0;
	// The caster's presentation-world transform (rows-major, then the origin) and the terrain
	// height under its origin, which the projection subtracts.
	std::array<float, 12> world_transform{};
	float ground_y = 0.0f;
	// The material state the surface draws under at the job's material tick.
	TerrainStaticShadowBlend blend = TerrainStaticShadowBlend::Opaque;
	bool two_sided = false;
	bool alpha_test_enabled = false;
	bool alpha_test_inverted = false;
	uint8_t alpha_ref = 0;
	float alpha_scale = 1.0f;
	// The material samples the diffuse alpha through its UV transform; otherwise the
	// authored UVs pass through.
	bool transforms_uv = false;
	renderer::UvAnimTransform uv{};
	// The alpha texture (the selected diffuse frame), or null for an untextured draw.
	const TerrainStaticShadowAlphaPyramid *alpha_texture = nullptr;
};

struct TerrainStaticShadowDeviceJob {
	TerrainTilePageKey page{};
	// Presentation-world surface-to-light (the projection's light).
	TerrainStaticShadowLightDirection surface_to_light{};
	// The temp target's depth clear: the receiver plane LESSEQUAL admits.
	float receiver_depth = 0.5f;
	std::vector<TerrainStaticShadowDeviceDraw> draws;
	uint64_t triangle_count = 0;
};

// The projected bounds of a job's vertices in page UV (diagnostics).
struct TerrainStaticShadowProjectedBounds {
	bool valid = false;
	float min_u = 0.0f;
	float min_v = 0.0f;
	float max_u = 0.0f;
	float max_v = 0.0f;
};

// One caster vertex as the device's vertex stage produces it: the surface vertex plus its
// ROBJ offset, through the caster's transform, projected onto the page; the UV through the
// material transform where the draw takes it. False for non-finite input.
bool project_terrain_static_shadow_device_vertex(const TerrainStaticShadowDeviceJob &job,
		const TerrainStaticShadowDeviceDraw &draw, std::size_t vertex,
		TerrainStaticShadowRasterVertex &r_projected) noexcept;

// The job's triangles as the CPU raster takes them: every draw's triangles projected, in
// the job's order (runs of it split over `threads` lanes, the result the same for every
// count), each with its draw's state and texture slot. False where a draw names no surface
// or a vertex does not project.
bool build_terrain_static_shadow_device_raster_input(const TerrainStaticShadowDeviceJob &job,
		std::size_t threads, TerrainStaticShadowRasterInput &r_input,
		TerrainStaticShadowProjectedBounds *bounds = nullptr);

// The job on the CPU: its raster input (above) rasterized into `page`
// (rasterize_terrain_static_shadow_alpha). False, `page` untouched, where the input does not
// build or the raster refuses it.
bool rasterize_terrain_static_shadow_device_job(const TerrainStaticShadowDeviceJob &job,
		TerrainStaticShadowAlphaPage &page, std::size_t threads,
		TerrainStaticShadowRasterStats *stats = nullptr,
		TerrainStaticShadowProjectedBounds *bounds = nullptr);

} // namespace opennova::terrain
