shader_type spatial;
render_mode unshaded;

// Shared terrain surface shading + fog (uniforms u_detail*/u_blendmap/
// u_detailmap*/lighting/fog all live here): terrain_surface_color_from_colormap(), apply_terrain_fog().
#include "res://shaders/terrain_lighting.gdshaderinc"
// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"

// Debug: 0=off, 1=LOD colors, 2=sector colors, 3=normals, 4=heightmap
uniform int u_debug_mode = 0;

// Runtime tile-composition cache: one shared 128-layer, single-level array.
// Page identity/LRU/pixels come from engine/runtime/terrain, which composes
// every visible page before the frame draws; this shader only selects the
// layer bound to the current terrain instance.
uniform sampler2DArray u_tile_cache : filter_linear, repeat_disable;
uniform bool u_has_tile_cache = false;

// Per-instance debug data (set via instance_geometry_set_shader_parameter)
instance uniform float u_instance_lod : hint_range(0.0, 7.0) = 0.0;
instance uniform float u_instance_sector_x = 0.0;
instance uniform float u_instance_sector_z = 0.0;
instance uniform vec2 u_instance_source_quadrant = vec2(0.0);
instance uniform bool u_instance_zero_height = false;
instance uniform bool u_instance_tile_cache_ready = false;
instance uniform float u_instance_tile_cache_layer = 0.0;
// Exact max-quality retail c7/c8 reduction: world origin X/Z, inverse world
// span, and span (diagnostic mirror). No failed-VS fallback matrix is carried.
instance uniform vec4 u_instance_tile_cache_projection = vec4(0.0);
// The pool slot this patch instance reads its light-pool rows from (draw-list
// index == pool slot, fixed per instance; terrain_lighting.gdshaderinc).
instance uniform float u_instance_light_slot = 0.0;

varying vec2 v_colormap_uv;
varying vec2 v_detail_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_eye_depth;

void vertex() {
	v_colormap_uv = UV;
	v_detail_uv = retail_detail_uv_from_atlas(UV, u_detail_density);
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
	// The flat mode zeros stage 2's blend transform as well as the primary
	// source coordinates. The independent detail/noise stream stays live.
	// [orig: render_terrain_sector_batch @ 0x6092A0, transform 8 @ 0x60973A..0x6097A0]
	vec2 source_uv = u_instance_zero_height ? vec2(0.0) : v_colormap_uv;
	// A patch whose page the cache could not claim this frame draws with t0
	// unbound, which D3D9 samples as (0, 0, 0, 1) (retail
	// PolyTrn_BindStageTextures @ 0x604356 clears the slot before the search).
	vec4 colormap = vec4(0.0, 0.0, 0.0, 1.0);
	if (u_has_tile_cache && u_instance_tile_cache_ready) {
		vec2 page_uv = (v_world_pos.xz -
			u_instance_tile_cache_projection.xy) *
			u_instance_tile_cache_projection.z;
		vec2 half_texel = vec2(0.5 / 256.0);
		page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);
		colormap = texture(u_tile_cache,
			vec3(page_uv, u_instance_tile_cache_layer));
	}
	vec3 result = terrain_surface_color_from_colormap(
		colormap, v_colormap_uv, v_detail_uv, source_uv, u_instance_source_quadrant);

	// A patch with pool lights composites the way retail's lit batch does:
	// the unfogged light passes build the saturated pool, one unfogged
	// DESTCOLOR/SRCCOLOR pass multiplies it by 2 * saturate(2 * page), and the
	// fogged ordinary pass adds on top (renderer::terrain_light_pool_composite;
	// retail render_terrain_sector_batch @0x60984C..0x609AB6).
	if (terrain_point_light_count(u_instance_light_slot) > 0) {
		vec3 pool = clamp(terrain_point_light_pool(v_world_pos, u_instance_light_slot),
			vec3(0.0), vec3(1.0));
		vec3 lit_by_pool = clamp(
			2.0 * clamp(2.0 * colormap.rgb, vec3(0.0), vec3(1.0)) * pool,
			vec3(0.0), vec3(1.0));
		vec3 fogged = apply_terrain_fog(clamp(result, vec3(0.0), vec3(1.0)),
			v_world_pos, CAMERA_POSITION_WORLD, v_eye_depth);
		result = clamp(lit_by_pool + fogged, vec3(0.0), vec3(1.0));
	} else {
		result = apply_terrain_fog(result, v_world_pos, CAMERA_POSITION_WORLD, v_eye_depth);
	}

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
	ALBEDO = scene_output(result);
}
