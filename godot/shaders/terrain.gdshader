shader_type spatial;
render_mode unshaded;

// Shared terrain surface shading + fog (uniforms u_colormap/u_detail*/u_blendmap/
// u_detailmap*/lighting/fog all live here): terrain_surface_color_from_colormap(), apply_terrain_fog().
#include "res://shaders/terrain_lighting.gdshaderinc"
// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Debug: 0=off, 1=LOD colors, 2=sector colors, 3=normals, 4=heightmap
uniform int u_debug_mode = 0;

// Per-instance debug data (set via instance_geometry_set_shader_parameter)
instance uniform float u_instance_lod : hint_range(0.0, 7.0) = 0.0;
instance uniform float u_instance_sector_x = 0.0;
instance uniform float u_instance_sector_z = 0.0;
instance uniform vec2 u_instance_source_quadrant = vec2(0.0);

varying vec2 v_colormap_uv;
varying vec2 v_detail_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_eye_depth;

void vertex() {
	v_colormap_uv = UV;
	v_detail_uv = UV * u_detail_density;
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_eye_depth = max(-(MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).z, 0.0);
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
	vec2 sampled_colormap_uv = clamp_retail_quadrant_uv(
		v_colormap_uv, u_instance_source_quadrant, textureSize(u_colormap, 0));
	vec4 colormap = retail_bare_terrain_tile(
		texture(u_colormap, sampled_colormap_uv).rgb, v_world_normal,
		v_colormap_uv, u_instance_source_quadrant);
	colormap = compose_retail_tile_overlay(colormap, v_world_pos);
	vec3 result = terrain_surface_color_from_colormap(
		colormap, v_colormap_uv, v_detail_uv, u_instance_source_quadrant);

	result = apply_terrain_fog(result, v_world_pos, CAMERA_POSITION_WORLD, v_eye_depth);

	// Debug overlays — replace output with diagnostic color
	if (u_debug_mode == 1) {
		// LOD colors: debug tint blended over dimmed terrain
		vec3 tint = lod_color(u_instance_lod);
		result = mix(result * 0.3, tint, 0.7);
	} else if (u_debug_mode == 2) {
		// Sector colors: unique color per sector grid cell
		vec3 tint = sector_color(u_instance_sector_x, u_instance_sector_z);
		result = mix(result * 0.3, tint, 0.7);
	} else if (u_debug_mode == 3) {
		// World normals as RGB
		result = v_world_normal * 0.5 + 0.5;
	} else if (u_debug_mode == 4) {
		// Heightmap grayscale from world Y
		float h = clamp(v_world_pos.y / 16.0, 0.0, 1.0);
		result = vec3(h);
	}

	// The witnessed math above ran in gamma space; encode once so the display
	// shows exactly those bytes (D-RMAT-7). Debug overlays ride the same
	// transform (diagnostic-only).
	ALBEDO = nova_gamma_to_linear(result);
}
