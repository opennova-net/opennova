// Values shared by object techniques. Execution topology (coverage, texture
// stages, normal source, family) lives in statically selected includes; this
// file contains no capability switches.
//
// ADR 0043 (modern presentation): the object families are LIT materials —
// Godot's sun (SunShadow at MODULATE2X energy), the hemisphere-sky ambient
// and the clustered scene omni lights replaced the hand-written
// fixed-function lighting, the per-draw point-light delivery and the
// environment-cube approximation that lived here. Each technique include
// fills an ObjSurface (albedo/normal/roughness/specular/emission) from the
// witnessed material facts; the output include maps it onto the lit
// pipeline. Additive wrappers and postmultiply remain unshaded emissive
// islands with the retail fade-to-black fog.

#include "res://shaders/color.gdshaderinc"
#include "res://shaders/viewmodel_pass.gdshaderinc"

uniform vec3 u_uv_transform_u = vec3(1.0, 0.0, 0.0);
uniform vec3 u_uv_transform_v = vec3(0.0, 1.0, 0.0);

// Water reflection CLIP class. The reflection camera's exact above-water
// cull mask is WORLD|TERRAIN_SHADOW_RECEIVER (0x8001); below-water mirrors
// add WORLD_NO_MIRROR and retail does not arm the clip class. Water publishes
// the exact mirrored eye and arms the clip only for its above-water
// reflection view; the arming bool is frame-level state shared by every
// camera of that frame, so the eye comparison is what keeps the main view
// and unrelated SubViewports unclipped. The plane is waterHeight - 0.1 and
// applies only to effects with an explicit CLIP block; missing blocks fall
// back to NORMAL and skinned submissions are skipped.
// [orig: Water_RenderReflectedWorldScene @ 0x5c8510;
// HLSLEffect CLIP fallback @ 0x5aebc8; bone skip @ 0x5d94bc;
// _FFP.fx TBoringFFPClip / Glass.fx TGlassFFP_clip].
global uniform bool opennova_water_active;
global uniform float opennova_water_height;
global uniform bool opennova_water_reflection_clip_active;
global uniform vec3 opennova_water_reflection_eye;

// The self-lit/additive gain: the modulator block's ColorSrcGlobalGain (the
// iris auto-exposure chain), the one lighting register the emissive
// islands still consume. [orig: Render_UnpackModulatorToLightScale
// @ 0x58db30 -> handle slot 232]
global uniform vec3 opennova_light_block_gain;
// The device fog state of the scene pass. The lit families take Environment
// fog; these globals feed the additive fade-to-black island and the
// postmultiply source fold. [orig: Environment_ApplyFogAndAmbient @ 0x57e440
// -> Render_SetFogState @ 0x58a950]
global uniform bool opennova_fog_enabled;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
// The per-entity lighting factors: y = the interior-parented flag (entry
// flag bit 1), z = the parent interior's daylight openness t (entry[10]).
// Under ADR 0043 they scale the AMBIENT the lit pipeline applies (an
// interior must not receive full sky ambient); x (the retired sun-visibility
// effectScale) is superseded by per-pixel shadow reception.
// [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]
instance uniform vec4 u_entity_light = vec4(1.0, 0.0, 1.0, 0.0);
// The witnessed ambient averages the interior daylight lerp runs between:
// indoor = (ceiling+floor)/2, outdoor = (sky+ground)/2 [orig:
// RenderBatchCtx_StoreLightingConstants @ 0x5d89e0 — ctx+208 / ctx+224].
global uniform vec3 opennova_outdoor_ambient;
global uniform vec3 opennova_indoor_ambient;
uniform vec4 u_reflect_color = vec4(0.7, 0.8, 0.9, 0.35);

// MATCHTERRAIN is a skinned crouch/prone pre-pass in retail. The reimpl folds
// that pass into the object's NORMAL draw: only pixels rejected by NORMAL's
// alpha test retain the terrain color, so accepted pixels match the retail
// NORMAL overwrite. The composed terrain page is the same cache sampled by
// Terrain and foliage. Per-model material ownership makes the array binding
// safe; per-instance state carries the live stance and resident page.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50, entity+300 & 0x300;
// _tSkFolg.fx Simple_MatchTerrain_PS; _vsSkFolg.fx vscSkMatchTerrain;
// terrain_tile_cache_lookup @ 0x604140].
uniform sampler2DArray u_match_terrain_cache : filter_linear, repeat_disable;
uniform bool u_has_match_terrain_cache = false;
instance uniform bool u_match_terrain_enabled = false;
instance uniform bool u_match_terrain_page_ready = false;
instance uniform float u_match_terrain_page_layer = 0.0;
// xy = page world origin, z = reciprocal world span, w = world span.
instance uniform vec4 u_match_terrain_page_projection = vec4(0.0);

