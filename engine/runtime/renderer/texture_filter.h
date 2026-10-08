#pragma once

// game.cfg's `texfilter_level` and the sampler it gives each texture stage
// (D-RMAT-22). Retail has two consumers of the word, each with its own timing:
//
// - The fixed-function device: each mission start copies the word with the
//   session's settings, and the main view's terrain pass turns the copy into
//   the device's filter mode, level + 1, every frame. Only a stage whose
//   texture was created with flag 0x8 takes that mode; every other stage is
//   bilinear with point mips whatever the level. The terrain's detail family
//   is the only texture family created with 0x8.
// - The HLSL effects (every model material): the word itself picks the define
//   each effect compiles its samplers with, at startup and again when the
//   front-end Options' Accept changes it, so it reaches the next draw at once.
//
// The Godot device leg turns the stage samplers into sampler state, the
// viewport's anisotropy and the shaders' filter globals; the level and the
// per-stage choice are here.

#include <cstdint>

#include <runtime/renderer/texture_dxt.h>

namespace opennova::renderer {

// --- the configuration word ---------------------------------------------------

// The rungs the config load clamps the word into, and the Options combobox's
// items (TT_BILINEAR, TT_TRILINEAR, TT_ANISOTROPIC, TT_ANISOTROPICHIGH).
// [orig: Settings_ClampGraphicsOptions @ 0x54D58E..0x54D5A3; options.mnu
//  TEXFILTER items 0..3]
inline constexpr int kTexFilterLevelMin = 0;
inline constexpr int kTexFilterLevelMax = 3;

// The config load's clamp of a persisted word.
inline constexpr int clamp_texfilter_level(int32_t level) {
	return level < kTexFilterLevelMin   ? kTexFilterLevelMin
	       : level > kTexFilterLevelMax ? kTexFilterLevelMax
	                                    : static_cast<int>(level);
}

// The word a fresh profile starts at: the first-launch video test (and the
// VIDEODEFAULT preset) writes quality_levels[9] = 1 on any pixel-shader card,
// raised by the preset offset on a shader-model-2 card (offset 0 for both, so
// 1, trilinear); a card with no pixel shaders gets 0.
// [orig: Renderer_ComputeQualityLevels @ 0x5860FC..0x58612C (index 9 at +0x24
//  of the 13-dword graphics block from 0x25507C4); Game_RunVideoTestDialog
//  @ 0x53ED0D and the VIDEODEFAULT preset @ 0x55A43A pass offset 0]
inline constexpr int kTexFilterLevelFreshProfile = 1;

// --- the stage sampler ----------------------------------------------------------

enum class SamplerMinFilter : uint8_t { Point, Linear, Anisotropic };
enum class SamplerMipFilter : uint8_t { Point, Linear };

// What the device samples one stage with (D3DSAMP_MINFILTER / MAGFILTER /
// MIPFILTER / MAXANISOTROPY). max_anisotropy is read only under an
// anisotropic minifier.
struct StageSampler {
	SamplerMinFilter min = SamplerMinFilter::Linear;
	bool mag_linear = true;
	SamplerMipFilter mip = SamplerMipFilter::Point;
	int max_anisotropy = 1;

