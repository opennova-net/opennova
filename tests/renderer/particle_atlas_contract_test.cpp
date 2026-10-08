#include <runtime/renderer/particle_atlas.h>

#include <base/io/log.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace r = opennova::renderer;

bool check(bool ok, const char *message) {
	if (!ok) {
		std::fputs(message, stderr);
		std::fputc(10, stderr);
	}
	return ok;
}

bool near(float actual, float expected) {
	return std::fabs(actual - expected) <= 0.000001f;
}

r::ParticleRgbaImage solid_image(int width, int height,
		std::array<std::uint8_t, 4> rgba = {1, 2, 3, 255}) {
	r::ParticleRgbaImage image;
	image.width = width;
	image.height = height;
	image.rgba.resize(static_cast<std::size_t>(width * height * 4));
	for (int pixel = 0; pixel < width * height; ++pixel) {
		for (int channel = 0; channel < 4; ++channel) {
			image.rgba[static_cast<std::size_t>(pixel * 4 + channel)] =
					rgba[static_cast<std::size_t>(channel)];
		}
	}
	return image;
}

std::array<std::uint8_t, 4> pixel_at(const r::ParticleAtlasPage &page,
		int x, int y) {
	const std::size_t offset = static_cast<std::size_t>(
			(y * page.image.width + x) * 4);
	return {
		page.image.rgba[offset + 0],
		page.image.rgba[offset + 1],
		page.image.rgba[offset + 2],
		page.image.rgba[offset + 3],
	};
}

bool frame_registrar_contract() {
	if (!check(r::retail_particle_frame_name("BCas.TGA", 1, 1) ==
			"BCas.TGA",
			"one-frame graphics retain the authored literal name and case")) return false;
	if (!check(r::retail_particle_frame_name("FX.TGA.backup.tga", 12, 1) ==
			"fx_01.tga",
			"multi-frame names lowercase and truncate at the first .tga")) return false;
	if (!check(r::retail_particle_frame_name("CFlamet3a.TGA", 12, 9) ==
			"cflamet3a_09.tga" &&
			r::retail_particle_frame_name("CFlamet3a.TGA", 12, 10) ==
			"cflamet3a_10.tga",
			"flip frames use _0N below ten and _N from ten onward")) return false;
	if (!check(r::retail_particle_frame_name("CFlamet3a.TGA", 2, 1) ==
			"cflamet3a_01.tga",
			"multi-frame registration retains trailing letters without a fallback")) return false;
	if (!check(r::retail_particle_frame_name("Smoke.dds", 2, 1) ==
			"smoke.dds_01.tga",
			"a non-tga authored base is retained before the retail suffix")) return false;

	r::ParticleAtlasBuilder builder;
	const auto first = builder.register_frame("Smoke_01.TGA", 0,
			solid_image(1, 1, {7, 2, 3, 4}));
	const auto duplicate = builder.register_frame("smoke_01.tga", 0,
			solid_image(1, 1, {99, 2, 3, 4}));
	const auto other_type = builder.register_frame("SMOKE_01.TGA", 2,
			solid_image(1, 1, {8, 2, 3, 4}));
	if (!check(first == duplicate && first != other_type &&
			builder.entry_count() == 2,
			"frame identity is case-insensitive name plus exact graphic type")) return false;
	const r::ParticleAtlasBuild built = builder.build();
	const auto &first_entry = built.entries[first];
	const auto &first_page = built.pages[first_entry.placement.page];
	const auto first_pixel = pixel_at(first_page,
			first_entry.placement.x, first_entry.placement.y);
	return check(first_entry.name == "Smoke_01.TGA" && first_pixel[0] == 7,
			"duplicate registration is first-win for spelling and source pixels");
}

bool page_family_contract() {
	r::ParticleAtlasBuilder builder;
	std::array<r::ParticleAtlasEntryId, 8> ids{};
	for (std::uint8_t type = 0; type < ids.size(); ++type) {
		ids[type] = builder.register_frame("type" + std::to_string(type),
				type, solid_image(1, 1));
	}
	const r::ParticleAtlasBuild built = builder.build();
	for (std::uint8_t type = 0; type < ids.size(); ++type) {
		const auto &placement = built.entries[ids[type]].placement;
		if (!check(placement.valid, "every in-range graphic type is placed"))
			return false;
		const int expected_side = type <= 2 ? 1024 : 256;
		if (!check(built.pages[placement.page].image.width == expected_side &&
				built.pages[placement.page].image.height == expected_side,
				"types 0-2 use 1024 pages and types 3-7 use 256 pages")) return false;
	}
	return check(built.entries[ids[1]].placement.page ==
			built.entries[ids[2]].placement.page &&
			built.pages[built.entries[ids[1]].placement.page].type == 1,
			"graphic types 1 and 2 share the first compatible page");
}

