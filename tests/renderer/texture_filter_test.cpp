// game.cfg's texfilter_level and the per-stage samplers it selects
// (runtime/renderer/texture_filter.h, D-RMAT-22).
#include <runtime/renderer/texture_filter.h>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using namespace opennova::renderer;

constexpr StageSampler sampler(SamplerMinFilter min, SamplerMipFilter mip, int max_anisotropy = 1,
		bool mag_linear = true) {
	StageSampler out;
	out.min = min;
	out.mip = mip;
	out.max_anisotropy = max_anisotropy;
	out.mag_linear = mag_linear;
	return out;
}

constexpr StageSampler kBilinearPointMip = sampler(SamplerMinFilter::Linear, SamplerMipFilter::Point);
constexpr StageSampler kTrilinear = sampler(SamplerMinFilter::Linear, SamplerMipFilter::Linear);

void check_word() {
	// The config load's clamp and the Options combobox's rungs.
	CHECK(clamp_texfilter_level(-4) == 0);
	CHECK(clamp_texfilter_level(0) == 0);
	CHECK(clamp_texfilter_level(3) == 3);
	CHECK(clamp_texfilter_level(9) == 3);
	// A strict Play's first run writes 1 (the video test, offset 0).
	CHECK(kTexFilterLevelFreshProfile == 1);
}

void check_device_mode() {
	// Terrain_RenderSkyboxPass: level + 1; a word outside 0..3 keeps the mode.
	CHECK(texfilter_device_mode(0, 0) == 1);
	CHECK(texfilter_device_mode(1, 0) == 2);
	CHECK(texfilter_device_mode(2, 0) == 3);
	CHECK(texfilter_device_mode(3, 0) == 4);
	CHECK(texfilter_device_mode(4, 2) == 2);
	CHECK(texfilter_device_mode(-1, 3) == 3);
}

void check_device_stage() {
	// A flag-0x8 stage follows the mode (CGfxDevice_ApplyRenderStates).
	CHECK(device_stage_sampler(0, kTextureFlagDeviceFilter, 16) == kBilinearPointMip);
	CHECK(device_stage_sampler(1, kTextureFlagDeviceFilter, 16) == kBilinearPointMip);
	CHECK(device_stage_sampler(2, kTextureFlagDeviceFilter, 16) == kTrilinear);
	CHECK(device_stage_sampler(3, kTextureFlagDeviceFilter, 16) ==
			sampler(SamplerMinFilter::Anisotropic, SamplerMipFilter::Linear, 2));
	CHECK(device_stage_sampler(4, kTextureFlagDeviceFilter, 16) ==
			sampler(SamplerMinFilter::Anisotropic, SamplerMipFilter::Linear, 16));
	CHECK(device_stage_sampler(4, kTextureFlagDeviceFilter, 8) ==
			sampler(SamplerMinFilter::Anisotropic, SamplerMipFilter::Linear, 8));
	// Any other stage is bilinear with point mips at every mode; flag 0x2
	// makes its min/mag POINT.
	for (int mode = 0; mode <= 4; ++mode) {
		CHECK(device_stage_sampler(mode, 0, 16) == kBilinearPointMip);
		CHECK(device_stage_sampler(mode, kTextureFlagClamp, 16) == kBilinearPointMip);
		CHECK(device_stage_sampler(mode, kTextureFlagPointMinMag, 16) ==
				sampler(SamplerMinFilter::Point, SamplerMipFilter::Point, 1, false));
	}
}

void check_effects() {
	// The effects' mode: 0, TRILINEAR, ANISO.
	CHECK(effect_texture_filter_mode(0) == 0);
	CHECK(effect_texture_filter_mode(1) == 1);
	CHECK(effect_texture_filter_mode(2) == 2);
	CHECK(effect_texture_filter_mode(3) == 2);
	// _BaseInc.fx's three samplers under each define.
	CHECK(effect_stage_sampler(0, EffectSampler::LinearWrap2D) == kBilinearPointMip);
	CHECK(effect_stage_sampler(1, EffectSampler::LinearWrap2D) == kTrilinear);
	CHECK(effect_stage_sampler(2, EffectSampler::LinearWrap2D) ==
			sampler(SamplerMinFilter::Anisotropic, SamplerMipFilter::Linear, 2));
	CHECK(effect_stage_sampler(2, EffectSampler::LinearClamp2D) ==
			sampler(SamplerMinFilter::Anisotropic, SamplerMipFilter::Linear, 2));
	for (int mode = 0; mode <= 2; ++mode)
		CHECK(effect_stage_sampler(mode, EffectSampler::LinearWrap3D) == kBilinearPointMip);
}

