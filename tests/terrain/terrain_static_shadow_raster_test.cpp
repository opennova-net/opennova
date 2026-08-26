// Static terrain-shadow page raster: retail renders black projected geometry
// into a 2x temporary DOT3-light surface, then linearly resolves temp blue into
// the destination page alpha. These synthetic pins cover opaque, depth-tested,
// and material-alpha-tested fragments without any Godot dependency.
#include <terrain/terrain_static_shadow_raster.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova::terrain;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

TerrainStaticShadowRasterVertex vertex(float page_u, float page_v,
		float depth, float texture_u = 0.0f, float texture_v = 0.0f) {
	return {page_u, page_v, depth, texture_u, texture_v};
}

TerrainStaticShadowRasterTriangle triangle(
		TerrainStaticShadowRasterVertex a,
		TerrainStaticShadowRasterVertex b,
		TerrainStaticShadowRasterVertex c,
		int alpha_texture = -1, uint8_t alpha_ref = 0) {
	TerrainStaticShadowRasterTriangle value;
	value.vertices = {a, b, c};
	value.alpha_texture_index = alpha_texture;
	value.alpha_ref = alpha_ref;
	value.alpha_test_enabled = alpha_texture >= 0;
	return value;
}

std::array<TerrainStaticShadowRasterTriangle, 2> rectangle(
		float left, float top, float right, float bottom,
		float depth, int alpha_texture = -1, uint8_t alpha_ref = 0) {
	const auto tl = vertex(left, top, depth, 0.25f, 0.25f);
	const auto tr = vertex(right, top, depth, 0.75f, 0.25f);
	const auto bl = vertex(left, bottom, depth, 0.25f, 0.75f);
	const auto br = vertex(right, bottom, depth, 0.75f, 0.75f);
	return {triangle(tl, tr, br, alpha_texture, alpha_ref),
			triangle(tl, br, bl, alpha_texture, alpha_ref)};
}

TerrainStaticShadowAlphaTextureView constant_alpha_texture(uint8_t alpha,
		std::vector<uint8_t> &storage,
		std::vector<TerrainStaticShadowAlphaMipView> &mips) {
	storage.assign(4, alpha);
	mips = {{2, 2, 2, storage.data()}};
	TerrainStaticShadowAlphaTextureView texture;
	texture.mips = mips.data();
	texture.mip_count = mips.size();
	texture.mip_filter = TerrainStaticShadowMipFilter::Point;
	return texture;
}

TerrainStaticShadowAlphaPage alpha_page(uint32_t width, uint32_t height,
		std::vector<uint8_t> alpha) {
	TerrainStaticShadowAlphaPage page;
	page.page.page_lod_level = 4;
	page.width = width;
	page.height = height;
	page.alpha = std::move(alpha);
	return page;
}

bool test_opaque_projection_preserves_outside_and_resolves_edges() {
	TerrainStaticShadowRasterInput input;
	TerrainStaticShadowAlphaPage result = alpha_page(4, 4, {
			40,  50,  60,  70,
			80,  90, 100, 110,
			120, 130, 140, 150,
			160, 170, 180, 190});

	// This rectangle begins/ends through destination pixel centers. At the
	// witnessed 2x temporary resolution, each edge pixel has two admitted and
	// two untouched samples, while the middle is fully black.
	const auto draws = rectangle(0.375f, 0.25f, 0.875f, 0.75f, 0.25f);
	input.triangles.assign(draws.begin(), draws.end());
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"valid opaque projection rasterizes")) return false;

	if (!expect(result.is_valid() && result.width == 4 && result.height == 4,
			"valid raster produces one destination-resolution alpha page")) return false;
	if (!expect(result.alpha[0] == 40 && result.alpha[3] == 70 &&
			result.alpha[12] == 160 && result.alpha[15] == 190,
			"samples outside the silhouette retain the composed DOT3 alpha")) return false;
	if (!expect(result.alpha[6] == 0 && result.alpha[10] == 0,
			"fully covered samples replace DOT3 alpha with black")) return false;
	return expect(result.alpha[5] == 45 && result.alpha[9] == 65 &&
			result.alpha[7] == 55 && result.alpha[11] == 75,
			"2x temporary coverage linearly resolves fractional edge alpha");
}

