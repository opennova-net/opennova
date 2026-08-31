shader_type spatial;
// ADR 0043 (modern presentation): the terrain is LIT — Godot's
// DirectionalLight3D sun (CSM shadows via ATTENUATION), clustered omni
// lights and Environment fog. The RESPONSE is the witnessed terrain fold —
// `sun_mask * sun_light + sky_ambient` per channel, flat sun with no N.L
// (the DOT3 normal term already rides the tile-cache albedo) and no
// MODULATE2X ("the halving and doubling cancel") [orig:
// compile_terrain_pixel_shaders @ 0x605260 shape; c0/c1 push @ 0x604420] —
// expressed through the canonical hooks: a custom light() flattens the sun
// and halves its MODULATE2X energy, EMISSION carries the flat sky term, and
// the pipeline ambient (the object family's outdoor average) is disabled.
render_mode blend_mix, depth_draw_opaque, cull_back, ambient_light_disabled;

// The env sky ambient (TOD-updated by MissionEnvironment).
global uniform vec3 opennova_sky_ambient;

// Shared terrain surface composition (uniforms u_colormap/u_detail*/
// u_blendmap/u_detailmap*: terrain_surface_albedo()).
#include "res://shaders/terrain_lighting.gdshaderinc"

// Debug: 0=off, 1=LOD colors, 2=sector colors, 3=normals, 4=heightmap
uniform int u_debug_mode = 0;

// Runtime tile-composition cache: one shared 128-layer, single-level array.
// Page identity/LRU/pixels come from engine/runtime/terrain; this shader only
// selects the layer bound to the current terrain instance. The analytic path
// below remains the fail-closed editor/cold-cache fallback. source_color:
// the CPU-composed pages hold retail gamma art — the sRGB view decodes RGB
// only, so the DOT3 alpha mask stays linear.
uniform sampler2DArray u_tile_cache : source_color, filter_linear, repeat_disable;
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

	// The witnessed splat combine keeps its text and constants over linear
	// inputs (source_color decodes the art; the scene is linear, ADR 0043
	// linear-scene amendment); the clamp is retail's ps.1.x saturate-on-write.
	ALBEDO = clamp(albedo, vec3(0.0), vec3(1.0));
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	// The witnessed flat sky term (ambient_light_disabled above): retail adds
	// sky_ambient unconditionally — never shadow-masked, never N-dependent.
	EMISSION = clamp(albedo, vec3(0.0), vec3(1.0)) * opennova_sky_ambient;

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
		EMISSION = clamp(tint, vec3(0.0), vec3(1.0));
	}
}

void light() {
	// The witnessed response: the sun contributes mask * sun_light — flat (no
	// N.L: the DOT3 normal term rides the tile-cache albedo) with ATTENUATION
	// as the CSM mask, and no MODULATE2X (LIGHT_COLOR arrives as
	// color * energy(2) * PI, so 0.5/PI restores sun_light * mask). Retail
	// terrain takes NO hemisphere term [orig: the fold @ 0x605260 has only
	// mask*sun + sky] — the scene's hemisphere delta pair excludes this
	// layer via light_cull_mask (SunShadow::_make_hemi_light), so every
	// directional reaching this fragment is the sun. Omni lights keep the
	// canonical Lambert response (MP-1's clustered pool).
	if (LIGHT_IS_DIRECTIONAL) {
		DIFFUSE_LIGHT += LIGHT_COLOR * (0.5 / PI) * ATTENUATION * ALBEDO;
	} else {
		DIFFUSE_LIGHT += clamp(dot(NORMAL, LIGHT), 0.0, 1.0) *
				LIGHT_COLOR * (1.0 / PI) * ATTENUATION * ALBEDO;
	}
}
