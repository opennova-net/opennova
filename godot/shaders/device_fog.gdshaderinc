#ifndef OPENNOVA_DEVICE_FOG_GDSHADERINC
#define OPENNOVA_DEVICE_FOG_GDSHADERINC

// The device fog laws for the .gdshader passes: the functions below are
// engine/runtime/renderer/device_fog.h kDeviceFogGlsl verbatim (the
// renderer_device_fog ctest fails when they differ); the C++ forms beside it
// are the CPU's. Each takes the Render_SetFogState pair as resolved:
// - device_fog_visibility: a fixed-function pass's fog at a point view_depth
//   along the eye's forward axis and radial_distance from the eye. A
//   start == end pair parks the fog at LINEAR 1000..1001 over the view depth,
//   type 0 is EXP at density ln(64)/end over the view depth, every other type
//   LINEAR over the radial distance [orig: CD3DDevice_SetFogParameters
//   @ 0x677960].
// - device_fog_exponential: whether that pair runs table EXP fog, which
//   replaces a vertex shader's own oFog.
// - vertex_shader_fog_visibility: a vertex-shader pass's oFog, linear in the
//   view depth for every type, a step at the start for a start == end pair
//   [orig: Material_ApplyShaderParameters @ 0x58e20d..0x58e239].
bool device_fog_exponential(float fog_start, float fog_end, int fog_type) {
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

#endif
