// The textures the game opens by its own names (renderer/fixed_texture_names.h): each once, in
// its role, through the loader that opens it (its role's, or the night vision's scale through
// FILE and the damage vignette through ARCHIVE); the HUD's set split by its loaders; and the
// MFD's power-of-two rule (hud/hud_texture_names.h).

#include <runtime/hud/hud_texture_names.h>
#include <runtime/renderer/fixed_texture_names.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <string>

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

const FixedTextureName *named(const std::string &name) {
	for (const FixedTextureName &fixed : fixed_texture_names())
		if (fixed.name == name) return &fixed;
	return nullptr;
}

void test_names() {
	std::set<std::string> seen;
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		std::string lower = fixed.name;
		for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		CHECK(seen.insert(lower).second, "each name once, whatever its case");
		CHECK(fixed.role != TextureRoleId::kCount && fixed.use != FixedTextureUse::kCount, "a role and a use each");
		const bool overridden = fixed.use == FixedTextureUse::NvgScale || fixed.use == FixedTextureUse::Vignette;
		CHECK(overridden || fixed.loader == texture_role(fixed.role).loader, "its role's loader");
	}
	// The HUD's set of 26, the board's monogram, 2 carried-item icons, 25 crosshairs, the scope's crosshair, 6
	// view-effect names, 2 weather, the smoke, 2 water rings, 5 scar strips and 4 scorch marks named, the
	// cursor, the cube, the boot splash, the default loading screen, the MFD.
	CHECK(fixed_texture_names().size() == 80, "every fixed name");
	const FixedTextureName *scope = named("scopexh.tga");
	CHECK(scope && scope->role == TextureRoleId::HudAlphaOnly && scope->loader == TextureLoader::HudAlpha,
			"the scope's crosshair in the HUD's alpha mode");
	const FixedTextureName *mfd = named("MFD1.PCX");
	CHECK(mfd && mfd->role == TextureRoleId::HudMfd && mfd->loader == TextureLoader::Pcx, "the MFD through the PCX reader");
	const FixedTextureName *scale = named("NVGScale.tga");
	CHECK(scale && scale->role == TextureRoleId::ViewEffect && scale->loader == TextureLoader::File, "the NVG scale through FILE");
	const FixedTextureName *vignette = named("vignette.tga");
	CHECK(vignette && vignette->loader == TextureLoader::Archive, "the vignette through ARCHIVE");
	const FixedTextureName *map = named("TSDicon.tga");
	CHECK(map && map->use == FixedTextureUse::HudMap && map->loader == TextureLoader::File, "the HUD map's art through FILE");
	const FixedTextureName *box = named("border.tga");
	CHECK(box && box->use == FixedTextureUse::BoardBox && box->loader == TextureLoader::Tga, "the board's box: the TGA reader");
	const FixedTextureName *tip = named("border3.tga");
	CHECK(tip && tip->use == FixedTextureUse::TipBox && tip->role == TextureRoleId::BoardBox, "the tip panel's box");
	const FixedTextureName *pip = named("rockpip.tga");
	CHECK(pip && pip->use == FixedTextureUse::HudArt && pip->loader == TextureLoader::HudColor, "the HUD's colour art");
	const FixedTextureName *smoke = named("smoktest.pcx");
	CHECK(smoke && smoke->loader == TextureLoader::ArchiveSelfAlpha, "the smoke's PCX its own luminance's alpha");
	const FixedTextureName *cube = named("HwmCube.dds");
	CHECK(cube && cube->loader == TextureLoader::Cube, "the preview's cube");
	const FixedTextureName *fallback = named("loadscrn.pcx");
	CHECK(fallback && fallback->role == TextureRoleId::LoadingScreen && fallback->use == FixedTextureUse::LoadingFallback,
			"the default loading screen");
	CHECK(named("monogram.tga") && named("loading.pcx") && named("newarow1.tga") && named("cross01.tga") &&
			named("cross25.tga") && !named("cross26.tga"), "the board's monogram, the splashes, the crosshair styles");
}

void test_mfd() {
	CHECK(opennova::hud::hud_mfd_texture_takes(256, 128) && opennova::hud::hud_mfd_texture_takes(1, 1),
			"powers of two make a material");
	CHECK(!opennova::hud::hud_mfd_texture_takes(250, 128) && !opennova::hud::hud_mfd_texture_takes(256, 0),
			"any other side makes none");
}

} // namespace

int main() {
	test_names();
	test_mfd();
	if (failures != 0) {
		std::fprintf(stderr, "fixed_texture_names: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("fixed_texture_names: ok\n");
	return 0;
}
