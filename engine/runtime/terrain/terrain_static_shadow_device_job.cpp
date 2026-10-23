#include <runtime/terrain/terrain_static_shadow_device_job.h>

#include <runtime/terrain/row_stripes.h>

// [orig: Terrain_CollectAndRenderTileModels @0x60D250 - the per-caster submit @0x60D971 and
//  flush @0x60D97D; the projection RenderSlot_SetupShadowCascadeMatrices_0 @0x58D4D3..0x58D618]

#include <algorithm>
#include <new>
#include <unordered_map>

namespace opennova::terrain {
namespace {

// The caster's world matrix applied to a model-space point, row by row in the order the
// planner's build has always evaluated it.
std::array<float, 3> transform_point(const std::array<float, 12> &transform,
		const std::array<float, 3> &point) {
	std::array<float, 3> out{};
	for (int row = 0; row < 3; ++row) {
		out[row] = transform[static_cast<std::size_t>(row) * 3] * point[0] +
				transform[static_cast<std::size_t>(row) * 3 + 1] * point[1] +
				transform[static_cast<std::size_t>(row) * 3 + 2] * point[2] +
				transform[9 + static_cast<std::size_t>(row)];
	}
	return out;
}

} // namespace

bool project_terrain_static_shadow_device_vertex(const TerrainStaticShadowDeviceJob &job,
		const TerrainStaticShadowDeviceDraw &draw, std::size_t vertex,
		TerrainStaticShadowRasterVertex &r_projected) noexcept {
	if (draw.geometry == nullptr || draw.lod_index > 1) return false;
	const std::vector<TerrainStaticShadowResolvedSurface> &surfaces =
			draw.geometry->surfaces[draw.lod_index];
	if (draw.surface_index >= surfaces.size()) return false;
	const TerrainStaticShadowResolvedSurface &surface = surfaces[draw.surface_index];
	if (vertex >= surface.vertices.size() || vertex >= surface.uvs.size()) return false;
	const std::array<float, 3> &local_base = surface.vertices[vertex];
	const std::array<float, 3> local{
		local_base[0] + surface.render_object_offset[0],
		local_base[1] + surface.render_object_offset[1],
		local_base[2] + surface.render_object_offset[2],
	};
	const std::array<float, 3> world = transform_point(draw.world_transform, local);
	const std::array<float, 2> &uv = surface.uvs[vertex];
	TerrainStaticShadowWorldVertex source;
	source.x = world[0];
	source.y = world[1];
	source.z = world[2];
	if (draw.transforms_uv) {
		source.texture_u = uv[0] * draw.uv.m00 + uv[1] * draw.uv.m10 + draw.uv.m20;
		source.texture_v = uv[0] * draw.uv.m01 + uv[1] * draw.uv.m11 + draw.uv.m21;
	} else {
		source.texture_u = uv[0];
		source.texture_v = uv[1];
	}
	TerrainStaticShadowProjectionInput projection;
	projection.page = job.page;
	projection.surface_to_light = job.surface_to_light;
	projection.caster_ground_y = draw.ground_y;
	return project_terrain_static_shadow_vertex(projection, source, r_projected);
}

bool build_terrain_static_shadow_device_raster_input(const TerrainStaticShadowDeviceJob &job,
		std::size_t threads, TerrainStaticShadowRasterInput &input,
		TerrainStaticShadowProjectedBounds *bounds) {
	try {
		input = TerrainStaticShadowRasterInput{};
		input.threads = threads;
		input.receiver_depth = job.receiver_depth;
		// The texture slots in draw order, one per distinct pyramid; each draw's triangles
		// start at its prefix.
		std::vector<int32_t> texture_slots(job.draws.size(), -1);
		std::vector<std::size_t> first_triangle(job.draws.size(), 0);
		std::unordered_map<const TerrainStaticShadowAlphaPyramid *, int32_t> slot_of;
		std::size_t triangle_total = 0;
		for (std::size_t index = 0; index < job.draws.size(); ++index) {
			const TerrainStaticShadowDeviceDraw &draw = job.draws[index];
			if (draw.geometry == nullptr || draw.lod_index > 1 ||
					draw.surface_index >= draw.geometry->surfaces[draw.lod_index].size() ||
					draw.geometry->surfaces[draw.lod_index][draw.surface_index].indices.size() <
							static_cast<std::size_t>(draw.triangle_count) * 3) {
				return false;
			}
			if (draw.alpha_texture != nullptr) {
				const auto found = slot_of.find(draw.alpha_texture);
				if (found != slot_of.end()) {
					texture_slots[index] = found->second;
				} else {
					const int32_t slot = static_cast<int32_t>(input.alpha_textures.size());
					TerrainStaticShadowAlphaTextureView view;
					view.mips = draw.alpha_texture->mips.data();
					view.mip_count = draw.alpha_texture->mips.size();
					view.mip_filter = TerrainStaticShadowMipFilter::Linear;
					input.alpha_textures.push_back(view);
					slot_of.emplace(draw.alpha_texture, slot);
					texture_slots[index] = slot;
				}
			}
			first_triangle[index] = triangle_total;
			triangle_total += draw.triangle_count;
		}
		input.triangles.resize(triangle_total);

		struct LaneResult {
			bool failed = false;
			TerrainStaticShadowProjectedBounds bounds;
		};
		// A lane per ~4k triangles, up to the raster's width.
		const std::size_t lanes = std::clamp<std::size_t>(
				triangle_total / 4096, 1, std::max<std::size_t>(threads, 1));
		std::vector<LaneResult> lane_results(lanes);
		run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
			LaneResult &result = lane_results[lane];
			const std::size_t begin = triangle_total * lane / lanes;
			const std::size_t end = triangle_total * (lane + 1) / lanes;
			if (begin >= end) return;
			// The draw holding `begin`: the last draw starting at or before it.
			std::size_t draw_index = static_cast<std::size_t>(std::upper_bound(
					first_triangle.begin(), first_triangle.end(), begin) -
					first_triangle.begin()) - 1;
			for (std::size_t slot = begin; slot < end; ++slot) {
				while (draw_index + 1 < job.draws.size() &&
						first_triangle[draw_index + 1] <= slot) {
					++draw_index;
				}
				const TerrainStaticShadowDeviceDraw &draw = job.draws[draw_index];
				const TerrainStaticShadowResolvedSurface &surface =
						draw.geometry->surfaces[draw.lod_index][draw.surface_index];
				const std::size_t index = (slot - first_triangle[draw_index]) * 3;
				TerrainStaticShadowRasterTriangle &triangle = input.triangles[slot];
				triangle.blend = draw.blend;
				triangle.two_sided = draw.two_sided;
				triangle.alpha_texture_index = texture_slots[draw_index];
				triangle.alpha_test_enabled = draw.alpha_test_enabled;
				triangle.alpha_test_inverted = draw.alpha_test_inverted;
				triangle.alpha_ref = draw.alpha_ref;
				triangle.alpha_scale = draw.alpha_scale;
				for (int corner = 0; corner < 3; ++corner) {
					const int32_t vertex_index =
							surface.indices[index + static_cast<std::size_t>(corner)];
					if (vertex_index < 0 ||
							!project_terrain_static_shadow_device_vertex(job, draw,
									static_cast<std::size_t>(vertex_index),
									triangle.vertices[corner])) {
						result.failed = true;
						return;
					}
					const TerrainStaticShadowRasterVertex &projected = triangle.vertices[corner];
					if (!result.bounds.valid) {
						result.bounds.min_u = result.bounds.max_u = projected.page_u;
						result.bounds.min_v = result.bounds.max_v = projected.page_v;
						result.bounds.valid = true;
					} else {
						result.bounds.min_u = std::min(result.bounds.min_u, projected.page_u);
						result.bounds.min_v = std::min(result.bounds.min_v, projected.page_v);
						result.bounds.max_u = std::max(result.bounds.max_u, projected.page_u);
						result.bounds.max_v = std::max(result.bounds.max_v, projected.page_v);
					}
				}
			}
		});
		for (const LaneResult &result : lane_results) {
			if (result.failed) return false;
			if (bounds == nullptr || !result.bounds.valid) continue;
			if (!bounds->valid) {
				*bounds = result.bounds;
				continue;
			}
			bounds->min_u = std::min(bounds->min_u, result.bounds.min_u);
			bounds->min_v = std::min(bounds->min_v, result.bounds.min_v);
			bounds->max_u = std::max(bounds->max_u, result.bounds.max_u);
			bounds->max_v = std::max(bounds->max_v, result.bounds.max_v);
		}
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}

bool rasterize_terrain_static_shadow_device_job(const TerrainStaticShadowDeviceJob &job,
		TerrainStaticShadowAlphaPage &page, std::size_t threads,
		TerrainStaticShadowRasterStats *stats, TerrainStaticShadowProjectedBounds *bounds) {
	TerrainStaticShadowRasterInput input;
	if (!build_terrain_static_shadow_device_raster_input(job, threads, input, bounds)) return false;
	input.stats = stats;
	return rasterize_terrain_static_shadow_alpha(input, page);
}

} // namespace opennova::terrain
