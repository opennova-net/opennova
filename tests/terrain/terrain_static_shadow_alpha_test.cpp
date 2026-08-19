// Static terrain-shadow page handoff: a device raster starts from the
// composed DOT3 alpha, changes covered samples, and commits the resulting
// light term without ever touching page RGB.
#include <terrain/terrain_static_shadow_alpha.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

opennova::terrain::Rgba8Image source_page() {
	opennova::terrain::Rgba8Image image;
	image.width = 2;
	image.height = 2;
	image.pixels = {
			10, 20, 30, 40,
			50, 60, 70, 80,
			90, 100, 110, 120,
			130, 140, 150, 160,
	};
	return image;
}

opennova::TerrainTileCompositionJob job() {
	opennova::TerrainTileCompositionJob value;
	value.target.page = {-512, -2048, 64, 128, 4};
	value.target.layer = 17;
	value.target.generation = 9;
	value.layout.texture_dimension = 2;
	value.layout.world_span = 64;
	return value;
}

bool test_begin_copies_the_composed_light_term() {
	using namespace opennova;
	using namespace opennova::terrain;
	const Rgba8Image source = source_page();
	const TerrainTileCompositionJob request = job();
	const TerrainTileContentStamp raster_stamp{UINT64_C(0x1122334455667788)};
	const TerrainStaticShadowAlphaPage page =
			begin_terrain_static_shadow_alpha_page(
					request, source, raster_stamp);
	if (!expect(page.is_valid(),
			"a raster page begins from a valid composed page")) return false;
	if (!expect(page.page.sector_origin_x == -512 &&
			page.page.sector_origin_z == -2048 &&
			page.page.page_local_x == 64 &&
			page.page.page_local_z == 128 &&
			page.page.page_lod_level == 4,
			"the alpha page carries the exact terrain page identity")) return false;
	if (!expect(page.content.value == raster_stamp.value,
			"the alpha page carries the raster contribution stamp")) return false;
	return expect(page.alpha == std::vector<uint8_t>({40, 80, 120, 160}),
			"the raster starts from the already-composed DOT3 alpha bytes");
}

bool test_apply_replaces_alpha_and_preserves_rgb() {
	using namespace opennova;
	using namespace opennova::terrain;
	const TerrainTileCompositionJob request = job();
	Rgba8Image destination = source_page();
	const Rgba8Image before = destination;
	TerrainStaticShadowAlphaPage page =
			begin_terrain_static_shadow_alpha_page(
					request, destination, TerrainTileContentStamp{77});
	// Opaque PROJSHAD coverage writes temp blue zero. A future alpha-tested
	// raster can retain the copied DOT3 byte for rejected fragments.
	page.alpha = {0, 80, 0, 160};
	if (!expect(apply_terrain_static_shadow_alpha_page(
			request, page, destination),
			"a matching final light-alpha page commits")) return false;
	for (std::size_t pixel = 0; pixel < page.alpha.size(); ++pixel) {
		const std::size_t offset = pixel * 4u;
		if (!expect(destination.pixels[offset] == before.pixels[offset] &&
				destination.pixels[offset + 1] == before.pixels[offset + 1] &&
				destination.pixels[offset + 2] == before.pixels[offset + 2],
				"static shadow commit is byte-invariant in RGB")) return false;
		if (!expect(destination.pixels[offset + 3] == page.alpha[pixel],
				"static shadow commit replaces destination alpha byte-for-byte")) {
			return false;
		}
	}
	return true;
}

bool test_invalid_page_fails_atomically() {
	using namespace opennova;
	using namespace opennova::terrain;
	const TerrainTileCompositionJob request = job();
	const Rgba8Image original = source_page();

	TerrainStaticShadowAlphaPage wrong_identity =
			begin_terrain_static_shadow_alpha_page(
					request, original, TerrainTileContentStamp{1});
	wrong_identity.page.page_local_x += 64;
	Rgba8Image destination = original;
	if (!expect(!apply_terrain_static_shadow_alpha_page(
			request, wrong_identity, destination) &&
			destination.pixels == original.pixels,
			"a page-identity mismatch rejects without partial mutation")) return false;

	TerrainStaticShadowAlphaPage wrong_size =
			begin_terrain_static_shadow_alpha_page(
					request, original, TerrainTileContentStamp{1});
	wrong_size.alpha.pop_back();
	destination = original;
	return expect(!apply_terrain_static_shadow_alpha_page(
			request, wrong_size, destination) &&
			destination.pixels == original.pixels,
			"an incomplete raster rejects without partial mutation");
}

bool test_shadow_stamp_is_domain_separated() {
	using namespace opennova;
	using namespace opennova::terrain;
	const TerrainTileContentStamp base{UINT64_C(0x123456789abcdef0)};
	const TerrainTileContentStamp first{1};
	const TerrainTileContentStamp second{2};
	const TerrainTileContentStamp combined_first =
			mix_terrain_static_shadow_content_stamp(base, first);
	const TerrainTileContentStamp combined_first_again =
			mix_terrain_static_shadow_content_stamp(base, first);
	const TerrainTileContentStamp combined_second =
			mix_terrain_static_shadow_content_stamp(base, second);
	return expect(combined_first.value == combined_first_again.value,
			"shadow stamp mixing is deterministic") &&
			expect(combined_first.value != base.value &&
					combined_first.value != combined_second.value,
			"shadow contributions occupy a distinct cache-content domain");
}

} // namespace

int main() {
	if (!test_begin_copies_the_composed_light_term()) return 1;
	if (!test_apply_replaces_alpha_and_preserves_rgb()) return 1;
	if (!test_invalid_page_fails_atomically()) return 1;
	if (!test_shadow_stamp_is_domain_separated()) return 1;
	std::puts("OK: terrain static-shadow final-alpha page handoff");
	return 0;
}
