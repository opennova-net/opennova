shader_type spatial;
// ADR 0043 (modern presentation): the terrain is LIT — Godot's
// DirectionalLight3D sun + hemisphere-sky ambient + clustered omni lights
// (+ CSM shadows at stage 4) replace the fixed-function light fold, the
// projected light-pool pass and the hand fog. Environment fog applies.
render_mode blend_mix, depth_draw_opaque, cull_back;

// Shared terrain surface composition (uniforms u_colormap/u_detail*/
// u_blendmap/u_detailmap*: terrain_surface_albedo()).
#include "res://shaders/terrain_lighting.gdshaderinc"
// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"

// Debug: 0=off, 1=LOD colors, 2=sector colors, 3=normals, 4=heightmap
uniform int u_debug_mode = 0;

// Runtime tile-composition cache: one shared 128-layer, single-level array.
// Page identity/LRU/pixels come from engine/runtime/terrain; this shader only
// selects the layer bound to the current terrain instance. The analytic path
// below remains the fail-closed editor/cold-cache fallback.
uniform sampler2DArray u_tile_cache : filter_linear, repeat_disable;
uniform bool u_has_tile_cache = false;

// Per-instance debug data (set via instance_geometry_set_shader_parameter)
instance uniform float u_instance_lod : hint_range(0.0, 7.0) = 0.0;
instance uniform float u_instance_sector_x = 0.0;
instance uniform float u_instance_sector_z = 0.0;
instance uniform vec2 u_instance_source_quadrant = vec2(0.0);
instance uniform bool u_instance_tile_cache_ready = false;
instance uniform float u_instance_tile_cache_layer = 0.0;
// Exact max-quality retail c7/c8 reduction: world origin X/Z, inverse world
// span, and span (diagnostic mirror). No failed-VS fallback matrix is carried.
instance uniform vec4 u_instance_tile_cache_projection = vec4(0.0);

varying vec2 v_colormap_uv;
varying vec2 v_detail_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;

void vertex() {
	v_colormap_uv = UV;
	v_detail_uv = retail_detail_uv_from_atlas(UV, u_detail_density);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

// LOD level to color — 8 distinct colors from green (high detail) to red (low)
vec3 lod_color(float lod) {
	int l = int(lod);
	if (l == 0) return vec3(0.1, 0.8, 0.1);  // green
	if (l == 1) return vec3(0.5, 0.9, 0.1);  // yellow-green
	if (l == 2) return vec3(0.9, 0.9, 0.1);  // yellow
	if (l == 3) return vec3(0.9, 0.6, 0.1);  // orange
	if (l == 4) return vec3(0.9, 0.2, 0.1);  // red
	if (l == 5) return vec3(0.8, 0.1, 0.5);  // magenta
	if (l == 6) return vec3(0.3, 0.1, 0.9);  // blue
	return vec3(0.1, 0.5, 0.9);              // cyan
}

// Sector position to color — deterministic hash for distinct per-sector colors
vec3 sector_color(float sx, float sz) {
	float h = fract(sin(sx * 127.1 + sz * 311.7) * 43758.5453);
	float s = 0.6;
	float v = 0.85;
	// HSV to RGB
	float c = v * s;
	float x = c * (1.0 - abs(mod(h * 6.0, 2.0) - 1.0));
	float m = v - c;
	vec3 rgb;
	float hh = h * 6.0;
	if (hh < 1.0) rgb = vec3(c, x, 0.0);
	else if (hh < 2.0) rgb = vec3(x, c, 0.0);
	else if (hh < 3.0) rgb = vec3(0.0, c, x);
	else if (hh < 4.0) rgb = vec3(0.0, x, c);
	else if (hh < 5.0) rgb = vec3(x, 0.0, c);
	else rgb = vec3(c, 0.0, x);
	return rgb + m;
}

void fragment() {
	vec3 colormap_rgb;
	if (u_has_tile_cache && u_instance_tile_cache_ready) {
		vec2 page_uv = (v_world_pos.xz -
			u_instance_tile_cache_projection.xy) *
			u_instance_tile_cache_projection.z;
		vec2 half_texel = vec2(0.5 / 256.0);
		page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);
		colormap_rgb = texture(u_tile_cache,
			vec3(page_uv, u_instance_tile_cache_layer)).rgb;
	} else {
		vec2 sampled_colormap_uv = clamp_retail_quadrant_uv(
			v_colormap_uv, u_instance_source_quadrant, textureSize(u_colormap, 0));
		colormap_rgb = retail_bare_terrain_tile(
			texture(u_colormap, sampled_colormap_uv).rgb);
		colormap_rgb = compose_retail_tile_overlay(colormap_rgb, v_world_pos);
	}
	vec3 albedo = terrain_surface_albedo(
		colormap_rgb, v_colormap_uv, v_detail_uv, u_instance_source_quadrant);

	// The witnessed texture math above ran in gamma space; the scene target is
	// gamma-domain end to end (color.gdshaderinc), so the lit products stay
	// retail's gamma-space multiplies (D-RMAT-7).
	ALBEDO = scene_output(albedo);
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;

	// Debug overlays — replace output with the diagnostic color (self-lit so
	// the sun/TOD cannot tint diagnostics).
	if (u_debug_mode != 0) {
		vec3 tint = vec3(0.0);
		if (u_debug_mode == 1) {
			tint = mix(albedo * 0.3, lod_color(u_instance_lod), 0.7);
		} else if (u_debug_mode == 2) {
			tint = mix(albedo * 0.3,
				sector_color(u_instance_sector_x, u_instance_sector_z), 0.7);
		} else if (u_debug_mode == 3) {
			tint = v_world_normal * 0.5 + 0.5;
		} else if (u_debug_mode == 4) {
			tint = vec3(clamp(v_world_pos.y / 16.0, 0.0, 1.0));
		}
		ALBEDO = vec3(0.0);
		EMISSION = scene_output(tint);
	}
}
