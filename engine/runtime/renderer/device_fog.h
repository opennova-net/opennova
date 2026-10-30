#pragma once

// The witnessed device fog evaluation shared by every retail pass that runs
// the primary fog state: the object/terrain/water shaders, the effect and
// particle passes, the corona fog-to-black fold, and the Q3 bloom-source
// copies. `start` and `end` are the values Render_SetFogState already resolved
// per fog type (formats/env compute_fog_params: type 1 = the caller's 0.5,
// types 2/3 = (1 - overcast) * end * {0.5, 0.25}); this law never re-derives
// them. CD3DDevice_SetFogParameters turns the pair into the primary block that
// CD3DDevice_SetFogAndBlendMode mode 0 applies (FOGTABLEMODE @ 0x677843,
// FOGVERTEXMODE, FOGSTART, FOGEND, FOGDENSITY, RANGEFOGENABLE @ 0x6778b2):
// - start == end: vertex LINEAR fog parked at 1000..1001 with range fog off
//   (@ 0x677973..0x6779a7), so the fog begins at view depth 1000;
// - type 0: EXP at density ln(64)/end, table fog or non-range vertex fog, so
//   over the view depth (@ 0x6779be..0x677a48);
// - every other type: vertex LINEAR from start to end with range fog on (the
//   NORMAL and LITE RANGEFOGENABLE words @ 0x677a5d / @ 0x677a69), so over
//   the radial distance from the eye (@ 0x677a50..0x677a7f).
// There is no other guard: start != end keeps the linear divisor nonzero.
// [orig: Render_SetFogState @ 0x58a950; CD3DDevice_SetFogParameters
// @ 0x677960 (useLinear = fog_type != 0, exponential density ln(64)/end)]

#include <algorithm>
#include <cmath>

namespace opennova::renderer {

// ln(64): the exponential density numerator (D3DFOG_EXP reaches 1/64 at end).
inline constexpr float kDeviceFogLn64 = 4.1588830833596715f;

// The degenerate pair's parked linear range [orig: CD3DDevice_SetFogParameters
// FOGSTART 1000.0 @ 0x67798b, FOGEND 1001.0 @ 0x677997].
inline constexpr float kDeviceFogParkedStart = 1000.0f;
inline constexpr float kDeviceFogParkedEnd = 1001.0f;

// Whether the device runs the EXP arm for this pair: type 0 over a range that
// is not parked. Table EXP fog replaces a vertex shader's oFog; every other
// arm leaves a vertex-shader pass its own oFog.
inline bool device_fog_exponential(float start, float end, int type) {
	return type == 0 && start != end;
}

// Visibility (1 = unfogged, 0 = fully fogged) of a fixed-function pass's point
// `view_depth` along the eye's forward axis and `radial_distance` from the eye.
inline float device_fog_visibility(float view_depth, float radial_distance,
		float start, float end, int type, bool enabled) {
	if (!enabled) {
		return 1.0f;
	}
	if (start == end) {
		return std::clamp((kDeviceFogParkedEnd - view_depth) /
						(kDeviceFogParkedEnd - kDeviceFogParkedStart),
				0.0f, 1.0f);
	}
	if (type == 0) {
		return std::clamp(
				std::exp(-std::max(view_depth, 0.0f) * (kDeviceFogLn64 / end)),
				0.0f, 1.0f);
	}
	return std::clamp((end - radial_distance) / (end - start), 0.0f, 1.0f);
}

// A vertex-shader pass's own oFog over the same pair, for every fog type:
// `oFog = 1 - (z - FogStart) * FogRangeRecip` over clip z, the pair pushed
// through the projection, so linear in the view depth from start to end; a
// start == end pair makes FogRangeRecip 1/0, a step at the start
// [orig: Material_ApplyShaderParameters @ 0x58e20d..0x58e239 (FogStart,
// FogRangeRecip = 1 / (end' - start')); g_FoliageWindSwayVS `mad oFog, -r1.x,
// c5.y, c5.z` @ 0x7de648, its c5 from Foliage_SetupVertexShaderConstants
// @ 0x6005ee..0x600652].
inline float vertex_shader_fog_visibility(float view_depth, float start, float end) {
	const float range = end - start;
	if (range == 0.0f) {
		return view_depth < start ? 1.0f : 0.0f;
	}
	return std::clamp((end - view_depth) / range, 0.0f, 1.0f);
}

// The same laws in the shading-language subset GLSL 450 and Godot's shader
// language share, for the GPU passes: the RenderingDevice passes splice them
// into their @DEVICE_FOG@ slot, and godot/shaders/device_fog.gdshaderinc
// carries them verbatim for the .gdshader passes (the renderer_device_fog
// ctest fails when that file's copy differs from this text).
inline constexpr const char kDeviceFogGlsl[] = R"GLSL(bool device_fog_exponential(float fog_start, float fog_end, int fog_type) {
	return fog_type == 0 && fog_start != fog_end;
}

float device_fog_visibility(float view_depth, float radial_distance, float fog_start,
		float fog_end, int fog_type, bool fog_enabled) {
	if (!fog_enabled) {
		return 1.0;
	}
	if (fog_start == fog_end) {
		return clamp((1001.0 - view_depth) / (1001.0 - 1000.0), 0.0, 1.0);
	}
	if (fog_type == 0) {
		return clamp(exp(-max(view_depth, 0.0) * (4.1588830833596715 / fog_end)), 0.0, 1.0);
	}
	return clamp((fog_end - radial_distance) / (fog_end - fog_start), 0.0, 1.0);
}

float vertex_shader_fog_visibility(float view_depth, float fog_start, float fog_end) {
	float fog_range = fog_end - fog_start;
	if (fog_range == 0.0) {
		return view_depth < fog_start ? 1.0 : 0.0;
	}
	return clamp((fog_end - view_depth) / fog_range, 0.0, 1.0);
}
)GLSL";

} // namespace opennova::renderer
