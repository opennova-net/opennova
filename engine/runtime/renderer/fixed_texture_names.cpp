#include <runtime/renderer/fixed_texture_names.h>

#include <set>

#include <base/io/strutil.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/hud/loading_screen.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/renderer/water_wake_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/world/impact_scar.h>

namespace opennova::renderer {

namespace {

using U = FixedTextureUse;
using R = TextureRoleId;

// Which use of the HUD's own set a slot of it is: the board's stdbox, the connection indicators,
// the tip panel's box and pictures, the map's art through FILE, else the HUD's colour art.
U hud_set_use(int32_t slot) {
	switch (slot) {
	case hud::kHudTexBoxBorder:
	case hud::kHudTexBoxTile: return U::BoardBox;
	case hud::kHudTexNetLinkIcon:
	case hud::kHudTexNetIcon:
	case hud::kHudTexNetNovaWorldIcon: return U::NetIcon;
	case hud::kHudTexTipBox: return U::TipBox;
	case hud::kHudTexTipKeyboard:
	case hud::kHudTexTipGameplay: return U::TipArt;
	case hud::kHudTexMapIcons:
	case hud::kHudTexMapWpIndicator:
	case hud::kHudTexLfpTeam1:
	case hud::kHudTexLfpTeam2:
	case hud::kHudTexLfpNeutral: return U::HudMap;
	default: return U::HudArt;
	}
}

R hud_set_role(U use) {
	switch (use) {
	case U::BoardBox:
	case U::TipBox: return R::BoardBox;
	case U::NetIcon: return R::NetIcon;
	case U::TipArt: return R::TipArt;
	case U::HudMap: return R::HudFileArt;
	default: return R::HudColour;
	}
}

std::vector<FixedTextureName> collect() {
	std::vector<FixedTextureName> out;
	std::set<std::string> seen;
	const auto add = [&](const std::string &name, U use, R role, TextureLoader loader) {
		if (name.empty() || !seen.insert(strutil::to_lower(name)).second) return;
		FixedTextureName row;
		row.name = name;
		row.use = use;
		row.role = role;
		row.loader = loader;
		out.push_back(std::move(row));
	};
	const auto add_by_role = [&](const std::string &name, U use, R role) { add(name, use, role, texture_role(role).loader); };
	// The HUD's own set [orig: HUD_LoadAllTextures @ 0x59DDA0]: the board's stdbox through the box loader
	// [orig: BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0], the connection indicators [orig:
	// CNetworkIcons_LoadTextures @ 0x4C2CF0], the tip panel's box through the box loader (the TGA reader,
	// no .dds tried) and its two pictures by STAGE [orig: CTipSystem_Init @ 0x5B6970], the map's art
	// through FILE [orig: HUD_LoadAllTextures @ 0x59E060..0x59E0F3], the rest the HUD's colour art.
	for (const hud::HudFixedTexture &texture : hud::kHudFixedTextures) {
		const U use = hud_set_use(texture.slot);
		add_by_role(texture.name, use, hud_set_role(use));
	}
	// The board's stdbox's third texture [orig: Game_StartMission @ 0x525AA3, through sub_56AB00 and the
	// box loader].
	add_by_role(hud::kHudBoxMonogramTexture, U::BoardMonogram, R::BoardBox);
	// [orig: HUD_LoadAllTextures @ 0x59DE53, @ 0x59DE64]
	add_by_role(hud::kHudCargoFlagTexture, U::HudCargoFlag, R::HudColour);
	add_by_role(hud::kHudCargoDocumentTexture, U::HudCargoItem, R::HudColour);
	// [orig: HUD_LoadAllTextures @ 0x59E3D6]
	for (int style = hud::kHudCrosshairStyleMin; style <= hud::kHudCrosshairStyleMax; ++style)
		add_by_role(hud::hud_crosshair_texture_name(style), U::Crosshair, R::HudColour);
	// In the HUD loader's alpha mode [orig: HUD_LoadAllTextures @ 0x59DDA0].
	add_by_role(hud::kHudScopeCrosshairTexture, U::ScopeCrosshair, R::HudAlphaOnly);
	// [orig: ViewFx_InitShadersAndTextures @ 0x5CFDB8, @ 0x5CFDF3; HUD_LoadAllTextures @ 0x59E109 (the
	// digits through FILE); ViewFx_InitShadersAndTextures @ 0x5CFE18, @ 0x5CFE4A (the scale through FILE);
	// sub_5C36B0 @ 0x5C36BC (the vignette through ARCHIVE)]
	add_by_role(hud::kViewEffectTextureNames[hud::kViewTexBinocularMask], U::BinocularMask, R::ViewEffect);
	add_by_role(hud::kViewEffectTextureNames[hud::kViewTexBinocularCrosshair], U::BinocularCrosshair, R::ViewEffect);
	add_by_role(hud::kViewEffectTextureNames[hud::kViewTexBinocularDigits], U::BinocularDigits, R::HudFileArt);
	add_by_role(hud::kViewEffectTextureNames[hud::kViewTexNvgMask], U::NvgMask, R::ViewEffect);
	add(hud::kViewEffectTextureNames[hud::kViewTexNvgScale], U::NvgScale, R::ViewEffect, TextureLoader::File);
	add(hud::kViewEffectTextureNames[hud::kViewTexVignette], U::Vignette, R::ViewEffect, TextureLoader::Archive);
	// [orig: WeatherParticle_LoadTextures @ 0x5DE840, @ 0x5DE88E]
	add_by_role(kRainTexture, U::Rain, R::WeatherDrop);
	add_by_role(kSnowTexture, U::Snow, R::WeatherDrop);
	// [orig: CEffectEmitterPool_CreateShaders @ 0x5DC8F0]
	add_by_role(kEmitterPoolTexture, U::SmokeTrail, R::TracerSmoke);
	// [orig: WaterRing_LoadResources @ 0x5DDC90]
	add_by_role(kWakeTexture, U::WaterRing, R::WaterWake);
	add_by_role(kWakeGradientTexture, U::WaterRing, R::WaterWake);
	// [orig: Scar_LoadTextures @ 0x5CC2E0]
	for (int strip = 0; strip < world::kScarTextureStripCount; ++strip)
		add_by_role(world::scar_texture_strip_name(strip), U::ImpactScar, R::ImpactScar);
	// [orig: Terrain_LoadScorchTextures @ 0x604CE0]
	for (int index = 0; index <= 255; ++index)
		add_by_role(std::string(terrain::terrain_scorch_texture_name(static_cast<uint8_t>(index))), U::Scorch,
				R::TerrainScorch);
	// [orig: Game_ShowStartMissionSplash @ 0x520820]
	add_by_role(hud::kSplashArrowImage, U::SplashCursor, R::SplashCursor);
	add_by_role(kPreviewCubeTexture, U::PreviewCube, R::PreviewCube);
	add_by_role(kBootSplashTexture, U::BootSplash, R::BootSplash);
	// [orig: Render_LoadingScreen @ 0x521D10]
	add_by_role(hud::kLoadingFallbackImage, U::LoadingFallback, R::LoadingScreen);
	// [orig: sub_59B120 @ 0x59B120]
	add_by_role(hud::kHudMfdTexture, U::Mfd, R::HudMfd);
	return out;
}

} // namespace

const std::vector<FixedTextureName> &fixed_texture_names() {
	static const std::vector<FixedTextureName> names = collect();
	return names;
}

} // namespace opennova::renderer