bool test_depth_and_winding_admission() {
	TerrainStaticShadowRasterInput input;
	TerrainStaticShadowAlphaPage result = alpha_page(2, 2,
			std::vector<uint8_t>(4, 200));
	const auto behind = rectangle(0.0f, 0.0f, 1.0f, 1.0f, 0.75f);
	input.triangles.assign(behind.begin(), behind.end());
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"depth-rejected projection is still a valid raster job")) return false;
	if (!expect(std::all_of(result.alpha.begin(), result.alpha.end(),
			[](uint8_t value) { return value == 200; }),
			"fragments behind the z=0.5 receiver quad do not shadow")) return false;

	// Reversed input still rasterizes when the authored material is two-sided.
	input.triangles.clear();
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, 0.5f), vertex(0.0f, 1.0f, 0.5f),
			vertex(1.0f, 1.0f, 0.5f)));
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, 0.5f), vertex(1.0f, 1.0f, 0.5f),
			vertex(1.0f, 0.0f, 0.5f)));
	for (TerrainStaticShadowRasterTriangle &draw : input.triangles) {
		draw.two_sided = true;
	}
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"clockwise projection rasterizes")) return false;
	return expect(std::all_of(result.alpha.begin(), result.alpha.end(),
			[](uint8_t value) { return value == 0; }),
			"clockwise fragments at the receiver depth are admitted");
}

bool test_one_sided_cull_and_two_sided_override() {
	TerrainStaticShadowRasterInput input;
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, 0.25f), vertex(0.0f, 1.0f, 0.25f),
			vertex(1.0f, 1.0f, 0.25f)));
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, 0.25f), vertex(1.0f, 1.0f, 0.25f),
			vertex(1.0f, 0.0f, 0.25f)));
	TerrainStaticShadowAlphaPage one_sided = alpha_page(1, 1, {200});
	if (!expect(rasterize_terrain_static_shadow_alpha(input, one_sided),
			"one-sided reversed geometry is a valid raster job")) return false;
	if (!expect(one_sided.alpha == std::vector<uint8_t>({200}),
			"retail CULLMODE CCW rejects the reversed one-sided silhouette")) {
		return false;
	}
	for (TerrainStaticShadowRasterTriangle &draw : input.triangles) {
		draw.two_sided = true;
	}
	TerrainStaticShadowAlphaPage two_sided = alpha_page(1, 1, {200});
	if (!expect(rasterize_terrain_static_shadow_alpha(input, two_sided),
			"the two-sided override rasterizes reversed geometry")) return false;
	return expect(two_sided.alpha == std::vector<uint8_t>({0}),
			"the authored two-sided flag switches the projected pass to cull-none");
}