	constexpr bool operator==(const StageSampler &other) const {
		return min == other.min && mag_linear == other.mag_linear && mip == other.mip &&
		       (min != SamplerMinFilter::Anisotropic || max_anisotropy == other.max_anisotropy);
	}
	constexpr bool operator!=(const StageSampler &other) const { return !(*this == other); }
};

// --- the fixed-function device ----------------------------------------------------

// The texture creation flags the stage decode reads (GTexture +0x18).
// [orig: CGfxShader_ApplyTextureStages @ 0x680856..0x680870 (bit 1 clear =
//  LINEAR min/mag into g_GfxStageLinearMinMag, bit 3 into
//  g_GfxStageDeviceFilter, bit 0 into g_GfxStageClamp); GTexture_BindToStage
//  @ 0x6845A9..0x6845C8 the same]
inline constexpr uint32_t kTextureFlagClamp = 0x1;
inline constexpr uint32_t kTextureFlagPointMinMag = 0x2;
inline constexpr uint32_t kTextureFlagDeviceFilter = 0x8;

// The device's filter mode from the session's copy of the word: level + 1.
// The switch leaves the mode alone for a word outside 0..3, which the load's
// clamp never lets through; the mode before the first terrain frame is the
// zero-initialised 0, which filters as mode 1.
// [orig: Terrain_RenderSkyboxPass @ 0x610BE8..0x610C1D (g_SessionTexFilterLevel
//  into g_GfxDeviceTexFilterMode, device +0x258); the session copy
//  apply_session_settings_to_globals @ 0x551565..0x551574]
inline constexpr int texfilter_device_mode(int session_level, int current_mode) {
	return session_level >= kTexFilterLevelMin && session_level <= kTexFilterLevelMax
	               ? session_level + 1
	               : current_mode;
}

// The device's MaxAnisotropy cap (D3DCAPS9.MaxAnisotropy, device +0x64) on the
// reference capture machine; mode 4 programs it.
// [orig: CGfxDevice_CreateDevice @ 0x67E6E5..0x67E6F9 (GetDeviceCaps)]
inline constexpr int kReferenceDeviceMaxAnisotropy = 16;

// One stage's sampler on the fixed-function path: a texture with flag 0x8
// takes the device mode (1 or below: LINEAR / LINEAR / POINT; 2: trilinear;
// 3: ANISOTROPIC at 2; 4: ANISOTROPIC at the device's cap), any other is
// bilinear with point mips, its min/mag POINT under flag 0x2.
// [orig: CGfxDevice_ApplyRenderStates @ 0x67E37D..0x67E3B0 (the mode's
//  MIN/MIP pair), @ 0x67E3DF..0x67E45B (a 0x8 stage: MIN, MAG LINEAR, MIP,
//  MAXANISOTROPY 2 or caps), @ 0x67E463..0x67E4A7 (any other: MIN/MAG LINEAR
//  or POINT, MIP POINT)]
inline constexpr StageSampler device_stage_sampler(int device_mode, uint32_t texture_flags,
		int device_max_anisotropy) {
	StageSampler sampler;
	if ((texture_flags & kTextureFlagDeviceFilter) == 0) {
		const bool linear = (texture_flags & kTextureFlagPointMinMag) == 0;
		sampler.min = linear ? SamplerMinFilter::Linear : SamplerMinFilter::Point;
		sampler.mag_linear = linear;
		sampler.mip = SamplerMipFilter::Point;
		return sampler;
	}
	sampler.mag_linear = true;
	if (device_mode == 2) {
		sampler.min = SamplerMinFilter::Linear;
		sampler.mip = SamplerMipFilter::Linear;
	} else if (device_mode == 3 || device_mode == 4) {
		sampler.min = SamplerMinFilter::Anisotropic;
		sampler.mip = SamplerMipFilter::Linear;
		sampler.max_anisotropy = device_mode == 3 ? 2 : device_max_anisotropy;
	} else {
		sampler.min = SamplerMinFilter::Linear;
		sampler.mip = SamplerMipFilter::Point;
	}
	return sampler;
}

// --- the HLSL effects -------------------------------------------------------------

// The effects' filter mode from the configuration word (not the session copy):
// 2 from level 2, 1 at level 1, else 0. It picks the define every effect
// compiles with: none, TRILINEAR, ANISO.
// [orig: Render_InitAllSubsystems @ 0x58640E..0x586427 and
//  Render_ReloadEffectsForTexFilter @ 0x586485..0x58649E into
//  HLSLEffect_SetTextureFilterMode @ 0x5ADD70; HLSLEffect_LoadFromFile
//  @ 0x5AE6B9..0x5AE6E5 (the define)]
inline constexpr int effect_texture_filter_mode(int cfg_level) {
	return cfg_level >= 2 ? 2 : cfg_level >= 1 ? 1 : 0;
}

// The three samplers every shipped effect binds its stages through.
enum class EffectSampler : uint8_t {
	LinearWrap2D,  // TexDiffuse1/2, TexNormal1
	LinearWrap3D,  // the cube maps
	LinearClamp2D, // the phong map, clip, depth gradients, the projected page
};

// One effect sampler under a filter mode: _BaseInc.fx's ANISO block
// (ANISOTROPIC / LINEAR / LINEAR, MaxAnisotropy 2), TRILINEAR block (LINEAR /
// LINEAR / LINEAR) and the bilinear block (LINEAR / LINEAR / POINT);
// sampLinearWrap3D is LINEAR / LINEAR / POINT in all three.
// [orig: _BaseInc.fx sampLinearWrap2D / sampLinearWrap3D / sampLinearClamp2D
//  under #ifdef ANISO / #elif defined(TRILINEAR) / #else]
inline constexpr StageSampler effect_stage_sampler(int effect_mode, EffectSampler which) {
	StageSampler sampler;
	sampler.mag_linear = true;
	if (which == EffectSampler::LinearWrap3D || effect_mode <= 0) {
		sampler.min = SamplerMinFilter::Linear;
		sampler.mip = SamplerMipFilter::Point;
	} else if (effect_mode == 1) {
		sampler.min = SamplerMinFilter::Linear;
		sampler.mip = SamplerMipFilter::Linear;
	} else {
		sampler.min = SamplerMinFilter::Anisotropic;
		sampler.mip = SamplerMipFilter::Linear;
		sampler.max_anisotropy = 2;
	}
	return sampler;
}

// --- the stages OpenNova draws ------------------------------------------------------

// Each texture stage family the game draws, by the creation flags (the
// fixed-function draws) or the effect sampler (the model draws) retail gives
// it. Census 2026-10-07: PolyTrn_InitTextures' `or ebp, 8` @ 0x60ABA0 is the
// only creation site that sets flag 0x8.
enum class TextureStage : uint8_t {
	// The splat layers c1..c3, the detail map and the second detail with their
	// far pairs, and the detail coefficient map: dword_31A1820 | 8 (the
	// terrain-detail halving word), DXT layers 0x400100 / 0x400200 on top.
	// [orig: PolyTrn_InitTextures @ 0x60AB95..0x60ABE5 (c1..c3),
	//  @ 0x60AF80..0x60AF88 (the second detail), @ 0x60B14D..0x60B155 (the
	//  coefficient map, flags | 0x1000000 in Texture_GenerateNormalMap
	//  @ 0x58C427)]
	TerrainDetail,
	// The DBlendmap quadrants: 0x100001 with the compression word.
	// [orig: PolyTrn_InitTextures @ 0x60B515..0x60B51C, @ 0x60B969..0x60B970]
	TerrainBlendMap,
	// A foliage definition's ":fd" mask, loaded with 0x100000 before its mip
	// chain is rebuilt in place (the rebuild keeps the flags).
	// [orig: Foliage_LoadDefAssets @ 0x601634..0x601650, @ 0x6019CD..0x6019E4;
	//  GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270 sets no flags]
	FoliageMask,
	// The sky maps (both cloud layers): 0x100000.
	// [orig: Terrain_InitRenderingResources @ 0x578A82..0x578A9A]
	SkyMap,
	// The impact scar strip (scorch1..4, bigscar, the glass holes, bhole1):
	// each entry's load word | 1, the word 0, 0x80000 (bigscar) or 0x40000
	// (the glass holes) (world::scar_texture_strip_creation_flags), drawn by
	// the scar drawer's GfxShader pass.
	// [orig: Scar_LoadTextures @ 0x5CC2F0..0x5CC302; Scar_DrawBatches
	//  @ 0x5CCDDE]
	ImpactScar,
	// The tracer pool's smoke texture (smoktest.pcx): 0x100000, on both stages
	// of the smoke and NVG ribbon passes, which draw through a GfxShader pass,
	// never an HLSL effect, the normal pass and the distortion pass alike.
	// [orig: CEffectEmitterPool_CreateShaders @ 0x5DC913; CEffectChannel_RenderRibbon
	//  @ 0x5DC86C..0x5DC877 (pass flags 0x10520000), the distortion branch
	//  @ 0x5DC0B6]
	TracerSmoke,
	// The water wake's ring and gradient (wake5.tga, wakegrad.tga): 0x100000.
	// [orig: WaterRing_LoadResources @ 0x5DDCA1, @ 0x5DDCBC; WaterRing_Draw
	//  @ 0x5DE245]
	WaterWake,
	// The rain and snow drops (eraindrp.tga, jsnwflk.tga): 0x100000 through the
	// stage loader, drawn under the mode word 0x651.
	// [orig: WeatherParticle_LoadTextures @ 0x5DE84C, @ 0x5DE88E;
	//  Render_WeatherTrailParticles @ 0x5DEF85]
	Precipitation,
	// The procedural light corona "texlightcrn", 128 x 128: 1.
	// [orig: Lighting_InitTextures @ 0x5A97D3..0x5A97F2;
	//  EffectWorld_RenderLightCoronas @ 0x5AAFCC]
	LightCorona,
	// The map icon strip (TSDicon.tga): the file loader's 0x100000, the image
	// tile it is built as created with it | 1, drawn by the tiled-image
	// drawer's GfxShader pass. The tile is the strip at its sides rounded up to
	// powers of two (16 x 512 for the stock 16 x 480), so its chain follows the
	// strip's width.
	// [orig: HUD_LoadAllTextures @ 0x59E03D..0x59E04C; Texture_LoadFromFile_0
	//  @ 0x58FE55, @ 0x58FE80 (the flags into CEffect_BeginPassTraced);
	//  GImage_CreateTiledTextures_0 @ 0x67A8B9 (the tile sides, sub_679DF0),
	//  @ 0x67AA30..0x67AA43 (| 1); render_tiled_image_strip @ 0x67B6FC]
	MapIconStrip,
	// A model stage through its effect: TexDiffuse1/2 and TexNormal1.
	ObjectStage,
	// A model's cube map through its effect.
	ObjectCube,
};

// The texture flags a fixed-function family is created with, as far as the
// sampler reads them (the halving, DXT and 0x100000 words left out; a scar
// strip's chain word is its table entry's).
inline constexpr uint32_t texture_stage_flags(TextureStage stage) {
	switch (stage) {
	case TextureStage::TerrainDetail: return kTextureFlagDeviceFilter;
	case TextureStage::TerrainBlendMap:
	case TextureStage::ImpactScar:
	case TextureStage::LightCorona:
	case TextureStage::MapIconStrip: return kTextureFlagClamp;
	case TextureStage::FoliageMask:
	case TextureStage::SkyMap:
	case TextureStage::TracerSmoke:
	case TextureStage::WaterWake:
	case TextureStage::Precipitation:
	case TextureStage::ObjectStage:
	case TextureStage::ObjectCube: return 0;
	}
	return 0;
}

// --- the stage's mip chain ------------------------------------------------------

// The chain flags a texture built from pixels is created with: one level, or
// at most three. MIPFILTER POINT never reads past a texture's last level, and
// the chain GTexture_CreateFromPixelData_0 asks for (texture_level_count:
// one level per halving while the smaller side exceeds 2, so 4 x 4 is the
// last, unless these cap it) is shorter than the full chain OpenNova's
// textures carry, so a stage stops at that chain's last level. A DDS keeps
// D3DX's chain, the one its OpenNova texture carries.
// [orig: GTexture_CreateFromPixelData_0 @ 0x6877BA..0x687801]
inline constexpr uint32_t kTextureFlagOneLevel = 0x40000;
inline constexpr uint32_t kTextureFlagThreeLevels = 0x80000;

// The last level a stage samples on a texture the game builds from pixels,
// `width` x `height` (after its halvings) under its creation flags.
inline uint32_t pixel_texture_last_level(uint32_t width, uint32_t height, uint32_t creation_flags) {
	const uint32_t levels = texture_level_count(width, height, creation_flags);
	return levels == 0 ? 0 : levels - 1;
}

// The device and effect state a frame draws under: the session's device mode,
// the configuration's effect mode, the device's anisotropy cap.
struct TexFilterState {
	int device_mode = kTexFilterLevelFreshProfile + 1;
	int effect_mode = effect_texture_filter_mode(kTexFilterLevelFreshProfile);
	int device_max_anisotropy = kReferenceDeviceMaxAnisotropy;
};

// The state after a mission start (the session copy of `session_level`) under
// the configuration word `cfg_level`.
inline constexpr TexFilterState texfilter_state(int session_level, int cfg_level,
		int device_max_anisotropy = kReferenceDeviceMaxAnisotropy) {
	TexFilterState state;
	state.device_mode = texfilter_device_mode(clamp_texfilter_level(session_level), 0);
	state.effect_mode = effect_texture_filter_mode(clamp_texfilter_level(cfg_level));
	state.device_max_anisotropy = device_max_anisotropy;
	return state;
}

// The sampler one stage family draws with under `state`.
inline constexpr StageSampler stage_sampler(TextureStage stage, const TexFilterState &state) {
	switch (stage) {
	case TextureStage::ObjectStage:
		return effect_stage_sampler(state.effect_mode, EffectSampler::LinearWrap2D);
	case TextureStage::ObjectCube:
		return effect_stage_sampler(state.effect_mode, EffectSampler::LinearWrap3D);
	default:
		return device_stage_sampler(state.device_mode, texture_stage_flags(stage),
				state.device_max_anisotropy);
	}
}

// The shaders' code for a sampler with a mip chain: 0 = bilinear on the
// nearest level (MIP POINT), 1 = trilinear, 2 = anisotropic. A POINT
// minifier never reaches a mipped OpenNova stage.
inline constexpr int shader_filter_code(const StageSampler &sampler) {
	if (sampler.mip == SamplerMipFilter::Point) return 0;
	return sampler.min == SamplerMinFilter::Anisotropic ? 2 : 1;
}

// The anisotropy a hardware sampler is programmed with: 1 (off) unless the
// minifier is anisotropic.
inline constexpr int hardware_max_anisotropy(const StageSampler &sampler) {
	return sampler.min == SamplerMinFilter::Anisotropic ? sampler.max_anisotropy : 1;
}

} // namespace opennova::renderer