void check_stages() {
	// What each OpenNova stage family draws with at each level.
	struct Row {
		int level;
		int detail_code;
		int detail_anisotropy;
		int object_code;
	};
	constexpr Row rows[] = {
			{0, 0, 1, 0},
			{1, 1, 1, 1},
			{2, 2, 2, 2},
			{3, 2, 16, 2},
	};
	for (const Row &row : rows) {
		const TexFilterState state = texfilter_state(row.level, row.level);
		CHECK(state.device_mode == row.level + 1);
		const StageSampler detail = stage_sampler(TextureStage::TerrainDetail, state);
		CHECK(shader_filter_code(detail) == row.detail_code);
		CHECK(hardware_max_anisotropy(detail) == row.detail_anisotropy);
		CHECK(shader_filter_code(stage_sampler(TextureStage::ObjectStage, state)) ==
				row.object_code);
		// The families created without 0x8, and the cube maps, never move.
		CHECK(stage_sampler(TextureStage::TerrainBlendMap, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::FoliageMask, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::SkyMap, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::ImpactScar, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::TracerSmoke, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::WaterWake, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::Precipitation, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::LightCorona, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::MapIconStrip, state) == kBilinearPointMip);
		CHECK(stage_sampler(TextureStage::ObjectCube, state) == kBilinearPointMip);
	}
	// The two consumers keep their own timing: the session copy drives the
	// device, the configuration word the effects.
	const TexFilterState mixed = texfilter_state(0, 3);
	CHECK(shader_filter_code(stage_sampler(TextureStage::TerrainDetail, mixed)) == 0);
	CHECK(shader_filter_code(stage_sampler(TextureStage::ObjectStage, mixed)) == 2);
	// The state clamps an out-of-range word as the config load does.
	CHECK(texfilter_state(7, -2).device_mode == 4);
	CHECK(texfilter_state(7, -2).effect_mode == 0);
	// The fresh profile: trilinear everywhere the level reaches.
	const TexFilterState fresh;
	CHECK(fresh.device_mode == 2);
	CHECK(fresh.effect_mode == 1);
	CHECK(shader_filter_code(stage_sampler(TextureStage::TerrainDetail, fresh)) == 1);
	CHECK(hardware_max_anisotropy(stage_sampler(TextureStage::TerrainDetail, fresh)) == 1);
}

void check_chains() {
	// GTexture_CreateFromPixelData_0's chain: halvings while the smaller side
	// exceeds 2 (4 x 4 last), one level under 0x40000, at most three under
	// 0x80000, D3DX's full chain for a side of 2 or less.
	CHECK(pixel_texture_last_level(256, 256, 0x100000u) == 6); // the sky maps, the smoke
	CHECK(pixel_texture_last_level(32, 32, 0x100000u) == 3);   // wakegrad.tga
	CHECK(pixel_texture_last_level(128, 128, kTextureFlagClamp) == 5); // the corona
	CHECK(pixel_texture_last_level(64, 64, kTextureFlagThreeLevels | kTextureFlagClamp) == 2);
	CHECK(pixel_texture_last_level(256, 256, kTextureFlagOneLevel) == 0);
	CHECK(pixel_texture_last_level(16, 480, 0x100000u) == 2);
	CHECK(pixel_texture_last_level(2, 2, 0) == 1);
	CHECK(pixel_texture_last_level(1, 1, kTextureFlagThreeLevels) == 0);
	CHECK(pixel_texture_last_level(0, 0, 0) == 0);
}

} // namespace

int main() {
	check_word();
	check_device_mode();
	check_device_stage();
	check_effects();
	check_stages();
	check_chains();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_filter: all checks passed\n");
	return 0;
}