bool stable_width_order_and_uv_contract() {
	r::ParticleAtlasBuilder builder;
	const auto narrow = builder.register_frame("narrow", 4, solid_image(1, 6));
	const auto wide_first = builder.register_frame("wide-first", 4,
			solid_image(6, 6));
	const auto wide_second = builder.register_frame("wide-second", 4,
			solid_image(6, 6));
	const auto medium = builder.register_frame("medium", 4, solid_image(3, 6));
	const r::ParticleAtlasBuild built = builder.build();
	const auto &a = built.entries[wide_first].placement;
	const auto &b = built.entries[wide_second].placement;
	const auto &c = built.entries[medium].placement;
	const auto &d = built.entries[narrow].placement;
	if (!check(a.x == 0 && b.x == 6 && c.x == 12 && d.x == 15,
			"entries place width-descending with registration-stable ties")) return false;
	return check(near(a.rect.u_min, 0.0f) &&
			near(a.rect.u_max, 6.0f / 256.0f) &&
			near(a.inset_u, 2.5f / 256.0f) &&
			near(a.inset_v, 2.5f / 256.0f),
			"placements expose exact normalized rects and the 2.5-pixel inset");
}

bool tiny_frame_inset_contract() {
	r::ParticleAtlasBuilder builder;
	const auto regular = builder.register_frame("regular", 4,
			solid_image(6, 7));
	const auto tiny = builder.register_frame("tiny", 4, solid_image(4, 2));
	const r::ParticleAtlasBuild built = builder.build();
	const auto &regular_placement = built.entries[regular].placement;
	const auto &tiny_placement = built.entries[tiny].placement;
	if (!check(near(regular_placement.inset_u, 2.5f / 256.0f) &&
			near(regular_placement.inset_v, 2.5f / 256.0f),
			"frames larger than five pixels retain the retail inset")) return false;
	if (!check(near(tiny_placement.inset_u, 2.0f / 256.0f) &&
			near(tiny_placement.inset_v, 1.0f / 256.0f),
			"tiny-frame inset is capped independently at each midpoint")) return false;
	return check(tiny_placement.rect.u_min + tiny_placement.inset_u <=
				tiny_placement.rect.u_max - tiny_placement.inset_u &&
			tiny_placement.rect.v_min + tiny_placement.inset_v <=
				tiny_placement.rect.v_max - tiny_placement.inset_v,
			"tiny-frame contracted UV bounds never invert");
}

bool allocator_control_flow_contract() {
	std::vector<int> right_edge(8, 0);
	const auto strict = r::allocate_retail_particle_atlas_rect(
			right_edge, 8, 8, 1);
	if (!check(!strict.valid && strict.candidate_starts == 0,
			"an entry touching the strict right edge is rejected before scanning"))
		return false;

	std::vector<int> double_skip = {8, 8, 8, 0, 0, 0, 0, 0};
	const auto skipped = r::allocate_retail_particle_atlas_rect(
			double_skip, 8, 1, 8);
	if (!check(skipped.valid && skipped.x == 4 && skipped.y == 0 &&
			skipped.rejected_starts == 2 && skipped.candidate_starts == 5,
			"a failed scan advances past both the failing and following starts"))
		return false;

	std::vector<int> persistent_min = {0, 4, 4, 4, 0, 0, 0, 0};
	const auto persistent = r::allocate_retail_particle_atlas_rect(
			persistent_min, 8, 1, 5);
	return check(persistent.valid && persistent.x == 0 &&
			persistent.rejected_starts == 0 && persistent.candidate_starts == 7,
			"the minimum skyline value persists across every candidate start");
}

std::string g_logged;
void capture_log(opennova::io::LogLevel level, const char *message) {
	if (level >= opennova::io::LogLevel::kWarn) g_logged += std::string(message) + "\n";
}

