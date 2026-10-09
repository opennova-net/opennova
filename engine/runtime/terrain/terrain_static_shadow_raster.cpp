#include <runtime/terrain/terrain_static_shadow_raster.h>

#include <runtime/renderer/material_classify.h>
#include <runtime/terrain/row_stripes.h>

// [orig: Terrain_CollectAndRenderTileModels @0x60D5BF..0x60DA4F:
// the alt DOT3 light pass apply @0x60D794, black PROJSHAD geometry
// @0x60D960..0x60D97D; PolyTrn_RenderTile @0x60E0C6..0x60E19D:
// 2x temp MINFILTER=LINEAR, PSDepthAlpha temp-blue -> destination alpha]

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace opennova::terrain {
namespace {

// The temp target is twice the page at every tier: config+0x1740 selects
// page dword_31A00D4 = 0x100 with temp dword_31A00D0 = 0x200, otherwise
// 0x80 with 0x100 [orig: PolyTrn_LoadTerrainConfig @0x60E41D..0x60E446]. The
// collector sizes its viewport from the temp dimension [orig: @0x60D5E0,
// @0x60D9E4], and the page composite samples the temp with MINFILTER LINEAR
// over a quad spanning [-0.5, page - 0.5] [orig: PolyTrn_RenderTile
// @0x60E0D1..0x60E0E0, @0x60E152..0x60E166]: every page pixel lands exactly
// between two temp texels per axis, so the bilinear fetch is the 2x2 box.
constexpr uint32_t kTemporaryScale = 2;
constexpr float kDegenerateArea = 1.0e-12f;
// The same 0.25 vertical clamp the collect phase applies in fixed point
// [orig: float clamp @ 0x60D33F..0x60D341, fixed twin (0x4000)
// @ 0x60d325..0x60d32c].
constexpr float kMinimumVerticalLight = 0.25f;
// Projected depth h*0.0005 - 0.00005 [orig: RenderSlot_SetupShadowCascadeMatrices_0
// @ 0x58D4D3..0x58D618].
constexpr float kProjectedDepthScale = 0.00050000002f;
constexpr float kProjectedDepthBias = -0.000050000002f;

struct ScreenVertex {
	float x = 0.0f;
	float y = 0.0f;
	float depth = 0.0f;
	float texture_u = 0.0f;
	float texture_v = 0.0f;
};

float edge(const ScreenVertex &a, const ScreenVertex &b,
		float x, float y) noexcept {
	return (b.x - a.x) * (y - a.y) -
			(b.y - a.y) * (x - a.x);
}

// D3D's half-open top-left fill convention in screen coordinates (Y grows
// down). `orientation` below reverses clockwise edges before this predicate,
// so a shared edge is inclusive for exactly one adjacent triangle.
// [orig: D3D9 rasterization rules consumed by the PROJSHAD draw
// @0x60D960..0x60D97D]
bool is_top_left(const ScreenVertex &a, const ScreenVertex &b) noexcept {
	const float dy = b.y - a.y;
	const float dx = b.x - a.x;
	return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

// A block of pixel centres [x_lo, x_hi] x [y_lo, y_hi] that one edge
// provably rejects: edge() is linear, so its exact value over the block
// peaks at a corner, and the float evaluation of any centre inside differs
// from the exact value by at most ~3 ulps of |dx||y - a.y| + |dy||x - a.x|
// (margin: 1e-6 of that bound). A corner peak below -margin therefore leaves
// every centre's float value negative, which edge_admits() refuses. Skipping
// the block changes no fragment; it only spares the scan of a thin
// triangle's empty bounding box.
bool edge_rejects_block(const ScreenVertex &a, const ScreenVertex &b,
		float orientation, int x_lo, int x_hi, int y_lo, int y_hi) noexcept {
	const double dx = static_cast<double>(b.x - a.x);
	const double dy = static_cast<double>(b.y - a.y);
	const double ax = a.x;
	const double ay = a.y;
	const double reach_x = std::max(std::abs(x_lo - ax), std::abs(x_hi - ax));
	const double reach_y = std::max(std::abs(y_lo - ay), std::abs(y_hi - ay));
	const double margin = 1.0e-6 * (std::abs(dx) * reach_y + std::abs(dy) * reach_x);
	const double sign = orientation;
	const double corners[4] = {
		(dx * (y_lo - ay) - dy * (x_lo - ax)) * sign,
		(dx * (y_lo - ay) - dy * (x_hi - ax)) * sign,
		(dx * (y_hi - ay) - dy * (x_lo - ax)) * sign,
		(dx * (y_hi - ay) - dy * (x_hi - ax)) * sign,
	};
	return std::max({corners[0], corners[1], corners[2], corners[3]}) < -margin;
}

bool edge_admits(float value, bool inclusive) noexcept {
	return value > 0.0f || (value == 0.0f && inclusive);
}

// The first and one-past-last pixel whose centre lies in [min, max]. D3D9
// puts a pixel's centre on its integer screen coordinate, and the viewport
// maps clip x = -1 onto screen 0: the untransformed-geometry draw into the
// temp target therefore samples pixel i at page_u = i / width, half a temp
// pixel before the texel centre the page composite later reads
// [orig: the collector's plain ortho, no half-pixel bias,
// RenderSlot_SetupShadowCascadeMatrices_0 @0x58D5CF..0x58D5EE; viewport = the temp
// dimension @0x60D5E0].
int first_pixel_centre(float value, int limit) noexcept {
	if (value <= 0.0f) return 0;
	if (value >= static_cast<float>(limit)) return limit;
	return static_cast<int>(std::ceil(value));
}

int past_last_pixel_centre(float value, int limit) noexcept {
	if (value < 0.0f) return 0;
	if (value >= static_cast<float>(limit - 1)) return limit;
	return static_cast<int>(std::floor(value)) + 1;
}

bool finite(const TerrainStaticShadowRasterVertex &vertex) noexcept {
	return std::isfinite(vertex.page_u) && std::isfinite(vertex.page_v) &&
			std::isfinite(vertex.depth) &&
			std::isfinite(vertex.texture_u) &&
			std::isfinite(vertex.texture_v);
}

bool valid_mip(const TerrainStaticShadowAlphaMipView &mip) noexcept {
	return mip.width > 0 && mip.height > 0 && mip.alpha != nullptr &&
			mip.row_stride >= mip.width;
}

bool valid_texture(
		const TerrainStaticShadowAlphaTextureView &texture) noexcept {
	if (texture.mips == nullptr || texture.mip_count == 0) return false;
	for (std::size_t index = 0; index < texture.mip_count; ++index) {
		if (!valid_mip(texture.mips[index])) return false;
	}
	return true;
}

// The byte -> [0, 1] texel decode, one entry per byte value (the same single
// division the per-texel form performed).
struct UnitByteTable {
	std::array<float, 256> values{};
	UnitByteTable() noexcept {
		for (int index = 0; index < 256; ++index) {
			values[static_cast<std::size_t>(index)] =
					static_cast<float>(index) / 255.0f;
		}
	}
};
const UnitByteTable kUnitByte;

// WRAP addressing of the texel pair around one sample: fract(u) * size - 0.5
// floors into [-1, size - 1], so (index, index + 1) folds into [0, size) with
// one conditional each (anything else takes the general fold).
int wrap(int value, int size) noexcept {
	value %= size;
	return value < 0 ? value + size : value;
}

void wrap_pair(int index, int size, int &r_first, int &r_second) noexcept {
	if (index < -1 || index >= size) {
		r_first = wrap(index, size);
		r_second = wrap(index + 1, size);
		return;
	}
	r_first = index < 0 ? index + size : index;
	r_second = index + 1 >= size ? index + 1 - size : index + 1;
}

float sample_mip(const TerrainStaticShadowAlphaMipView &mip,
		float u, float v) noexcept {
	const float x = (u - std::floor(u)) * static_cast<float>(mip.width) - 0.5f;
	const float y = (v - std::floor(v)) * static_cast<float>(mip.height) - 0.5f;
	const float floor_x = std::floor(x);
	const float floor_y = std::floor(y);
	int x0 = 0;
	int x1 = 0;
	int y0 = 0;
	int y1 = 0;
	wrap_pair(static_cast<int>(floor_x), static_cast<int>(mip.width), x0, x1);
	wrap_pair(static_cast<int>(floor_y), static_cast<int>(mip.height), y0, y1);
	const float tx = x - floor_x;
	const float ty = y - floor_y;
	const auto texel = [&](int px, int py) noexcept {
		return kUnitByte.values[
				mip.alpha[static_cast<std::size_t>(py) * mip.row_stride + px]];
	};
	const float top = texel(x0, y0) +
			(texel(x1, y0) - texel(x0, y0)) * tx;
	const float bottom = texel(x0, y1) +
			(texel(x1, y1) - texel(x0, y1)) * tx;
	return top + (bottom - top) * ty;
}

struct TextureDerivatives {
	float du_dx = 0.0f;
	float dv_dx = 0.0f;
	float du_dy = 0.0f;
	float dv_dy = 0.0f;
};

// One triangle's mip choice: its UV derivatives are constant across the
// affine triangle, so the level and the blend weight are too.
struct MipSelection {
	const TerrainStaticShadowAlphaMipView *lower = nullptr;
	const TerrainStaticShadowAlphaMipView *upper = nullptr;
	float t = 0.0f;
	bool point = false;
};

MipSelection select_mips(const TerrainStaticShadowAlphaTextureView &texture,
		const TextureDerivatives &derivatives) noexcept {
	const TerrainStaticShadowAlphaMipView &base = texture.mips[0];
	const float width = static_cast<float>(base.width);
	const float height = static_cast<float>(base.height);
	const float rho_x = std::hypot(
			derivatives.du_dx * width, derivatives.dv_dx * height);
	const float rho_y = std::hypot(
			derivatives.du_dy * width, derivatives.dv_dy * height);
	const float rho = std::max({1.0f, rho_x, rho_y});
	const float lod = std::clamp(std::log2(rho), 0.0f,
			static_cast<float>(texture.mip_count - 1));
	MipSelection selection;
	if (texture.mip_filter == TerrainStaticShadowMipFilter::Point) {
		const std::size_t mip = static_cast<std::size_t>(
				std::clamp(static_cast<int>(std::floor(lod + 0.5f)), 0,
						static_cast<int>(texture.mip_count - 1)));
		selection.lower = &texture.mips[mip];
		selection.point = true;
		return selection;
	}
	const std::size_t lower = static_cast<std::size_t>(std::floor(lod));
	const std::size_t upper = std::min(lower + 1, texture.mip_count - 1);
	selection.lower = &texture.mips[lower];
	selection.upper = &texture.mips[upper];
	selection.t = lod - static_cast<float>(lower);
	return selection;
}

float sample_texture_alpha(const MipSelection &selection,
		float u, float v) noexcept {
	const float a = sample_mip(*selection.lower, u, v);
	if (selection.point) return a;
	return a + (sample_mip(*selection.upper, u, v) - a) * selection.t;
}

TextureDerivatives texture_derivatives(
		const std::array<ScreenVertex, 3> &vertices, float area) noexcept {
	const float dx1 = vertices[1].x - vertices[0].x;
	const float dy1 = vertices[1].y - vertices[0].y;
	const float dx2 = vertices[2].x - vertices[0].x;
	const float dy2 = vertices[2].y - vertices[0].y;
	const float du1 = vertices[1].texture_u - vertices[0].texture_u;
	const float dv1 = vertices[1].texture_v - vertices[0].texture_v;
	const float du2 = vertices[2].texture_u - vertices[0].texture_u;
	const float dv2 = vertices[2].texture_v - vertices[0].texture_v;
	return {
			(du1 * dy2 - du2 * dy1) / area,
			(dv1 * dy2 - dv2 * dy1) / area,
			(dx1 * du2 - dx2 * du1) / area,
			(dx1 * dv2 - dx2 * dv1) / area};
}

uint8_t blend_fragment(uint8_t destination,
		TerrainStaticShadowBlend blend, float source_alpha) noexcept {
	switch (blend) {
		case TerrainStaticShadowBlend::Opaque:
		case TerrainStaticShadowBlend::Multiply:
			return 0;
		case TerrainStaticShadowBlend::Additive:
			return destination;
		case TerrainStaticShadowBlend::Alpha:
			return static_cast<uint8_t>(std::clamp(
					static_cast<int>(std::lround(destination *
							(1.0f - source_alpha))), 0, 255));
	}
	return destination;
}

bool validate(const TerrainStaticShadowRasterInput &input,
		const TerrainStaticShadowAlphaPage &page) noexcept {
	if (!page.is_valid() || !std::isfinite(input.receiver_depth)) return false;
	if (page.width > std::numeric_limits<uint32_t>::max() / kTemporaryScale ||
			page.height > std::numeric_limits<uint32_t>::max() / kTemporaryScale) {
		return false;
	}
	for (const TerrainStaticShadowRasterTriangle &triangle : input.triangles) {
		if (!std::isfinite(triangle.alpha_scale)) return false;
		for (const TerrainStaticShadowRasterVertex &vertex : triangle.vertices) {
			if (!finite(vertex)) return false;
		}
		if (triangle.alpha_texture_index < 0) {
			if (triangle.alpha_test_enabled) return false;
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(
				triangle.alpha_texture_index);
		if (index >= input.alpha_textures.size() ||
				!valid_texture(input.alpha_textures[index])) {
			return false;
		}
	}
	return true;
}

// A triangle clipped against one plane has at most four corners.
constexpr std::size_t kClippedPolygonMax = 4;

TerrainStaticShadowRasterVertex lerp_vertex(
		const TerrainStaticShadowRasterVertex &a,
		const TerrainStaticShadowRasterVertex &b, float t) noexcept {
	TerrainStaticShadowRasterVertex out;
	out.page_u = a.page_u + (b.page_u - a.page_u) * t;
	out.page_v = a.page_v + (b.page_v - a.page_v) * t;
	out.depth = a.depth + (b.depth - a.depth) * t;
	out.texture_u = a.texture_u + (b.texture_u - a.texture_u) * t;
	out.texture_v = a.texture_v + (b.texture_v - a.texture_v) * t;
	return out;
}

// Sutherland-Hodgman against the near plane depth >= 0 (the D3D clip volume's
// z >= 0 face, w = 1 under the ortho). Keeps the input winding. Returns the
// corner count: 0 when the whole triangle lies below the plane.
std::size_t clip_near_plane(
		const std::array<TerrainStaticShadowRasterVertex, 3> &in,
		std::array<TerrainStaticShadowRasterVertex, kClippedPolygonMax> &out)
		noexcept {
	std::size_t count = 0;
	for (std::size_t index = 0; index < in.size(); ++index) {
		const TerrainStaticShadowRasterVertex &current = in[index];
		const TerrainStaticShadowRasterVertex &next =
				in[(index + 1) % in.size()];
		const bool current_inside = current.depth >= 0.0f;
		const bool next_inside = next.depth >= 0.0f;
		if (current_inside) {
			out[count++] = current;
		}
		if (current_inside != next_inside) {
			const float t = current.depth / (current.depth - next.depth);
			out[count++] = lerp_vertex(current, next, t);
		}
	}
	return count;
}


// One fan triangle after clipping, culling and setup: everything the pixel
// loop reads.
struct PreparedTriangle {
	std::array<ScreenVertex, 3> vertices;
	float orientation = 1.0f;
	float inverse_area = 0.0f;
	int x0 = 0;
	int x1 = 0;
	int y0 = 0;
	int y1 = 0;
	bool edge0_inclusive = false;
	bool edge1_inclusive = false;
	bool edge2_inclusive = false;
	bool textured = false;
	MipSelection mips;
	const TerrainStaticShadowRasterTriangle *source = nullptr;
};

// Below this many input triangles the raster stays on the calling thread.
constexpr std::size_t kParallelMinimumTriangles = 256;

bool prepare_triangle(const TerrainStaticShadowRasterTriangle &triangle,
		const std::array<const TerrainStaticShadowRasterVertex *, 3> &corner,
		const TerrainStaticShadowRasterInput &input,
		uint32_t high_width, uint32_t high_height,
		PreparedTriangle &r_out) noexcept {
	std::array<ScreenVertex, 3> &vertices = r_out.vertices;
	for (std::size_t index = 0; index < vertices.size(); ++index) {
		vertices[index] = {
				corner[index]->page_u * high_width,
				corner[index]->page_v * high_height,
				corner[index]->depth,
				corner[index]->texture_u,
				corner[index]->texture_v};
	}
	const float area = edge(vertices[0], vertices[1],
			vertices[2].x, vertices[2].y);
	if (std::abs(area) <= kDegenerateArea) return false;
	// The ordinary tile-model pass uses CULLMODE CCW: in D3D screen
	// coordinates (Y down), positive signed area is the retained clockwise
	// front face. The material two-sided bit switches to CULLMODE NONE.
	// [orig: CRenderBatchQueue_FlushBatches @0x5DA3E4..0x5DA401]
	if (!triangle.two_sided && area < 0.0f) return false;
	const float orientation = area > 0.0f ? 1.0f : -1.0f;
	const TextureDerivatives derivatives =
			texture_derivatives(vertices, area);
	const float min_x = std::min({vertices[0].x, vertices[1].x,
			vertices[2].x});
	const float max_x = std::max({vertices[0].x, vertices[1].x,
			vertices[2].x});
	const float min_y = std::min({vertices[0].y, vertices[1].y,
			vertices[2].y});
	const float max_y = std::max({vertices[0].y, vertices[1].y,
			vertices[2].y});
	r_out.x0 = first_pixel_centre(min_x, static_cast<int>(high_width));
	r_out.x1 = past_last_pixel_centre(max_x, static_cast<int>(high_width));
	r_out.y0 = first_pixel_centre(min_y, static_cast<int>(high_height));
	r_out.y1 = past_last_pixel_centre(max_y, static_cast<int>(high_height));
	if (r_out.x0 >= r_out.x1 || r_out.y0 >= r_out.y1) return false;
	r_out.edge0_inclusive = orientation > 0.0f
			? is_top_left(vertices[1], vertices[2])
			: is_top_left(vertices[2], vertices[1]);
	r_out.edge1_inclusive = orientation > 0.0f
			? is_top_left(vertices[2], vertices[0])
			: is_top_left(vertices[0], vertices[2]);
	r_out.edge2_inclusive = orientation > 0.0f
			? is_top_left(vertices[0], vertices[1])
			: is_top_left(vertices[1], vertices[0]);
	r_out.orientation = orientation;
	r_out.textured = triangle.alpha_texture_index >= 0;
	if (r_out.textured) {
		r_out.mips = select_mips(input.alpha_textures[static_cast<std::size_t>(
				triangle.alpha_texture_index)], derivatives);
	}
	r_out.inverse_area = 1.0f / std::abs(area);
	r_out.source = &triangle;
	return true;
}

// The pixel loop of one prepared triangle over rows [row_begin, row_end).
void raster_rows(const PreparedTriangle &prepared, int row_begin, int row_end,
		uint32_t high_width, uint8_t *temporary, float *depth_buffer) noexcept {
	const std::array<ScreenVertex, 3> &vertices = prepared.vertices;
	const TerrainStaticShadowRasterTriangle &triangle = *prepared.source;
	const float orientation = prepared.orientation;
	const float inverse_area = prepared.inverse_area;
	const int x0 = prepared.x0;
	const int x1 = prepared.x1;
	const int y0 = std::max(prepared.y0, row_begin);
	const int y1 = std::min(prepared.y1, row_end);
	constexpr int kBlock = 8;
	for (int block_y = y0; block_y < y1; block_y += kBlock) {
	const int block_y_end = std::min(block_y + kBlock, y1);
	for (int block_x = x0; block_x < x1; block_x += kBlock) {
	const int block_x_end = std::min(block_x + kBlock, x1);
	if (edge_rejects_block(vertices[1], vertices[2], orientation,
				block_x, block_x_end - 1, block_y, block_y_end - 1) ||
			edge_rejects_block(vertices[2], vertices[0], orientation,
					block_x, block_x_end - 1, block_y, block_y_end - 1) ||
			edge_rejects_block(vertices[0], vertices[1], orientation,
					block_x, block_x_end - 1, block_y, block_y_end - 1)) {
		continue;
	}
	for (int y = block_y; y < block_y_end; ++y) {
		for (int x = block_x; x < block_x_end; ++x) {
			const float sample_x = static_cast<float>(x);
			const float sample_y = static_cast<float>(y);
			const float e0 = edge(vertices[1], vertices[2],
					sample_x, sample_y) * orientation;
			const float e1 = edge(vertices[2], vertices[0],
					sample_x, sample_y) * orientation;
			const float e2 = edge(vertices[0], vertices[1],
					sample_x, sample_y) * orientation;
			if (!edge_admits(e0, prepared.edge0_inclusive) ||
					!edge_admits(e1, prepared.edge1_inclusive) ||
					!edge_admits(e2, prepared.edge2_inclusive)) {
				continue;
			}
			const float w0 = e0 * inverse_area;
			const float w1 = e1 * inverse_area;
			const float w2 = e2 * inverse_area;
			const float depth = vertices[0].depth * w0 +
					vertices[1].depth * w1 + vertices[2].depth * w2;
			const std::size_t pixel =
					static_cast<std::size_t>(y) * high_width + x;
			// Every shipped PROJSHAD declaration has normal z mode. Retail
			// therefore applies LESSEQUAL and writes depth even for alpha,
			// additive, and multiplicative material-blend variants.
			if (depth > depth_buffer[pixel]) continue;
			float source_alpha = std::clamp(triangle.alpha_scale, 0.0f, 1.0f);
			if (prepared.textured) {
				const float u = vertices[0].texture_u * w0 +
						vertices[1].texture_u * w1 +
						vertices[2].texture_u * w2;
				const float v = vertices[0].texture_v * w0 +
						vertices[1].texture_v * w1 +
						vertices[2].texture_v * w2;
				source_alpha *= sample_texture_alpha(prepared.mips, u, v);
			}
			if (triangle.alpha_test_enabled) {
				const float alpha_byte = source_alpha * 255.0f;
				if (!renderer::alpha_test_passes(alpha_byte, triangle.alpha_ref,
						triangle.alpha_test_inverted)) continue;
			}
			depth_buffer[pixel] = depth;
			uint8_t &destination = temporary[pixel];
			destination = blend_fragment(
					destination, triangle.blend, source_alpha);
		}
	}
	}
	}
}
} // namespace

TerrainStaticShadowLightDirection
terrain_static_shadow_world_light_from_environment_tuple(
		float g0, float g1, float g2) noexcept {
	// The direct getter tuple feeds renderer axes after the fixed matrix builder
	// maps BMS XYZ into (-Y,Z,X), i.e. presentation (Z,Y,X). Reducing that
	// permutation back to presentation world gives (g2,g1,g0).
	// [orig: Environment_GetLightDirectionFloat @0x57D870;
	// Math_BuildFixedPointToFloatMatrix4x4 @0x612402..0x612457]
	return TerrainStaticShadowLightDirection{g2, g1, g0};
}

bool project_terrain_static_shadow_vertex(
		const TerrainStaticShadowProjectionInput &input,
		const TerrainStaticShadowWorldVertex &world,
		TerrainStaticShadowRasterVertex &projected) noexcept {
	// RenderSlot_SetupShadowCascadeMatrices_0 divides horizontal light components by
	// the collector-clamped vertical component, renders in a page-centered
	// orthographic view, and writes depth `h*0.0005 - 0.00005`.
	// [orig: clamp @0x60D33F..0x60D341; caster terrain-relative translation
	// @0x60D8FA; page recenter @0x60D901..0x60D91A;
	// RenderSlot_SetupShadowCascadeMatrices_0 @0x58D4D3..0x58D618]
	const std::optional<TerrainTilePageProjection> page_projection =
			TerrainTileCompositionCache::page_projection(input.page);
	if (!page_projection.has_value() ||
			!std::isfinite(input.surface_to_light.x) ||
			!std::isfinite(input.surface_to_light.y) ||
			!std::isfinite(input.surface_to_light.z) ||
			!std::isfinite(input.caster_ground_y) ||
			!std::isfinite(world.x) || !std::isfinite(world.y) ||
			!std::isfinite(world.z) || !std::isfinite(world.texture_u) ||
			!std::isfinite(world.texture_v)) {
		return false;
	}
	const float vertical = std::max(
			input.surface_to_light.y, kMinimumVerticalLight);
	const float height = world.y - input.caster_ground_y;
	const float shadow_x = world.x -
			height * input.surface_to_light.x / vertical;
	const float shadow_z = world.z -
			height * input.surface_to_light.z / vertical;
	const std::array<float, 2> page_uv = page_projection->project(
			shadow_x, shadow_z);
	TerrainStaticShadowRasterVertex result;
	result.page_u = page_uv[0];
	result.page_v = page_uv[1];
	result.depth = height * kProjectedDepthScale + kProjectedDepthBias;
	result.texture_u = world.texture_u;
	result.texture_v = world.texture_v;
	if (!finite(result)) return false;
	projected = result;
	return true;
}

bool rasterize_terrain_static_shadow_alpha(
		const TerrainStaticShadowRasterInput &input,
		TerrainStaticShadowAlphaPage &page) noexcept {
	if (!validate(input, page)) return false;
	const uint32_t high_width = page.width * kTemporaryScale;
	const uint32_t high_height = page.height * kTemporaryScale;
	const std::size_t high_count =
			static_cast<std::size_t>(high_width) * high_height;
	std::vector<uint8_t> temporary;
	std::vector<uint8_t> resolved;
	std::vector<float> depth_buffer;
	try {
		temporary.resize(high_count);
		resolved.resize(page.alpha.size());
		depth_buffer.assign(high_count, input.receiver_depth);
	} catch (const std::bad_alloc &) {
		return false;
	}

	for (uint32_t y = 0; y < high_height; ++y) {
		for (uint32_t x = 0; x < high_width; ++x) {
			temporary[static_cast<std::size_t>(y) * high_width + x] =
					page.alpha[static_cast<std::size_t>(y / kTemporaryScale) *
							page.width + x / kTemporaryScale];
		}
	}

	// Every fan triangle, set up once in input order: lane k prepares the
	// k-th contiguous run of the input, and the runs are walked in lane order.
	const std::size_t lanes = input.triangles.size() >= kParallelMinimumTriangles
			? std::max<std::size_t>(input.threads, 1)
			: 1;
	std::vector<std::vector<PreparedTriangle>> runs(lanes);
	std::vector<uint8_t> run_failed(lanes, 0);
	run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
		const std::size_t count = input.triangles.size();
		const std::size_t begin = count * lane / lanes;
		const std::size_t end = count * (lane + 1) / lanes;
		std::vector<PreparedTriangle> &prepared = runs[lane];
		try {
			prepared.reserve(end - begin);
			for (std::size_t index = begin; index < end; ++index) {
				const TerrainStaticShadowRasterTriangle &triangle = input.triangles[index];
				// The D3D clip volume cuts the projected triangle at the near
				// plane before rasterization: with w = 1 under the witnessed
				// ortho, depth `h*0.0005 - 0.00005` is negative for every
				// vertex lower than 0.1 u above the caster's ground plane, so
				// buried foundations and skirts never cast; a straddling
				// triangle keeps only its part above the plane, with every
				// attribute interpolated linearly along the cut edges
				// (Sutherland-Hodgman, exactly the clipper's arithmetic).
				// [orig: RenderSlot_SetupShadowCascadeMatrices_0 @0x58D5F8 (P[10] =
				// 0.0005), @0x58D602 (P[14] = -0.00005), @0x58D60C (P[15] = 1);
				// the temp RT clear depth 0.99995 @0x60D5BF places the far side
				// likewise]
				std::array<TerrainStaticShadowRasterVertex, kClippedPolygonMax> clipped;
				const std::size_t clipped_count = clip_near_plane(
						triangle.vertices, clipped);
				for (std::size_t fan = 2; fan < clipped_count; ++fan) {
					PreparedTriangle setup;
					if (prepare_triangle(triangle,
								{&clipped[0], &clipped[fan - 1], &clipped[fan]},
								input, high_width, high_height, setup)) {
						prepared.push_back(setup);
					}
				}
			}
		} catch (const std::bad_alloc &) {
			run_failed[lane] = 1;
		}
	});
	if (std::find(run_failed.begin(), run_failed.end(), 1) != run_failed.end()) {
		return false;
	}

	// Each pixel's fragments arrive in triangle order however the rows are
	// split, so the stripes below produce the serial loop's bytes exactly.
	run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
		for (const std::vector<PreparedTriangle> &prepared : runs) {
			for (const PreparedTriangle &triangle : prepared) {
				for_lane_stripes(lane, lanes, triangle.y0, triangle.y1,
						[&](int row_begin, int row_end) {
							raster_rows(triangle, row_begin, row_end, high_width,
									temporary.data(), depth_buffer.data());
						});
			}
		}
	});

	for (uint32_t y = 0; y < page.height; ++y) {
		for (uint32_t x = 0; x < page.width; ++x) {
			unsigned sum = 0;
			for (uint32_t oy = 0; oy < kTemporaryScale; ++oy) {
				for (uint32_t ox = 0; ox < kTemporaryScale; ++ox) {
					sum += temporary[
							static_cast<std::size_t>(y * kTemporaryScale + oy) *
									high_width + x * kTemporaryScale + ox];
				}
			}
			resolved[static_cast<std::size_t>(y) * page.width + x] =
					static_cast<uint8_t>((sum + 2u) / 4u);
		}
	}
	page.alpha.swap(resolved);
	return true;
}

} // namespace opennova::terrain
