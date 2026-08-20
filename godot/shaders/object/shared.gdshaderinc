// Values and witnessed math shared by object techniques. Execution topology
// (coverage, texture stages, normal source, self-lighting, family) lives in
// statically selected includes; this file contains no capability switches.

#include "res://shaders/nova_color.gdshaderinc"

uniform vec3 u_uv_transform_u = vec3(1.0, 0.0, 0.0);
uniform vec3 u_uv_transform_v = vec3(0.0, 1.0, 0.0);

// Defaults are the shipped full_00.env noon register. Live worlds replace
// them with the post-modulator environment block.
uniform vec3 u_hemi_sky_color = vec3(0.32941, 0.34510, 0.34902);
uniform vec3 u_hemi_ground_color = vec3(0.19216, 0.21569, 0.18039);
uniform vec3 u_dir_light_dir = vec3(-0.4082, -0.8165, -0.4082);
uniform vec3 u_dir_light_color = vec3(0.66667, 0.66667, 0.65490);
uniform vec3 u_color_src_global_gain = vec3(1.0);
uniform bool u_fog_enabled = false;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

// EffectWorld dynamic point lights: the <=4 nearest group-passing pool
// instances for THIS model's draw context (the witness map lives on
// engine/runtime/renderer/light_scene.h — spawn/query/select and the D3D FF
// participation these uniforms stand in for). atten2 = 15/range^2; the
// premultiplied rgb is record x ambient x gen x 1.5; range is the
// D3DLIGHT9.Range hard cutoff.
// Per-instance: each rendered model carries its own draw context's selected
// four (retail selects per draw, owner-gated — update_light_slots @0x5abc50;
// docs/render/render-lighting-re.md). Count rides as float (instance uniforms
// exclude int); posr_N = xyz world + atten2, color_N = premultiplied rgb +
// range.
instance uniform float u_point_light_count = 0.0;
instance uniform vec4 u_point_light_posr_0 = vec4(0.0);
instance uniform vec4 u_point_light_posr_1 = vec4(0.0);
instance uniform vec4 u_point_light_posr_2 = vec4(0.0);
instance uniform vec4 u_point_light_posr_3 = vec4(0.0);
instance uniform vec4 u_point_light_color_0 = vec4(0.0);
instance uniform vec4 u_point_light_color_1 = vec4(0.0);
instance uniform vec4 u_point_light_color_2 = vec4(0.0);
instance uniform vec4 u_point_light_color_3 = vec4(0.0);

// LGHT preview is explicit editor-only state; gameplay binds count zero. The
// GAMEPLAY consumption of model lights is the EffectWorld pool above (spawned
// per placed entity at mission start), not these per-material uniforms.
uniform int u_local_light_count = 0;
uniform vec3 u_local_light_position = vec3(0.0);
uniform vec3 u_local_light_color = vec3(1.0);
uniform float u_local_light_intensity = 1.0;
uniform float u_local_light_atten_start = 0.0;
uniform float u_local_light_atten_end = 5.0;

varying vec2 v_uv;
varying vec2 v_uv2;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying vec3 v_geom_normal;
varying vec3 v_world_tangent;
varying vec3 v_world_binormal;
// EffectWorld point-light diffuse, evaluated at the VERTEX rate like retail's
// D3D vertex-lighting pipeline (the enabled <=4 pool lights join the vertex
// diffuse and Gouraud-interpolate across the triangle; a large low-poly face
// whose vertices all sit outside a light's range stays dark — the witnessed
// armory-wall falloff) [orig: update_light_slots @ 0x5abc50 /
// Light_ApplyAsD3DLight @ 0x5abd50 — D3D LightEnable, fixed-function VERTEX
// lights; Light_FillD3DPointLight @ 0x5aa450 fills a D3DLIGHT9 POINT].
varying vec3 v_point_light_diffuse;

// Full D3D COUNT2 affine transform. Coefficient rows preserve SET, shear,
// scroll, scale, and center rotation. [orig: compute_uv_transform_matrix @ 0x5B1990]
vec2 obj_transform_uv(vec2 uv) {
	vec3 uv1 = vec3(uv, 1.0);
	return vec2(dot(uv1, u_uv_transform_u), dot(uv1, u_uv_transform_v));
}

