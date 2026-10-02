#pragma once

// The textures the HUD and the first-person view effects open by a fixed name: the one table the
// shell's HUD device loads them by and the editor's import brings them by (ADR 0046 S14, review F4).

#include <cstdint>
#include <cstdio>
#include <string>

#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

// A texture the HUD loads by a fixed name into the device slot it draws it from.
struct HudFixedTexture {
	int32_t slot; // HudTexture
	const char *name;
};

inline constexpr HudFixedTexture kHudFixedTextures[] = {
	// The set the HUD loads as it is set up [orig: HUD_LoadAllTextures @0x59dda0].
	{ kHudTexVehicleFixed, "rockpip.tga" },     // @0x59ddc8
	{ kHudTexVehicleLag, "turrpip.tga" },       // @0x59ddd9
	{ kHudTexTargetFriendly, "comlck2x.tga" },  // @0x59ddfb
	{ kHudTexTarget, "comalck2.tga" },          // @0x59de0c
	{ kHudTexMapCompass, "compring.tga" },      // @0x59de1d
	{ kHudTexMapRadar, "dmgslice.tga" },        // @0x59de31
	{ kHudTexMapRadarNarrow, "dmgslc_n.tga" },  // @0x59de42
	{ kHudTexLogoHelo, "LogoHelo.tga" },        // @0x59de86
	{ kHudTexLogoHumm, "LogoHumm.tga" },        // @0x59de9a
	{ kHudTexLogoBoat, "LogoBoat.tga" },        // @0x59deab
	{ kHudTexMapIcons, "TSDicon.tga" },         // @0x59e060
	{ kHudTexMapWpIndicator, "WPIndctr.tga" },  // @0x59e079
	{ kHudTexLfpTeam1, "JO_LFP.tga" },          // @0x59e0c1
	{ kHudTexLfpTeam2, "R_LFP.tga" },           // @0x59e0da
	{ kHudTexLfpNeutral, "N_LFP.tga" },         // @0x59e0f3
	{ kHudTexLfpTileOther, "lfp_alf.tga" },     // @0x59e10e
	{ kHudTexLfpTileOwn, "lfp_dlf.tga" },       // @0x59e11f
	{ kHudTexDriverCrosshair, "dirguide.tga" }, // @0x59e40a
	// The Tab board's stdbox, registered as the mission starts [orig: Game_StartMission @0x524360,
	// boxtile.tga @0x525aa8, border.tga @0x525aad].
	{ kHudTexBoxBorder, "border.tga" },
	{ kHudTexBoxTile, "boxtile.tga" },
	// The connection indicators [orig: CNetworkIcons_LoadTextures @0x4c2cf0, neticon1.tga @0x4c2d1d,
	// neticon2.tga @0x4c2d94, neticon3.tga @0x4c2e0d].
	{ kHudTexNetLinkIcon, "neticon1.tga" },
	{ kHudTexNetIcon, "neticon2.tga" },
	{ kHudTexNetNovaWorldIcon, "neticon3.tga" },
	// The tip panel [orig: CTipSystem_Init @0x5b6970, border3 @0x5b698c, k_tip @0x5b69ae, g_tip
	// @0x5b69b6].
	{ kHudTexTipBox, "border3.tga" },
	{ kHudTexTipKeyboard, "k_tip.tga" },
	{ kHudTexTipGameplay, "g_tip.tga" },
};

// The fixed name of a slot the table holds; null for a slot it does not (one hudpos.def, an item
// definition or the frame names).
constexpr const char *hud_fixed_texture_name(int32_t slot) {
	for (const HudFixedTexture &texture : kHudFixedTextures)
		if (texture.slot == slot) return texture.name;
	return nullptr;
}

// The cargo icons the combat feed shows for a carried flag and for any other carried item naming no
// image of its own [orig: HUD_LoadAllTextures, H_flag.tga @0x59de53, H_docmnt.tga @0x59de64].
inline constexpr const char *kHudCargoFlagTexture = "H_flag.tga";
inline constexpr const char *kHudCargoDocumentTexture = "H_docmnt.tga";

// The user's crosshair style picks the crosshair's texture, "cross%02d.tga" of the style + 1 [orig:
// HUD_LoadAllTextures @0x59e3d6]; the options offer the styles 0 to kHudCrosshairStyleMax.
inline constexpr int kHudCrosshairStyleMin = 0;
inline constexpr int kHudCrosshairStyleMax = 24;
inline std::string hud_crosshair_texture_name(int style) {
	char name[24];
	std::snprintf(name, sizeof(name), "cross%02d.tga", style + 1);
	return name;
}

// The first-person view effects' textures [orig: ViewFx_InitShadersAndTextures @0x5cf8e0,
// Binoculr.tga @0x5cfdb8, BinoCH.tga @0x5cfdf3, NVG.tga @0x5cfe18, NVGScale.tga @0x5cfe4a; the
// binocular range digits, HUD_LoadAllTextures @0x59e109; the damage vignette, vignette.tga
// @0x5c36bc].
enum ViewEffectTexture : int32_t {
	kViewTexBinocularMask,
	kViewTexBinocularCrosshair,
	kViewTexBinocularDigits,
	kViewTexNvgMask,
	kViewTexNvgScale,
	kViewTexVignette,
	kViewTexCount,
};
inline constexpr const char *kViewEffectTextureNames[kViewTexCount] = {
	"Binoculr.tga", "BinoCH.tga", "BNumbers.tga", "NVG.tga", "NVGScale.tga", "vignette.tga",
};

} // namespace opennova::hud