bool test_projshad_depth_writes_order_overlapping_surfaces() {
	TerrainStaticShadowRasterInput input;
	const auto near_noop = rectangle(0.0f, 0.0f, 1.0f, 1.0f, 0.2f);
	const auto farther_opaque = rectangle(0.0f, 0.0f, 1.0f, 1.0f, 0.4f);
	input.triangles.assign(near_noop.begin(), near_noop.end());
	for (TerrainStaticShadowRasterTriangle &draw : input.triangles) {
		draw.blend = TerrainStaticShadowBlend::Additive;
	}
	input.triangles.insert(input.triangles.end(), farther_opaque.begin(),
			farther_opaque.end());
	TerrainStaticShadowAlphaPage result = alpha_page(1, 1, {200});
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"overlapping projected-shadow surfaces rasterize")) return false;
	if (!expect(result.alpha == std::vector<uint8_t>({200}),
			"a nearer additive no-op still depth-occludes a farther opaque caster")) {
		return false;
	}

	// Alpha-test rejection happens before the z write, so a farther admitted
	// caster remains visible through a cutout hole.
	std::vector<uint8_t> storage;
	std::vector<TerrainStaticShadowAlphaMipView> mips;
	input.alpha_textures.push_back(constant_alpha_texture(0, storage, mips));
	input.triangles.clear();
	const auto rejected_near = rectangle(0.0f, 0.0f, 1.0f, 1.0f,
			0.2f, 0, 64);
	input.triangles.assign(rejected_near.begin(), rejected_near.end());
	input.triangles.insert(input.triangles.end(), farther_opaque.begin(),
			farther_opaque.end());
	result = alpha_page(1, 1, {200});
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"alpha-rejected near geometry preserves later depth admission")) {
		return false;
	}
	return expect(result.alpha == std::vector<uint8_t>({0}),
			"a cutout hole does not hide the farther opaque silhouette");
}

bool test_near_plane_clip() {
	// Geometry below the caster's ground plane (depth < 0: h < 0.1 u) is cut
	// by the D3D near plane before rasterization, so a fully buried rectangle
	// leaves the page untouched.
	TerrainStaticShadowRasterInput input;
	TerrainStaticShadowAlphaPage result = alpha_page(4, 4,
			std::vector<uint8_t>(16, 200));
	const auto buried = rectangle(0.0f, 0.0f, 1.0f, 1.0f, -0.1f);
	input.triangles.assign(buried.begin(), buried.end());
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"a buried projection is still a valid raster job")) return false;
	if (!expect(std::all_of(result.alpha.begin(), result.alpha.end(),
			[](uint8_t value) { return value == 200; }),
			"fragments below the near plane never shadow")) return false;

	// A rectangle whose top edge sits at depth -0.25 and bottom edge at +0.25
	// crosses the plane at v = 0.5: the upper two rows stay untouched, the
	// lower two are fully covered, and the cut edge lands exactly on the
	// destination row boundary.
	input.triangles.clear();
	const auto tl = vertex(0.0f, 0.0f, -0.25f);
	const auto tr = vertex(1.0f, 0.0f, -0.25f);
	const auto bl = vertex(0.0f, 1.0f, 0.25f);
	const auto br = vertex(1.0f, 1.0f, 0.25f);
	input.triangles.push_back(triangle(tl, tr, br));
	input.triangles.push_back(triangle(tl, br, bl));
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"a straddling projection rasterizes")) return false;
	for (std::size_t index = 0; index < 8; ++index) {
		if (!expect(result.alpha[index] == 200,
				"rows above the near-plane cut keep the composed alpha")) return false;
	}
	for (std::size_t index = 8; index < 16; ++index) {
		if (!expect(result.alpha[index] == 0,
				"rows below the near-plane cut are fully covered")) return false;
	}

	// The cut interpolates the alpha-test texture coordinate too: a clipped
	// triangle keeps sampling its material where the surviving part lies.
	std::vector<uint8_t> storage;
	std::vector<TerrainStaticShadowAlphaMipView> mips;
	input.alpha_textures.push_back(constant_alpha_texture(255, storage, mips));
	input.triangles.clear();
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, -0.25f, 0.0f, 0.0f),
			vertex(1.0f, 0.0f, -0.25f, 1.0f, 0.0f),
			vertex(1.0f, 1.0f, 0.25f, 1.0f, 1.0f), 0, 64));
	input.triangles.push_back(triangle(
			vertex(0.0f, 0.0f, -0.25f, 0.0f, 0.0f),
			vertex(1.0f, 1.0f, 0.25f, 1.0f, 1.0f),
			vertex(0.0f, 1.0f, 0.25f, 0.0f, 1.0f), 0, 64));
	result = alpha_page(4, 4, std::vector<uint8_t>(16, 200));
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"a straddling alpha-tested projection rasterizes")) return false;
	return expect(result.alpha[4] == 200 && result.alpha[8] == 0 &&
			result.alpha[15] == 0,
			"alpha-tested fragments survive only below the cut");
}