bool strict_page_edge_contract() {
	r::ParticleAtlasBuilder builder;
	const auto rejected = builder.register_frame("full", 4,
			solid_image(256, 1));
	const auto accepted = builder.register_frame("almost-full", 4,
			solid_image(255, 1));
	const auto missing = builder.register_frame("missing", 4, r::ParticleRgbaImage{});
	g_logged.clear();
	opennova::io::set_log_sink(capture_log);
	const r::ParticleAtlasBuild built = builder.build();
	opennova::io::set_log_sink(nullptr);
	// A graphic no empty page holds hangs retail's build; the port leaves it undrawn,
	// lists it and names it on the log (D-PTL-31). An empty image is rejected too,
	// as retail packs only what its size probe found, but it is no hang.
	// [orig: CParticleManager_BuildTextureAtlases @ 0x5E8DB0, @ 0x5E9185 / @ 0x5E91BB]
	return check(!built.entries[rejected].placement.valid &&
			built.entries[accepted].placement.valid &&
			built.entries[accepted].placement.x == 0 &&
			!built.entries[missing].placement.valid &&
			built.rejected_entries == 2 &&
			built.oversized_entries.size() == 1 && built.oversized_entries[0] == rejected &&
			g_logged.find("particle atlas: full (256x1, type 4) fits no empty 256x256 page") !=
					std::string::npos &&
			g_logged.find("missing") == std::string::npos,
			"page packing preserves the allocator's strict right edge; an oversized graphic is named");
}

bool overlapping_skyline_candidate_contract() {
	r::ParticleAtlasBuilder builder;
	const auto shelf = builder.register_frame("shelf", 4,
			solid_image(100, 1, {10, 0, 0, 255}));
	const auto tower = builder.register_frame("tower", 4,
			solid_image(100, 250, {20, 0, 0, 255}));
	const auto short_first = builder.register_frame("short-first", 4,
			solid_image(60, 1, {30, 0, 0, 255}));
	const auto short_second = builder.register_frame("short-second", 4,
			solid_image(60, 1, {40, 0, 0, 255}));
	const r::ParticleAtlasBuild built = builder.build();
	const auto &shelf_placement = built.entries[shelf].placement;
	const auto &tower_placement = built.entries[tower].placement;
	const auto &first_placement = built.entries[short_first].placement;
	const auto &second_placement = built.entries[short_second].placement;
	if (!check(shelf_placement.valid && tower_placement.valid &&
			first_placement.valid && second_placement.valid,
			"the skyline-overlap sequence keeps every entry admitted")) return false;
	if (!check(built.pages.size() == 2 &&
			shelf_placement.page == tower_placement.page &&
			first_placement.page == tower_placement.page &&
			second_placement.page != tower_placement.page &&
			shelf_placement.x == 0 && shelf_placement.y == 0 &&
			tower_placement.x == 100 && tower_placement.y == 0 &&
			first_placement.x == 0 && first_placement.y == 1 &&
			second_placement.x == 0 && second_placement.y == 0,
			"an intersecting retail candidate falls back to a fresh page")) return false;
	const auto tower_pixel = pixel_at(built.pages[tower_placement.page],
			tower_placement.x, tower_placement.y + 1);
	return check(tower_pixel == std::array<std::uint8_t, 4>{20, 0, 0, 255},
			"fallback placement cannot overwrite an occupied atlas rectangle");
}

r::ParticleRgbaImage height_image() {
	r::ParticleRgbaImage image;
	image.width = 4;
	image.height = 1;
	image.rgba = {
		1, 2, 16, 11,
		1, 2, 0, 22,
		1, 2, 16, 33,
		1, 2, 0, 44,
	};
	return image;
}

bool raw_rgba_preprocess_contract() {
	r::ParticleAtlasBuilder builder;
	const auto additive = builder.register_frame("add", 1,
			solid_image(1, 1, {10, 20, 30, 77}));
	const auto premult = builder.register_frame("premult", 2,
			solid_image(1, 1, {40, 50, 60, 88}));
	const auto bump = builder.register_frame("bump", 3, height_image());
	const auto bumpadd = builder.register_frame("bumpadd", 6, height_image());
	const auto distort = builder.register_frame("distort", 7, height_image());
	const r::ParticleAtlasBuild built = builder.build();

	auto entry_pixel = [&](r::ParticleAtlasEntryId id, int local_x) {
		const auto &placement = built.entries[id].placement;
		return pixel_at(built.pages[placement.page],
				placement.x + local_x, placement.y);
	};
	const auto additive_pixel = entry_pixel(additive, 0);
	const auto premult_pixel = entry_pixel(premult, 0);
	if (!check(additive_pixel == std::array<std::uint8_t, 4>{10, 20, 30, 0} &&
			premult_pixel == std::array<std::uint8_t, 4>{40, 50, 60, 88},
			"type 1 clears source alpha while compatible type 2 retains it"))
		return false;

	const auto bump_wrap = entry_pixel(bump, 0);
	const auto bump_slope = entry_pixel(bump, 3);
	const auto bumpadd_slope = entry_pixel(bumpadd, 3);
	if (!check(bump_wrap == std::array<std::uint8_t, 4>{127, 127, 255, 11} &&
			bump_slope == std::array<std::uint8_t, 4>{217, 127, 217, 44} &&
			bumpadd_slope == bump_slope,
			"types 3 and 6 use wrapped blue heights at scale 0.125")) return false;

	const auto distort_slope = entry_pixel(distort, 3);
	return check(distort_slope ==
			std::array<std::uint8_t, 4>{158, 127, 255, 44},
			"type 7 uses scale 0.03125, forces blue to 255, and retains alpha");
}

