// The texture roles (renderer/texture_roles.h): a row per role in the enum's order, each with the
// loader that opens its file, the formats that loader takes, its size rule and whether it reads the
// alpha, the research's table pinned here; the def texture fields' roles; which row's alpha the
// material's alpha test falls on, and what a model row's alpha is to its material's technique.

#include <runtime/renderer/texture_roles.h>

#include <formats/threedi/threedi_3di3.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::renderer;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

void test_catalog() {
	CHECK(kTextureRoleCount == 46, "46 roles");
	for (size_t i = 0; i < kTextureRoleCount; ++i)
		CHECK(static_cast<size_t>(texture_role(static_cast<TextureRoleId>(i)).id) == i, "a row each, in the enum's order");
	// The rules the research witnessed, pinned.
	const TextureRole &colour = texture_role(TextureRoleId::TerrainColourMap);
	CHECK(colour.loader == TextureLoader::Tga && colour.size == TextureSizeRule::Exact && colour.width == 1024 &&
			colour.height == 1024, "the colour map: the TGA reader, exactly 1024 x 1024");
	CHECK(texture_role_takes(colour, ".TGA") && !texture_role_takes(colour, ".dds") && !texture_role_takes(colour, ".pcx"),
			"the colour map takes a TGA alone");
	const TextureRole &foliage = texture_role(TextureRoleId::TerrainFoliageMap);
	CHECK(foliage.size == TextureSizeRule::SquarePowerOfTwoAtMost && foliage.width == 1024 && !foliage.reads_alpha &&
			foliage.loader == TextureLoader::Pcx8, "the foliage map: an 8-bit PCX, square, a power of two, at most 1024");
	CHECK(texture_role(TextureRoleId::TerrainTileAtlas).size == TextureSizeRule::MultipleOf &&
			texture_role(TextureRoleId::TerrainTileAtlas).width == 64, "the tile atlas in 64-texel cells");
	const TextureRole &normal = texture_role(TextureRoleId::ModelNormalMap);
	CHECK(normal.size == TextureSizeRule::AtMost && normal.width == 512 && normal.loader == TextureLoader::Normal &&
			texture_role_takes(normal, ".mdt") && !texture_role_takes(normal, ".pcx"), "a normal map halved to 512");
	CHECK(texture_role(TextureRoleId::LoadingScreen).size == TextureSizeRule::Exact &&
			texture_role(TextureRoleId::LoadingScreen).width == 800 &&
			texture_role(TextureRoleId::LoadingScreen).height == 600, "the loading screen: 800 x 600");
	CHECK(texture_role_takes(texture_role(TextureRoleId::ParticleGraphic), ".tga") &&
			!texture_role_takes(texture_role(TextureRoleId::ParticleGraphic), ".dds"), "a particle graphic: a TGA");
	CHECK(texture_role_takes(texture_role(TextureRoleId::MenuImage), ".png") &&
			!texture_role_takes(texture_role(TextureRoleId::ModelDiffuse), ".png"), "only the menus take a PNG");
	CHECK(texture_role(TextureRoleId::HudMfd).size == TextureSizeRule::PowerOfTwo, "the MFD's sides powers of two");
	CHECK(texture_role(TextureRoleId::TerrainBlendMap).size == TextureSizeRule::QuadrantSplit, "the blend map split");
	const TextureRole &particle = texture_role(TextureRoleId::ParticleGraphic);
	CHECK(particle.size == TextureSizeRule::AtlasPage && particle.width == 1024 && particle.height == 256 &&
			particle.loader == TextureLoader::Particle, "a particle graphic's atlas pages");
	CHECK(texture_role_extensions(texture_role(TextureRoleId::ModelDiffuse)) ==
			std::vector<std::string>({".tga", ".mdt", ".pcx", ".dds"}), "the stage loader's formats in order");
	CHECK(texture_role(TextureRoleId::ModelChunk).formats == 0 && texture_role_extensions(texture_role(TextureRoleId::ModelChunk)).empty(),
			"a chunk container under any name");
	CHECK(&texture_role(TextureRoleId::kCount) == &texture_role(TextureRoleId::ModelDiffuse), "past the table: the first row");
}

