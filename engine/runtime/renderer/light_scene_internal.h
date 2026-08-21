// Helpers shared by the LightScene translation units (light_scene.cpp and
// light_terrain_pass.cpp). Not part of the library's public surface.
#pragma once

#include <renderer/light_scene.h>
#include <renderer/material_eval.h>

#include <array>
#include <cstdint>

namespace renderer::detail {

inline constexpr uint16_t kHandleFlag = 0x8000; // [orig: @ 0x5a8e94]
inline constexpr uint16_t kHandleIndexMask = 0x7FFF;

// The RgbGen multiply shared by the point-light select, the corona walk and
// the terrain projected pass: tick the FLICKER register from the wave ring,
// then scale by the gen's color x intensity [orig: Light_GetPointLightParams
// @ 0x5a9211..0x5a9243; the corona walk's copy @ 0x5ab149..0x5ab1b8; the
// terrain pass's copy Light_SetupTerrainProjectedPass @ 0x5aaa05..0x5aaa5f].
inline void apply_rgb_gen(const LightSpawnParams &params,
		const LightFlickerInputs &flicker, std::array<float, 3> &rgb) {
	if (!params.has_gen || params.gen.style == 0) {
		return;
	}
	const int32_t ctrl = light_flicker_value(params.position_fixed, flicker);
	const LightRuntime gen = eval_light_runtime(params.gen.style,
			params.gen.phase, params.gen.rate, params.gen.color_start,
			params.gen.color_end, flicker.time_ms, ctrl);
	rgb[0] *= gen.r * gen.intensity;
	rgb[1] *= gen.g * gen.intensity;
	rgb[2] *= gen.b * gen.intensity;
}

} // namespace renderer::detail