// Ambient average plus the opposing sky/ground delta lights reduces to this
// hemisphere mix. [orig: RenderBatchCtx_StoreLightingConstants @ 0x5D89E0;
// Lighting_SetHemisphereD3DLights @ 0x5D8CB0]
vec3 obj_hemi(vec3 normal_ws) {
	float up = clamp(normal_ws.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(u_hemi_ground_color, u_hemi_sky_color, up);
}

// The EffectWorld point-light contribution to the same vertex-diffuse sum the
// D3D lights joined: per selected light, color * ndotl / (1 + atten2 * d^2)
// inside the range cutoff. Called from the VERTEX stage (vertex_*.gdshaderinc
// fills v_point_light_diffuse) so the falloff Gouraud-interpolates exactly
// like the fixed-function pipeline the pool lights ride — evaluating this
// per-pixel painted a crisp circular pool retail never draws (the
// 00tra-armory-lght-retail fixture). The hemi/directional terms stay
// per-pixel: they are spatially smooth, so the rate is visually equivalent.
// [orig: enable <=4 @ update_light_slots 0x5abc50 / Light_ApplyAsD3DLight
// @ 0x5abd50; params @ Light_GetPointLightParams 0x5a9180 /
// Light_FillD3DPointLight 0x5aa450]
vec3 obj_point_light_one(vec3 world_pos, vec3 normal_ws, vec4 posr, vec4 col) {
	vec3 to_light = posr.xyz - world_pos;
	float dist = length(to_light);
	if (dist >= col.w) {
		return vec3(0.0);
	}
	float ndotl = max(dot(normal_ws, to_light / max(dist, 0.001)), 0.0);
	return col.rgb * (ndotl / (1.0 + posr.w * dist * dist));
}

vec3 obj_point_light_sum(vec3 world_pos, vec3 normal_ws) {
	vec3 sum = vec3(0.0);
	if (u_point_light_count > 0.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				u_point_light_posr_0, u_point_light_color_0);
	}
	if (u_point_light_count > 1.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				u_point_light_posr_1, u_point_light_color_1);
	}
	if (u_point_light_count > 2.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				u_point_light_posr_2, u_point_light_color_2);
	}
	if (u_point_light_count > 3.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				u_point_light_posr_3, u_point_light_color_3);
	}
	return sum;
}

// D3D vertex diffuse saturates hemi + directional + the enabled point
// lights, then MODULATE2X combines texture and diffuse. [orig: _FFP.fx
// TBoringFFP; D3D light 0 setup @ 0x5D9CE2..0x5D9D76]
vec3 obj_ff_lighting(vec3 base_rgb, vec3 normal_ws) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-u_dir_light_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	return base_rgb * min(obj_hemi(normal) + u_dir_light_color * ndotl
			+ v_point_light_diffuse, vec3(1.0)) * 2.0;
}

vec3 obj_self_lit(vec3 base_rgb) {
	return base_rgb * min(u_color_src_global_gain, vec3(1.0)) * 2.0;
}

vec3 obj_add_local_light(vec3 lit, vec3 base_rgb, vec3 normal_ws) {
	if (u_local_light_count <= 0) {
		return lit;
	}
	vec3 to_light = u_local_light_position - v_world_pos;
	float dist = length(to_light);
	if (dist >= u_local_light_atten_end) {
		return lit;
	}
	float range = max(u_local_light_atten_end - u_local_light_atten_start, 0.001);
	float atten = 1.0 - clamp((dist - u_local_light_atten_start) / range, 0.0, 1.0);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndotl = max(dot(normalize(normal_ws), light_dir), 0.0);
	return lit + base_rgb * u_local_light_color *
			(u_local_light_intensity * ndotl * atten);
}

// Witnessed device fog policy shared with terrain and water.
// [orig: Render_SetFogState @ 0x58A950; CD3DDevice_SetFogParameters @ 0x677960]
float obj_fog_visibility(float dist, float fog_start, float fog_end, int fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}
	float start = fog_start;
	if (fog_type == 2) {
		start = safe_end * 0.5;
	} else if (fog_type == 3) {
		start = safe_end * 0.25;
	}
	return clamp((safe_end - dist) / max(safe_end - start, 1.0), 0.0, 1.0);
}

vec3 obj_apply_fog(vec3 lit, vec3 camera_position_world) {
	if (!u_fog_enabled) {
		return lit;
	}
	float visibility = obj_fog_visibility(
		distance(camera_position_world, v_world_pos),
		u_fog_start,
		u_fog_end,
		u_fog_type
	);
	return mix(u_fog_color, lit, visibility);
}