void test_loaders() {
	// The loader a role goes through: the archive loader naming the file twice for the sky maps and
	// the tracer smoke, once for the others; the HUD loader in each mode; the cine's own loader.
	CHECK(texture_role(TextureRoleId::SkyCloud).loader == TextureLoader::ArchiveSelfAlpha &&
			texture_role(TextureRoleId::TracerSmoke).loader == TextureLoader::ArchiveSelfAlpha &&
			texture_role(TextureRoleId::ImpactScar).loader == TextureLoader::Archive &&
			texture_role(TextureRoleId::WaterWake).loader == TextureLoader::Archive, "the archive loader's two calls");
	CHECK(texture_role(TextureRoleId::HudColour).loader == TextureLoader::HudColor &&
			texture_role(TextureRoleId::HudAlphaOnly).loader == TextureLoader::HudAlpha, "the HUD loader's two modes");
	CHECK(texture_role(TextureRoleId::CinematicFade).loader == TextureLoader::CineFade, "the cine's own loader");
	CHECK(texture_role(TextureRoleId::HudFileArt).loader == TextureLoader::File &&
			texture_role(TextureRoleId::MenuImage).loader == TextureLoader::Menu &&
			texture_role(TextureRoleId::ModelPlain).loader == TextureLoader::Plain, "FILE, MENU and PLAIN");
	// The roles no named-file loader reads: a model row's normal map, producer and chunk, the 8-bit
	// maps read by their own name, the cube map.
	for (TextureRoleId role : {TextureRoleId::ModelNormalMap, TextureRoleId::ModelHeightNormal, TextureRoleId::ModelHorizon,
				 TextureRoleId::ModelOcclusion, TextureRoleId::ModelChunk, TextureRoleId::TerrainFoliageMap,
				 TextureRoleId::TerrainCharMap, TextureRoleId::PreviewCube})
		CHECK(!texture_loader_has_attempts(texture_role(role).loader), "no named-file loader");
	CHECK(texture_loader_has_attempts(texture_role(TextureRoleId::TerrainColourMap).loader), "the TGA reader names its file");
	// Read by its own name too: a terrain's detail maps alone.
	CHECK(texture_role_read_by_name(TextureRoleId::TerrainDetailCoefficient) &&
			texture_role_read_by_name(TextureRoleId::TerrainSplatDetail) &&
			texture_role_read_by_name(TextureRoleId::TerrainSecondDetail) &&
			!texture_role_read_by_name(TextureRoleId::ModelDiffuse), "the terrain details read by name");
}

void test_def_fields() {
	TextureRoleId role = TextureRoleId::kCount;
	CHECK(def_texture_field_role("hudicon", role) && role == TextureRoleId::HudAlphaOnly, "a weapon's HUD icon: alpha only");
	CHECK(def_texture_field_role("crosshair_secondary", role) && role == TextureRoleId::HudAlphaOnly, "a crosshair: alpha only");
	CHECK(def_texture_field_role("hud_image", role) && role == TextureRoleId::HudAlphaOnly, "an item's HUD image: alpha only");
	CHECK(def_texture_field_role("loadout_menu_icon", role) && role == TextureRoleId::MenuImage, "the loadout icon: a menu image");
	CHECK(def_texture_field_role("texture", role) && role == TextureRoleId::SightCard, "a sight's texture: a sight card");
	CHECK(!def_texture_field_role("shadow_texture", role) && !def_texture_field_role("graphic", role),
			"a field whose loader is not witnessed: none");
}

void test_alpha() {
	using M = TextureAlphaMeaning;
	// What a model row's alpha is: VS_PHONGT's diffuse the specular brightness; a blended material's the
	// transparency; an opaque one's unused; an alpha-tested one's the cut-out; a detail row's multiplied in;
	// a .tga normal row's the height.
	CHECK(texture_row_alpha_meaning("VS_PHONGT", 0, 0, 1, "body.tga") == M::Specular, "Phong: specular");
	CHECK(texture_row_alpha_meaning("FF_ST_AB", 0, 0, 1, "glass.tga") == M::Blend, "a blend");
	CHECK(texture_row_alpha_meaning("FF_ST_OP", 0, 0, 1, "wall.tga") == M::Unused, "opaque: unused");
	CHECK(texture_row_alpha_meaning("FF_ST_OP", 1, 0, 1, "fence.tga") == M::CutOut, "alpha-tested: the cut-out");
	CHECK(texture_row_alpha_meaning("FF_MT_OP", 0, 0, 2, "grain.tga") == M::Detail, "a detail row");
	CHECK(texture_row_alpha_meaning("VS_PHONGT", 0, 4, 3, "bump.tga") == M::Height, "a .tga normal row: the height");
	CHECK(texture_row_alpha_meaning("VS_PHONGT", 0, 4, 3, "bump.mdt") == M::Unused, "an .mdt normal row: unused");
	CHECK(!texture_alpha_is_transparency(M::Specular) && texture_alpha_is_transparency(M::CutOut) &&
			texture_alpha_is_transparency(M::Blend), "transparency: a cut-out or a blend");
	// The alpha test kept on the row the technique tests (a diffuse's), cleared on a detail row's, and
	// left alone where the material tests nothing.
	const uint8_t tested = uint8_t(opennova::threedi::THREEDI_MATERIAL_FLAG_ALPHA_TEST);
	CHECK(texture_row_material_flags("FF_ST_OP", tested, 0, 1) == tested, "the diffuse row keeps the test");
	CHECK(texture_row_material_flags("FF_MT_OP", tested, 0, 2) == 0, "a detail row's cleared");
	CHECK(texture_row_material_flags("FF_ST_OP", 0x04, 0, 2) == 0x04, "no test: the flags as they are");
}

} // namespace

int main() {
	test_catalog();
	test_loaders();
	test_def_fields();
	test_alpha();
	if (failures != 0) {
		std::fprintf(stderr, "texture_roles: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_roles: ok\n");
	return 0;
}
