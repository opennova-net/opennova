#pragma once

// The material words the HUD's own textures are made with, by their maker. A
// draw combines texel and vertex colour through its texture's material word
// (renderer::material_color_stage), whatever loader read the pixels; the HUD
// loader's textures take renderer::hud_loader_material_word instead. Witness
// record: docs/interface/hud-re.md ("The HUD texture loader").

#include <cstdint>

namespace opennova::hud {

// The map icon strip, TSDicon.tga [orig: HUD_LoadAllTextures @0x59dda0 —
// Texture_LoadFromFile_0("TSDicon.tga", 0x651, 0x100000), the word @0x59e042].
inline constexpr uint32_t kTsdIconMaterialWord = 0x651u;

// The waypoint altitude indicator, WPIndctr.tga [orig: HUD_LoadAllTextures —
// Texture_LoadFromFile_0("WPIndctr.tga", 0x300631, 0x140000), the word @0x59e056].
inline constexpr uint32_t kWpIndicatorMaterialWord = 0x300631u;

// The capture-point team icons JO_LFP.tga / R_LFP.tga / N_LFP.tga [orig:
// HUD_LoadAllTextures — Texture_LoadFromFile_0(name, 0x300631, 0x100000), the
// words @0x59e09e / @0x59e0b7 / @0x59e0d0]. Every tile of the strip draws with
// sub_676D50(tile, 0x300631) [orig: GImage_CreateTiledTextures_0 @0x67a830,
// applied per tile by Render_DrawTiledTextureStrip @0x67aed0].
inline constexpr uint32_t kLfpIconMaterialWord = 0x300631u;

// The box styles (border.tga with boxtile.tga, border3.tga alone): the extracted
// fill cell's, the atlas-with-secondary's and the atlas-alone's materials [orig:
// BoxTexture_LoadAndSetupUVRegions @0x56acd0 — sub_676D50(cell, 0x651) @0x56ae78,
// CGfxTexture_Create(atlas, secondary, 0x651, 2) @0x56af2d, sub_676D50(atlas,
// 0x651) @0x56af43].
inline constexpr uint32_t kBoxMaterialWord = 0x651u;

// The tip panel's icons, k_tip.tga / g_tip.tga [orig: CTipSystem_Init @0x5b6970 —
// GfxShader_Create1TexModeId(icon, 0x651) @0x5b69bc / @0x5b69ca].
inline constexpr uint32_t kTipIconMaterialWord = 0x651u;

// The network icons neticon1/2/3.tga [orig: CNetworkIcons_LoadTextures @0x4c2cf0 —
// CEffect_BeginPassTraced(..., 0x300451, 0x140000) @0x4c2d53 / @0x4c2dcb /
// @0x4c2e41]: colour family 0x400, SELECTARG1(TEXTURE).
inline constexpr uint32_t kNetIconMaterialWord = 0x300451u;

} // namespace opennova::hud