// The page's device levels [orig: CParticleTexture_InitTextureAndChannels
// @ 0x5E82B2..0x5E82EB, 0x180000 on the full-quality branch;
// GTexture_CreateFromPixelData_0 @ 0x6877F2..0x687801 (at most three
// levels), @ 0x6878BE (D3DXFilterTexture BOX, each level from the one
// before, stored as A8R8G8B8 bytes)]: three levels for both page sides,
// each texel the rounded mean of the four above it.
bool page_levels_contract() {
	r::ParticleRgbaImage big = solid_image(1024, 1024, {0, 0, 0, 0});
	// One 2x2 block at the origin: three zero texels and a full one on R and
	// A, the mirror on G; B a ramp 10/20/30/41.
	const auto put = [&big](int x, int y, std::array<std::uint8_t, 4> rgba) {
		const std::size_t at = static_cast<std::size_t>((y * big.width + x) * 4);
		for (int c = 0; c < 4; ++c)
			big.rgba[at + static_cast<std::size_t>(c)] = rgba[static_cast<std::size_t>(c)];
	};
	put(0, 0, {0, 255, 10, 0});
	put(1, 0, {0, 255, 20, 0});
	put(0, 1, {0, 255, 30, 0});
	put(1, 1, {255, 0, 41, 255});
	const r::ParticleAtlasPageTexture texture =
			r::particle_atlas_page_texture(big, r::kParticlePageCreationFlags);
	const std::vector<r::ParticleRgbaImage> &levels = texture.rgba_levels;
	if (!check(r::kParticlePageCreationFlags == 0x180000u &&
					texture.format == r::TextureDxtFormat::None && levels.size() == 3 &&
					texture.side() == 1024 && texture.level_count() == 3 &&
					levels[0].width == 1024 && levels[1].width == 512 &&
					levels[1].height == 512 && levels[2].width == 256 &&
					levels[1].valid() && levels[2].valid(),
				"a 1024 page carries three levels: 1024, 512, 256"))
		return false;
	// (0 + 0 + 0 + 1) / 4 x 255 = 63.75, +0.5 -> 64; (3 x 255) / 4 = 191.25 -> 191;
	// (10 + 20 + 30 + 41) / 4 = 25.25 -> 25.
	const std::uint8_t *texel = levels[1].rgba.data();
	if (!check(texel[0] == 64 && texel[1] == 191 && texel[2] == 25 && texel[3] == 64,
				"level 1 is D3DX's box filter of level 0, rounded half up"))
		return false;
	// Level 2 filters level 1's stored bytes: 64 and three zeros -> 16.
	if (!check(levels[2].rgba[0] == 16 && levels[2].rgba[3] == 16,
				"level 2 filters the bytes level 1 was stored as"))
		return false;
	const r::ParticleAtlasPageTexture small = r::particle_atlas_page_texture(
			solid_image(256, 256, {9, 8, 7, 6}), r::kParticlePageCreationFlags);
	if (!check(small.rgba_levels.size() == 3 && small.rgba_levels[2].width == 64 &&
					small.rgba_levels[2].rgba[0] == 9 && small.rgba_levels[2].rgba[3] == 6,
				"a 256 page carries three levels: 256, 128, 64"))
		return false;
	return check(r::particle_atlas_page_last_level(1024, r::kParticlePageCreationFlags) == 2 &&
					r::particle_atlas_page_last_level(256, r::kParticlePageCreationFlags) == 2 &&
					r::particle_atlas_page_texture(r::ParticleRgbaImage{},
							r::kParticlePageCreationFlags).level_count() == 0,
			"both page sides stop at level 2; an empty page has no levels");
}

