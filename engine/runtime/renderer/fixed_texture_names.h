#pragma once

// The textures the game opens by a name of its own, not one a file names: the HUD's art
// (runtime/hud/hud_texture_names.h), the screens' (runtime/hud/loading_screen.h), the weather's,
// the smoke trails', the water rings', the scars' and the scorch marks', the player preview's
// reflection and the boot splash. Each with the role it is used in (texture_roles.h), the loader
// that opens it and the use that opens it; fixed_texture_names.cpp holds each use's witness.

#include <runtime/renderer/texture_load_rules.h>
#include <runtime/renderer/texture_roles.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::renderer {

// The player preview's reflection cube [orig: PlayerInfo_InitPreviewModel @ 0x56010C, through
// sub_58A690].
inline constexpr const char *kPreviewCubeTexture = "HwmCube.dds";
// The boot splash, stretched to the desktop [orig: Game_ShowLoadingScreen @ 0x4A5420].
inline constexpr const char *kBootSplashTexture = "loading.pcx";

// What the game opens a fixed name for, a use each.
enum class FixedTextureUse : uint8_t {
	BoardBox, // the Tab board's stdbox: border.tga, boxtile.tga
	BoardMonogram, // the stdbox's monogram
	NetIcon, // the connection indicators
	TipBox, // the tip panel's box
	TipArt, // the tip panel's two pictures
	HudMap, // the HUD map's art, through FILE
	HudArt, // the rest of the HUD's own art
	HudCargoFlag, // the combat feed's carried flag
	HudCargoItem, // the combat feed's carried item
	Crosshair, // a crosshair style's
	ScopeCrosshair, // the scope's crosshair
	BinocularMask,
	BinocularCrosshair,
	BinocularDigits, // the binoculars' range digits
	NvgMask,
	NvgScale,
	Vignette, // the damage vignette
	Rain,
	Snow,
	SmokeTrail,
	WaterRing,
	ImpactScar,
	Scorch,
	SplashCursor, // the start-mission splash's cursor
	PreviewCube,
	BootSplash,
	LoadingFallback, // the loading screen of a mission with none of its own
	Mfd, // the vehicles' MFD
	kCount,
};

struct FixedTextureName {
	std::string name;
	FixedTextureUse use = FixedTextureUse::kCount;
	TextureRoleId role = TextureRoleId::kCount;
	// The loader that opens it: its role's, or another where the use goes through one (the night
	// vision's scale through FILE, the damage vignette through ARCHIVE).
	TextureLoader loader = TextureLoader::Stage;
};

// Every name the game opens by itself, each once (names that differ only in case are one):
// the HUD's set, the board's monogram, the carried-item icons, the crosshair styles, the scope's
// crosshair, the view effects, the weather, the smoke trails, the water rings, every scar strip
// and scorch mark, the start-mission cursor, the preview cube, the boot splash, the default
// loading screen and the MFD.
const std::vector<FixedTextureName> &fixed_texture_names();

} // namespace opennova::renderer
