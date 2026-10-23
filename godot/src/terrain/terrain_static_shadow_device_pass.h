#pragma once

// The terrain static-shadow pass on the device: one page job's casters drawn into the temp
// target and resolved into the page's alpha, as retail's tile render does on its device.
// Device work only: the job (which casters, in which order, under which state) is the
// engine's (runtime/terrain/terrain_static_shadow_device_job.h), and the CPU raster that
// evaluates the same job (rasterize_terrain_static_shadow_device_job) is its oracle and the
// raster wherever this pass cannot run.
//
// Per job, recorded on the RenderingDevice ahead of the frame's terrain draw:
// - the fill: a compute pass writes the temp (R8, twice the page) from the uploaded layer's
//   DOT3 light alpha, nearest 2x, where retail draws its DOT3 light quad into the temp;
// - the casters: one draw list into the temp and a D32F depth cleared to the job's
//   clear_depth (retail's temp clear: its light quad writes no depth), one draw per caster
//   surface in the job's order, LESSEQUAL with depth writes, the vertex stage projecting the
//   caster as the shadow ortho does and the fragment stage writing the black PROJSHAD
//   fragment under the material's alpha test and blend;
// - the resolve: a compute pass writes the layer's alpha from the temp's 2x2 boxes (the
//   composite's MINFILTER LINEAR fetch at the box centre), its RGB untouched.
// The witnesses for each step are the engine's (runtime/terrain/
// terrain_static_shadow_device_job.h).

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/terrain/terrain_static_shadow_device_job.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace godot {

class TerrainStaticShadowDevicePass {
public:
	TerrainStaticShadowDevicePass() = default;
	~TerrainStaticShadowDevicePass();
	TerrainStaticShadowDevicePass(const TerrainStaticShadowDevicePass &) = delete;
	TerrainStaticShadowDevicePass &operator=(const TerrainStaticShadowDevicePass &) = delete;

	// Builds the pass for `p_page_array` (an RGBA8 2D array of `p_page_dimension`-square
	// layers with storage use): the temp target and its depth, the shaders and pipelines.
	// False when the device cannot run it (a format it cannot attach or store, a shader or
	// pipeline it refuses); failure() says why.
	bool prepare(RenderingDevice *p_rd, const RID &p_page_array, uint32_t p_page_dimension);
	bool is_prepared() const { return prepared_; }
	// Draws one job's casters into layer `p_layer` of the page array, whose bytes the caller
	// already uploaded. False (failure() says why) where the device refused a step.
	bool record(const opennova::terrain::TerrainStaticShadowDeviceJob &p_job, uint32_t p_layer);
	// Frees the cached caster geometry and textures no caster holds any more.
	void prune();
	void release();
	const std::string &failure() const { return failure_; }

	// Counters (diagnostics).
	uint64_t recorded_jobs() const { return recorded_jobs_; }
	uint64_t recorded_draws() const { return recorded_draws_; }
	uint64_t recorded_triangles() const { return recorded_triangles_; }
	uint64_t geometry_uploads() const { return geometry_uploads_; }
	uint64_t texture_uploads() const { return texture_uploads_; }
	std::size_t cached_geometries() const { return geometries_.size(); }
	std::size_t cached_textures() const { return textures_.size(); }

private:
	// One caster geometry's triangles on the device, each surface's corners in the CPU
	// raster's order (index k of triangle t is corner indices[3t + k]).
	struct GeometryEntry {
		std::shared_ptr<const opennova::terrain::TerrainStaticShadowResolvedGeometry> geometry;
		RID vertices;
		// The first vertex of surfaces[lod][surface].
		std::array<std::vector<uint32_t>, 2> first_vertex;
	};
	struct TextureEntry {
		// The geometry whose material owns the pyramid (keeps the key alive).
		std::shared_ptr<const opennova::terrain::TerrainStaticShadowResolvedGeometry> owner;
		RID texture;
		RID uniform_set;
	};

	bool fail(const std::string &p_reason);
	bool build_shaders();
	bool build_targets();
	bool build_pipelines();
	const GeometryEntry *geometry_for(const opennova::terrain::TerrainStaticShadowDeviceDraw &p_draw);
	RID texture_set_for(const opennova::terrain::TerrainStaticShadowDeviceDraw &p_draw);
	bool dispatch(const RID &p_pipeline, const RID &p_set, uint32_t p_layer, uint32_t p_size);
	void free_rid(RID &r_rid);

	RenderingDevice *rd_ = nullptr;
	RID page_array_;
	uint32_t page_dimension_ = 0;
	uint32_t temp_dimension_ = 0;
	bool prepared_ = false;
	std::string failure_;

	RID temp_color_;
	RID temp_depth_;
	RID framebuffer_;
	int64_t vertex_format_ = -1;
	RID caster_shader_;
	// [blend group][cull]: the opaque / multiply write, the alpha blend, the additive one;
	// culled (CCW) or two-sided.
	std::array<std::array<RID, 2>, 3> caster_pipelines_{};
	RID sampler_;
	RID blank_texture_;
	RID blank_set_;
	RID fill_shader_;
	RID fill_pipeline_;
	RID fill_set_;
	RID resolve_shader_;
	RID resolve_pipeline_;
	RID resolve_set_;
	PackedByteArray caster_push_;
	PackedByteArray pass_push_;
	TypedArray<RID> vertex_buffers_;
	PackedInt64Array vertex_offsets_;

	std::unordered_map<const opennova::terrain::TerrainStaticShadowResolvedGeometry *, GeometryEntry>
			geometries_;
	std::unordered_map<const opennova::terrain::TerrainStaticShadowAlphaPyramid *, TextureEntry>
			textures_;

	uint64_t recorded_jobs_ = 0;
	uint64_t recorded_draws_ = 0;
	uint64_t recorded_triangles_ = 0;
	uint64_t geometry_uploads_ = 0;
	uint64_t texture_uploads_ = 0;
};

} // namespace godot