varying vec2 v_uv;
varying vec2 v_uv2;
// Raw TEXCOORD0 is distinct: several non-#UV retail vertex programs bypass
// CalcAnimatedUV even when another pass of the same material uses Diffuse1.
varying vec2 v_raw_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying vec3 v_geom_normal;
varying vec3 v_world_tangent;
varying vec3 v_world_binormal;

// The surface a technique include computes from the witnessed material facts
// and the output include maps onto the lit pipeline.
struct ObjSurface {
	vec3 albedo;
	float alpha;
	vec3 emission;
	vec3 normal_ws;
	float roughness;
	float specular;
	float metallic;
};

ObjSurface obj_surface_defaults(vec4 base, vec3 normal_ws) {
	ObjSurface s;
	s.albedo = base.rgb;
	s.alpha = base.a;
	s.emission = vec3(0.0);
	s.normal_ws = normal_ws;
	s.roughness = 1.0;
	s.specular = 0.0;
	s.metallic = 0.0;
	return s;
}

// The interior ambient island: an interior-parented draw's ambient is
// retail's indoor/outdoor lerp — lerp(indoor, outdoor, daylight openness),
// which bottoms out at the INDOOR (ceiling+floor)/2 ambience, never black
// [orig: setup_entity_lighting_and_shader_constants @ 0x5d9a6a..0x5d9c71].
// The lit pipeline applies the outdoor term (Environment ambient color), so
// the AO scalar is the lerped-to-outdoor luminance ratio (the indoor/outdoor
// hue shift is the accepted ADR 0043 residual of a scalar hook).
float obj_interior_ambient_factor() {
	if (u_entity_light.y <= 0.5) {
		return 1.0;
	}
	float t = clamp(u_entity_light.z, 0.0, 1.0);
	vec3 lum = vec3(0.299, 0.587, 0.114);
	float outdoor = max(dot(opennova_outdoor_ambient, lum), 0.0001);
	float lerped = dot(mix(opennova_indoor_ambient, opennova_outdoor_ambient,
			t), lum);
	return clamp(lerped / outdoor, 0.0, 1.0);
}

// The CLIP-class discard must stay in the one material Godot gives a mesh
// (no per-camera material variant exists), so the depth prepass of every
// explicit-CLIP wrapper keeps this discard. Order the rejection by cost: the
// frame-level arming bool alone decides disarmed frames, and the mirrored-eye
// match and plane test follow only when the mirror is live. Nothing samples a
// texture before this runs.
bool obj_water_reflection_clips(vec3 camera_position_world) {
	if (!opennova_water_reflection_clip_active) {
		return false;
	}
	return opennova_water_active &&
			distance(camera_position_world, opennova_water_reflection_eye) < 0.01 &&
			v_world_pos.y < opennova_water_height - 0.1;
}

void obj_apply_water_reflection_clip(vec3 camera_position_world) {
	#ifdef OBJ_CLIP_EXPLICIT
	if (obj_water_reflection_clips(camera_position_world)) {
		discard;
	}
	#endif
	#ifdef OBJ_CLIP_EXPLICIT_UNSKINNED_OR_SUBMIT_SKIP_SKINNED
	if (obj_water_reflection_clips(camera_position_world)) {
		discard;
	}
	#endif
}

// Full D3D COUNT2 affine transform. Coefficient rows preserve SET, shear,
// scroll, scale, and center rotation. [orig: compute_uv_transform_matrix @ 0x5B1990]
vec2 obj_transform_uv(vec2 uv) {
	vec3 uv1 = vec3(uv, 1.0);
	return vec2(dot(uv1, u_uv_transform_u), dot(uv1, u_uv_transform_v));
}

// The self-lit law: base x the modulator gain under MODULATE2X.
// [orig: _FFP.fx SELFLUM; Render_UnpackModulatorToLightScale @ 0x58db30]
vec3 obj_self_lit(vec3 base_rgb) {
	return base_rgb * min(opennova_light_block_gain, vec3(1.0)) * 2.0;
}

// Witnessed device fog policy, kept for the additive island and the
// postmultiply source fold (the lit families take Environment fog).
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

// The pass fog visibility at this fragment; 1.0 while the pass has no fog.
float obj_scene_fog_visibility(vec3 camera_position_world) {
	if (!opennova_fog_enabled) {
		return 1.0;
	}
	return obj_fog_visibility(
		distance(camera_position_world, v_world_pos),
		opennova_fog_start,
		opennova_fog_end,
		opennova_fog_type
	);
}

// Additive fades toward BLACK with distance, never toward the fog color —
// the intent behind retail's additive fog state (ADR 0043 island).
vec3 obj_apply_additive_fog(vec3 lit, vec3 camera_position_world) {
	return lit * obj_scene_fog_visibility(camera_position_world);
}
