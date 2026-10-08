#pragma once

// game.cfg's `texcompression_level` and the texture families it compresses
// (D-RMAT-24). The front-end Options' TEXCOMPRESSION row edits the word
// (0 maximum, 1 normal, 2 minimal compression); each mission start copies it
// with the session's settings, and the families below read that copy when the
// mission builds them, so a change reaches the draw at the next mission.
// [orig: Config_ParseSettingsLine @ 0x551268 (game.cfg +0x13C, never clamped);
//  apply_session_settings_to_globals @ 0x551565..0x551574 (the copy's +0x30,
//  g_SessionTexCompressionLevel @ 0x24D2070); sub_55A710 @ 0x55A957..0x55A972
//  (the Accept writes g_CfgTexCompressionLevel @ 0x25507F4 only)]
//
// A family's flag word is a texture creation word (renderer/texture_dxt.h
// select_texture_dxt_format): 0x200 asks for DXT5, 0x100 for DXT1, and 0x400000
// falls back to DXT1 on a card outside the keep-DXT5 list (the reference card).

#include <cstdint>

namespace opennova::renderer {

// The word a fresh profile starts at: the first-launch video test (and the
// VIDEODEFAULT preset) writes quality_levels[12] = 1 on a shader-model-2 card,
// 0 on any other, then clamps it into 0..2; the preset offset is 0 for both.
// [orig: Renderer_ComputeQualityLevels @ 0x586149..0x586156 (caps +0x20, the
//  shader-model-2 mark from sub_587790 @ 0x58787A), @ 0x586333..0x586340 (the
//  clamp); Game_RunVideoTestDialog @ 0x53ED0D and the VIDEODEFAULT preset
//  sub_55A430 @ 0x55A43A pass offset 0]
inline constexpr int kTexCompressionLevelFreshProfile = 1;

// The Options row's rungs: 0..2, every one open, the device-maximum block's
// word being 2 on every card.
// [orig: UI_PopulateRenderAndAudioSettings @ 0x55D0F3..0x55D141 (rows above
//  g_CfgTexCompressionLevelMax @ 0x2550828 locked); RenderSettings_ComputeFromGPUCaps
//  @ 0x587A43 (always 2)]
inline constexpr int kTexCompressionLevelMin = 0;
inline constexpr int kTexCompressionLevelMax = 2;

// The particle atlas pages: DXT5 at 1 or less (renderer/particle_atlas.h).
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82BF]
inline constexpr uint32_t particle_page_compression_flags(int32_t session_level) {
	return session_level <= 1 ? 0x200u : 0u;
}

// The terrain's splat detail layers (c1..c3, and the base detail map on the
// tiers below the pixel-shader path): DXT1 below 1, else the DXT5 request with
// the DXT1 fallback, so DXT1 either way on the reference card.
// [orig: PolyTrn_InitTextures @ 0x60AB9B..0x60ABBC]
inline constexpr uint32_t terrain_detail_layer_compression_flags(int32_t session_level) {
	return session_level < 1 ? 0x400100u : 0x400200u;
}

// The colormap quadrants (Colormap0..3, which the terrain pages and the map's
// terrain pass sample): the DXT5 request with the DXT1 fallback below 2,
// uncompressed at 2 and above.
// [orig: PolyTrn_InitTextures @ 0x60ABAD..0x60ABC6 (var_2C0), @ 0x60B515..0x60B51C
//  (| 0x100001 on the pixel-shader path), @ 0x60B969..0x60B970 (the others)]
inline constexpr uint32_t terrain_colormap_compression_flags(int32_t session_level) {
	return session_level < 2 ? 0x400200u : 0u;
}

// A weapon.def SIGHTS row's picture, through the stage loader: 0x201 (DXT5,
// clamped) at 1 or less, else 1 (clamped). weapon.def loads at each mission
// start, after the copy.
// [orig: WeaponDef_CreateBlendNamedMaterial @ 0x540188..0x5401A1;
//  WeaponDefs_LoadFile called from Game_StartMission @ 0x5254BD]
inline constexpr uint32_t sight_card_texture_flags(int32_t session_level) {
	return session_level > 1 ? 0x1u : 0x201u;
}

} // namespace opennova::renderer