bool test_material_alpha_ref_64() {
	std::vector<uint8_t> rejected_storage;
	std::vector<uint8_t> admitted_storage;
	std::vector<TerrainStaticShadowAlphaMipView> rejected_mips;
	std::vector<TerrainStaticShadowAlphaMipView> admitted_mips;
	std::vector<TerrainStaticShadowAlphaTextureView> textures;
	textures.push_back(constant_alpha_texture(
			64, rejected_storage, rejected_mips));
	textures.push_back(constant_alpha_texture(
			65, admitted_storage, admitted_mips));

	TerrainStaticShadowRasterInput input;
	input.alpha_textures = textures;
	TerrainStaticShadowAlphaPage result = alpha_page(2, 1, {180, 220});
	const auto rejected = rectangle(0.0f, 0.0f, 0.5f, 1.0f,
			0.25f, 0, 64);
	const auto admitted = rectangle(0.5f, 0.0f, 1.0f, 1.0f,
			0.25f, 1, 64);
	input.triangles.assign(rejected.begin(), rejected.end());
	input.triangles.insert(input.triangles.end(), admitted.begin(), admitted.end());
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"material-alpha-tested projections rasterize")) return false;
	return expect(result.alpha == std::vector<uint8_t>({180, 0}),
			"AMODE_NORMAL material test admits alpha > ref and rejects alpha == ref");
}

bool test_shared_diagonal_uses_single_fragment_ownership() {
	TerrainStaticShadowRasterInput input;
	TerrainStaticShadowAlphaPage result = alpha_page(1, 1, {200});
	const auto draws = rectangle(0.0f, 0.0f, 1.0f, 1.0f, 0.25f);
	input.triangles.assign(draws.begin(), draws.end());
	for (TerrainStaticShadowRasterTriangle &draw : input.triangles) {
		draw.blend = TerrainStaticShadowBlend::Alpha;
		draw.alpha_scale = 0.5f;
	}
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"adjacent alpha-blended projection triangles rasterize")) return false;
	return expect(result.alpha == std::vector<uint8_t>({100}),
			"the D3D top-left rule gives a shared diagonal to exactly one triangle");
}

bool test_inverted_material_alpha_ref() {
	std::vector<uint8_t> low_storage;
	std::vector<uint8_t> high_storage;
	std::vector<TerrainStaticShadowAlphaMipView> low_mips;
	std::vector<TerrainStaticShadowAlphaMipView> high_mips;
	TerrainStaticShadowRasterInput input;
	input.alpha_textures.push_back(constant_alpha_texture(
			64, low_storage, low_mips));
	input.alpha_textures.push_back(constant_alpha_texture(
			65, high_storage, high_mips));
	const auto admitted = rectangle(0.0f, 0.0f, 0.5f, 1.0f,
			0.25f, 0, 64);
	const auto rejected = rectangle(0.5f, 0.0f, 1.0f, 1.0f,
			0.25f, 1, 64);
	input.triangles.assign(admitted.begin(), admitted.end());
	input.triangles.insert(input.triangles.end(), rejected.begin(), rejected.end());
	for (TerrainStaticShadowRasterTriangle &draw : input.triangles) {
		draw.alpha_test_inverted = true;
	}
	TerrainStaticShadowAlphaPage result = alpha_page(2, 1, {180, 220});
	if (!expect(rasterize_terrain_static_shadow_alpha(input, result),
			"inverted material-alpha-tested projections rasterize")) return false;
	return expect(result.alpha == std::vector<uint8_t>({0, 220}),
			"inverted alpha test admits alpha <= ref and rejects alpha > ref");
}