// The page flags under the session's two words (D-RMAT-24): 0x200 at a
// texcompression_level of 1 or less, 0x10000 at a particle_density of 1 or
// less [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82CC].
bool page_flags_contract() {
	return check(r::particle_atlas_page_creation_flags(2, 2) == 0x180000u &&
					r::particle_atlas_page_creation_flags(1, 2) == 0x180200u &&
					r::particle_atlas_page_creation_flags(0, 2) == 0x180200u &&
					r::particle_atlas_page_creation_flags(-4, 2) == 0x180200u &&
					r::particle_atlas_page_creation_flags(7, 2) == 0x180000u &&
					r::particle_atlas_page_creation_flags(2, 1) == 0x190000u &&
					r::particle_atlas_page_creation_flags(2, 0) == 0x190000u &&
					r::particle_atlas_page_creation_flags(1, 1) == 0x190200u,
			"the page flags follow the two session words");
}

// A particle_density of 1 or less halves the page once with
// GTexture_Downsample2x2_RGBA8 (each channel the truncated mean) before its
// three levels [orig: GTexture_DownsampleToLimits @ 0x6871DB; @ 0x687000].
bool page_halving_contract() {
	r::ParticleRgbaImage big = solid_image(1024, 1024, {0, 0, 0, 0});
	big.rgba[3] = 255; // one texel of the first 2x2 block opaque
	const r::ParticleAtlasPageTexture texture = r::particle_atlas_page_texture(big, 0x190000u);
	if (!check(texture.format == r::TextureDxtFormat::None && texture.side() == 512 &&
					texture.rgba_levels.size() == 3 && texture.rgba_levels[2].width == 128,
				"a halved 1024 page carries 512, 256 and 128"))
		return false;
	if (!check(texture.rgba_levels[0].rgba[3] == 63,
				"the halving truncates (255 / 4 = 63), where D3DX's filter rounds"))
		return false;
	return check(r::particle_atlas_page_last_level(1024, 0x190000u) == 2 &&
					r::particle_atlas_page_texture(solid_image(256, 256), 0x190000u).side() == 128,
			"the halved pages still stop at level 2");
}

// A texcompression_level of 1 or less makes the page DXT5: level 0 the page
// through D3DX's encoder, every later level the box filter of the level
// before as its blocks decode [orig: GTexture_CreateFromPixelData_0
// @ 0x687717..0x687727, @ 0x6878A0..0x6878BE].
bool page_dxt5_contract() {
	r::ParticleRgbaImage page = solid_image(256, 256, {200, 100, 50, 255});
	for (std::size_t i = 0; i < page.rgba.size(); i += 4) {
		const std::size_t texel = i / 4;
		page.rgba[i] = static_cast<std::uint8_t>(texel % 251);
		page.rgba[i + 3] = static_cast<std::uint8_t>((texel * 7) % 256);
	}
	const r::ParticleAtlasPageTexture texture = r::particle_atlas_page_texture(page, 0x180200u);
	const std::vector<r::DxtSurface> expected =
			r::build_dxt_texture_levels(page.rgba.data(), 256, 256, r::TextureDxtFormat::Dxt5, 3);
	if (!check(texture.format == r::TextureDxtFormat::Dxt5 && texture.rgba_levels.empty() &&
					texture.dxt_levels.size() == 3 && texture.side() == 256 &&
					texture.dxt_levels[2].width == 64 &&
					texture.dxt_levels[0].blocks == expected[0].blocks &&
					texture.dxt_levels[2].blocks == expected[2].blocks,
				"a compressed page is D3DX's DXT5 chain of three levels"))
		return false;
	const r::ParticleAtlasPageTexture both = r::particle_atlas_page_texture(page, 0x190200u);
	return check(both.format == r::TextureDxtFormat::Dxt5 && both.side() == 128 &&
					both.dxt_levels.size() == 3 && both.dxt_levels[2].width == 32,
			"a compressed, halved page is DXT5 from 128 down to 32");
}

} // namespace

int main() {
	if (!frame_registrar_contract()) return 1;
	if (!page_family_contract()) return 1;
	if (!stable_width_order_and_uv_contract()) return 1;
	if (!tiny_frame_inset_contract()) return 1;
	if (!allocator_control_flow_contract()) return 1;
	if (!strict_page_edge_contract()) return 1;
	if (!overlapping_skyline_candidate_contract()) return 1;
	if (!raw_rgba_preprocess_contract()) return 1;
	if (!page_levels_contract()) return 1;
	if (!page_flags_contract()) return 1;
	if (!page_halving_contract()) return 1;
	if (!page_dxt5_contract()) return 1;
	std::puts("renderer_particle_atlas_contract_test ok");
	return 0;
}
