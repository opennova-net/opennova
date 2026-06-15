// Shared object-material shading — a faithful port of the original engine's
// `_BaseInc.fx` effect header (the file every object `.fx` `#include`s). The
// uniforms keep the names the editor already feeds (nova_object_model.gd), so
// only the *math* changed from the pre-witness builder. Witness map and the
// stable divergence ids are in docs/renderer/renderer-re.md.
//
// Conventions carried over from the originals:
//  * 2x overbright — every combine ends in Modulate2x / mul_x2 (D-RENDER-1).
//  * Hemisphere ambient lerps a ground colour to a sky colour by up-ness
//    [orig: HemicolorFromVectorY in _BaseInc.fx] (D-RENDER-2).
//  * Directional self-shadow wrap clamp(N.L*10+0.5)
//    [orig: CalcSelfShadowTerm in _BaseInc.fx] (D-RENDER-6).
//  * Planar view-Z fog 1-(viewZ-start)*rangeRecip
//    [orig: CalcFogSegmented in _BaseInc.fx] (D-RENDER-3).
//  * D3D point attenuation 1/(c+l*d+q*d^2)
//    [orig: CalcPointLightAttenuation in _BaseInc.fx] (D-RENDER-4).

// 2x overbright. The single biggest reason the pre-witness output looked dim.
const float OBJ_OVERBRIGHT = 2.0;

// --- Textures ----------------------------------------------------------------
uniform sampler2D u_diffuse : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;

// --- UV transform ------------------------------------------------------------
// Approximate scale/rotate/offset. The original applies a full 3x2 texture
// matrix (MatTexCoord1); the matrix feed lands with the runtime param grill
// (renderer-re.md D-RENDER-8 / open questions).
uniform vec2 u_uv_offset = vec2(0.0);
uniform vec2 u_uv_scale = vec2(1.0);
uniform float u_uv_rotation = 0.0;

// --- Per-material modulation -------------------------------------------------
uniform vec3 u_rgb_mod = vec3(1.0);
uniform float u_alpha_mod = 1.0;
uniform float u_alpha_test_threshold = 0.5;
uniform float u_alpha_test_invert = 0.0;
uniform vec4 u_reflect_color = vec4(0.75, 0.75, 0.75, 0.75);
uniform float u_emissive = 0.0;

// --- Lighting ----------------------------------------------------------------
// u_dir_light_dir points along light *travel* (engine DirLightVector = -this).
// u_ambient_color is the hemisphere SKY/up colour (HemiSkyColor);
// u_fill_light_color is the hemisphere GROUND colour (HemiGroundColor).
uniform vec3 u_dir_light_dir = vec3(-0.4082, -0.8165, -0.4082);
uniform vec3 u_dir_light_color = vec3(0.85, 0.82, 0.75);
uniform vec3 u_ambient_color = vec3(0.35, 0.36, 0.40);
uniform vec3 u_fill_light_color = vec3(0.18, 0.20, 0.25);

// --- Fog (planar view-Z) -----------------------------------------------------
uniform bool u_fog_enabled = false;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

// --- Point light (single; the ≤4 PointLight*Array path arrives with the param
//     grill, D-RENDER-9). NEAR/FAR range stands in for the c/l/q triple. -------
uniform int u_local_light_count = 0;
uniform vec3 u_local_light_position = vec3(0.0);
uniform vec3 u_local_light_color = vec3(1.0);
uniform float u_local_light_intensity = 1.0;
uniform float u_local_light_atten_start = 0.0;
uniform float u_local_light_atten_end = 5.0;

// --- Flag wind (VS_FLAG) -----------------------------------------------------
uniform float u_wind_amount = 1.0;
uniform float u_wind_phase = 0.0;

vec2 obj_transform_uv(vec2 uv) {
	vec2 c = (uv - vec2(0.5)) * u_uv_scale;
	float cs = cos(u_uv_rotation);
	float sn = sin(u_uv_rotation);
	return vec2(cs * c.x - sn * c.y, sn * c.x + cs * c.y) + vec2(0.5) + u_uv_offset;
}

// Hemisphere ambient: lerp(ground, sky, up). [orig: HemicolorFromVectorY in _BaseInc.fx]
vec3 obj_hemisphere(vec3 n) {
	float up = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(u_fill_light_color, u_ambient_color, up);
}

// Directional self-shadow wrap. [orig: CalcSelfShadowTerm in _BaseInc.fx]
float obj_self_shadow(float ndotl_geom) {
	return clamp(ndotl_geom * 10.0 + 0.5, 0.0, 1.0);
}

// Additive point-light contribution, D3D-style attenuation approximated from a
// NEAR/FAR pair: atten(start)=1, falling to ~0 past end. [orig: CalcPointLightAttenuation]
vec3 obj_point_light(vec3 base_rgb, vec3 n, vec3 world_pos) {
	if (u_local_light_count <= 0) {
		return vec3(0.0);
	}
	vec3 to_light = u_local_light_position - world_pos;
	float dist = length(to_light);
	float start = u_local_light_atten_start;
	float end = max(u_local_light_atten_end, start + 0.001);
	float d = max(dist - start, 0.0);
	float lin = 1.0 / (end - start);
	float atten = 1.0 / (1.0 + lin * d + lin * lin * d * d);
	float ndotl = max(dot(normalize(n), normalize(to_light)), 0.0);
	return base_rgb * u_local_light_color * (u_local_light_intensity * ndotl * atten);
}

// Planar view-Z fog. factor 0 = clear, 1 = full fog. [orig: CalcFogSegmented in _BaseInc.fx]
vec3 obj_apply_fog(vec3 color, float view_z) {
	if (!u_fog_enabled) {
		return color;
	}
	float range = max(u_fog_end - u_fog_start, 0.001);
	float f = clamp((view_z - u_fog_start) / range, 0.0, 1.0);
	return mix(color, u_fog_color, f);
}