bool test_invalid_views_fail_closed() {
	TerrainStaticShadowRasterInput input;
	TerrainStaticShadowAlphaPage page = alpha_page(2, 2,
			std::vector<uint8_t>(3, 255));
	if (!expect(!rasterize_terrain_static_shadow_alpha(input, page),
			"mis-sized base alpha rejects the raster job")) return false;

	page = alpha_page(2, 2, std::vector<uint8_t>(4, 255));
	const std::vector<uint8_t> original = page.alpha;
	input.triangles.push_back(triangle(vertex(0.0f, 0.0f, 0.25f),
			vertex(1.0f, 0.0f, 0.25f), vertex(0.0f, 1.0f, 0.25f), 4, 64));
	return expect(!rasterize_terrain_static_shadow_alpha(input, page) &&
			page.alpha == original,
			"a missing alpha texture rejects atomically without corrupting the page");
}

bool test_retail_world_to_page_projection() {
	const TerrainStaticShadowLightDirection world_light =
			terrain_static_shadow_world_light_from_environment_tuple(
					0.1f, 0.8f, -0.3f);
	if (!expect(std::abs(world_light.x + 0.3f) < 1.0e-6f &&
			std::abs(world_light.y - 0.8f) < 1.0e-6f &&
			std::abs(world_light.z - 0.1f) < 1.0e-6f,
			"direct environment tuple maps (g2,g1,g0) into Godot world axes")) {
		return false;
	}

	TerrainStaticShadowProjectionInput projection;
	projection.page = opennova::TerrainTilePageKey{100, -200, 32, 64, 4};
	projection.surface_to_light = {0.5f, 1.0f, -0.25f};
	projection.caster_ground_y = 10.0f;
	TerrainStaticShadowWorldVertex world;
	world.x = 164.0f;
	world.y = 26.0f;
	world.z = -104.0f;
	world.texture_u = 1.25f;
	world.texture_v = -0.5f;
	TerrainStaticShadowRasterVertex raster;
	if (!expect(project_terrain_static_shadow_vertex(
			projection, world, raster),
			"finite world vertex projects into a valid level-4 page")) return false;
	if (!expect(std::abs(raster.page_u - 0.375f) < 1.0e-6f &&
			std::abs(raster.page_v - 0.5625f) < 1.0e-6f,
			"height projects opposite world surface-to-light into page UV")) return false;
	if (!expect(std::abs(raster.depth - 0.00795f) < 1.0e-6f,
			"projected depth preserves the retail 0.0005 scale and -0.00005 bias")) {
		return false;
	}
	if (!expect(raster.texture_u == world.texture_u &&
			raster.texture_v == world.texture_v,
			"projection preserves material UV for the alpha-test sampler")) return false;

	projection.surface_to_light = {0.5f, 0.1f, 0.0f};
	projection.caster_ground_y = 18.0f;
	world.y = 26.0f;
	if (!expect(project_terrain_static_shadow_vertex(
			projection, world, raster) &&
			std::abs(raster.page_u - 0.25f) < 1.0e-6f,
			"projection clamps the live vertical light component to retail 0.25")) {
		return false;
	}

	projection.page.page_lod_level = 0;
	return expect(!project_terrain_static_shadow_vertex(
			projection, world, raster),
			"invalid page identity rejects projection without guessed dimensions");
}

} // namespace

int main() {
	if (!test_opaque_projection_preserves_outside_and_resolves_edges()) return 1;
	if (!test_depth_and_winding_admission()) return 1;
	if (!test_one_sided_cull_and_two_sided_override()) return 1;
	if (!test_projshad_depth_writes_order_overlapping_surfaces()) return 1;
	if (!test_near_plane_clip()) return 1;
	if (!test_material_alpha_ref_64()) return 1;
	if (!test_shared_diagonal_uses_single_fragment_ownership()) return 1;
	if (!test_inverted_material_alpha_ref()) return 1;
	if (!test_invalid_views_fail_closed()) return 1;
	if (!test_retail_world_to_page_projection()) return 1;
	std::puts("OK: terrain static-shadow 2x projection/alpha-test raster");
	return 0;
}
