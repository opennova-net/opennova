shader_type spatial;
render_mode unshaded, cull_disabled;

// Shared terrain surface shading + fog (uniforms u_colormap/u_detail*/u_blendmap/
// u_detailmap*/lighting/fog all live here): terrain_surface_color_from_colormap(), apply_terrain_fog().
#include "res://shaders/terrain_lighting.gdshaderinc"
// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Editor-only extras: a live heightmap (the editor sculpts geometry the runtime
// bakes into CPT), a surface/sector inspection overlay, and the paint brush ring.
uniform sampler2D u_heightmap : filter_nearest, repeat_disable;
uniform float u_heightmap_size = 1024.0;
// Raw sampling (editor diagnostic view rides the same convention so the
// painted surface matches the runtime look).
uniform sampler2D u_charmap : filter_nearest, repeat_disable;
uniform bool u_show_surface_overlay = false;
uniform bool u_show_sector_overlay = false;
// The shell's View > Show grid guide: thin NEUTRAL sector-boundary lines that
// follow the surface. Distinct from u_show_sector_overlay (the Layout
// workflow's colored per-sector diagnostic with its tint fill) — a reference
// grid must not wash the map in color.
uniform bool u_show_grid = false;

uniform vec2 u_brush_pos = vec2(-10000.0, -10000.0);
uniform float u_brush_radius = 10.0;
uniform float u_brush_hardness = 0.5;
uniform float u_brush_ring_width = 2.0;
uniform vec3 u_brush_color = vec3(1.0, 1.0, 0.2);

uniform vec3 u_empty_sector_fill = vec3(0.16, 0.17, 0.19);

instance uniform float u_instance_sector_id = 0.0;

varying vec2 v_colormap_uv;
varying vec2 v_detail_uv;
varying vec2 v_sector_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying float v_sector_id;
varying float v_eye_depth;


vec2 sector_offset(float sector_id) {
	int sid = int(sector_id + 0.5);
	if (sid == 3) {
		return vec2(0.5, 0.0);
	}
	if (sid == 2) {
		return vec2(0.0, 0.5);
	}
	if (sid == 4) {
		return vec2(0.5, 0.5);
	}
	return vec2(0.0, 0.0);
}


void vertex() {
	v_sector_id = u_instance_sector_id;
	v_sector_uv = UV;

	float height = 0.0;
	vec2 source_uv = UV * 0.5 + sector_offset(u_instance_sector_id);

	if (u_instance_sector_id > 0.5) {
		// Cyclic wrap: atlas pixel past the edge must tile back to pixel 0.
		// Needed for .trn files with wrap_x/wrap_y=1 and grids that repeat the
		// canonical 1/3/2/4 pattern — seams at non-canonical transitions (3→1,
		// 4→3 horizontally; 2→1, 4→3 vertically) rely on this wrap.
		// Matches src/terrain/nova_terrain_data.cpp:357-358 (& hm_size-1).
		int hm_mask = int(u_heightmap_size) - 1;
		ivec2 hm_coord = ivec2(source_uv * u_heightmap_size) & ivec2(hm_mask);
		height = texelFetch(u_heightmap, hm_coord, 0).r;
		VERTEX.y = height;

		float hL = texelFetch(u_heightmap, ivec2((hm_coord.x - 1) & hm_mask, hm_coord.y), 0).r;
		float hR = texelFetch(u_heightmap, ivec2((hm_coord.x + 1) & hm_mask, hm_coord.y), 0).r;
		float hD = texelFetch(u_heightmap, ivec2(hm_coord.x, (hm_coord.y - 1) & hm_mask), 0).r;
		float hU = texelFetch(u_heightmap, ivec2(hm_coord.x, (hm_coord.y + 1) & hm_mask), 0).r;
		NORMAL = normalize(vec3(hL - hR, 2.0, hD - hU));
	} else {
		NORMAL = vec3(0.0, 1.0, 0.0);
	}

	v_colormap_uv = source_uv;
	v_detail_uv = source_uv * u_detail_density;
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_eye_depth = max(-(MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).z, 0.0);
}


vec3 sector_overlay_color(float sector_id) {
	int sid = int(sector_id + 0.5);
	if (sid == 1) {
		return vec3(0.28, 0.68, 0.36);
	}
	if (sid == 2) {
		return vec3(0.82, 0.72, 0.20);
	}
	if (sid == 3) {
		return vec3(0.26, 0.44, 0.82);
	}
	if (sid == 4) {
		return vec3(0.86, 0.34, 0.30);
	}
	return vec3(0.22, 0.23, 0.26);
}


void fragment() {
	vec3 result;
	if (v_sector_id > 0.5) {
		vec2 quadrant = sector_offset(v_sector_id) * 2.0;
		vec2 sampled_colormap_uv = clamp_retail_quadrant_uv(
			v_colormap_uv, quadrant, textureSize(u_colormap, 0));
		vec4 colormap = retail_bare_terrain_tile(
			texture(u_colormap, sampled_colormap_uv).rgb, v_world_normal,
			v_colormap_uv, quadrant);
		colormap = compose_retail_tile_overlay(colormap, v_world_pos);
		result = terrain_surface_color_from_colormap(
			colormap, v_colormap_uv, v_detail_uv, quadrant);
		result = apply_terrain_fog(result, v_world_pos, CAMERA_POSITION_WORLD, v_eye_depth);
		if (u_show_surface_overlay) {
			vec3 surface_rgb = texture(u_charmap, v_colormap_uv).rgb;
			result = mix(result * 0.25, surface_rgb, 0.75);
		}
		if (u_show_sector_overlay) {
			float edge = min(min(v_sector_uv.x, 1.0 - v_sector_uv.x), min(v_sector_uv.y, 1.0 - v_sector_uv.y));
			float border = 1.0 - smoothstep(0.0, 0.015, edge);
			float overlay_alpha = max(0.14, border * 0.72);
			result = mix(result, sector_overlay_color(v_sector_id), overlay_alpha);
		}
		if (u_show_grid) {
			float edge = min(min(v_sector_uv.x, 1.0 - v_sector_uv.x), min(v_sector_uv.y, 1.0 - v_sector_uv.y));
			float line = 1.0 - smoothstep(0.0, 0.006, edge);
			result = mix(result, vec3(0.85), line * 0.45);
		}
	} else {
		result = u_empty_sector_fill;
	}

	if (v_sector_id > 0.5) {
		float brush_dist = length(v_world_pos.xz - u_brush_pos);
		float core_radius = u_brush_radius * clamp(u_brush_hardness, 0.0, 1.0);
		float fill_alpha = 0.0;
		if (brush_dist < u_brush_radius) {
			float shoulder = max(u_brush_radius - core_radius, 0.0001);
			if (brush_dist <= core_radius) {
				fill_alpha = 0.12;
			} else {
				float u = clamp((u_brush_radius - brush_dist) / shoulder, 0.0, 1.0);
				fill_alpha = 0.12 * u * u * (3.0 - 2.0 * u);
			}
		}
		float ring = abs(brush_dist - u_brush_radius);
		float ring_alpha = 1.0 - smoothstep(0.0, u_brush_ring_width, ring);
		float core_ring = 1.0 - smoothstep(0.0, max(u_brush_ring_width * 0.6, 0.6), abs(brush_dist - core_radius));
		result = mix(result, u_brush_color, max(fill_alpha, ring_alpha * 0.8));
		if (core_radius > 0.0 && core_radius < u_brush_radius) {
			result = mix(result, u_brush_color, core_ring * 0.35);
		}
	}

	// Gamma-space output (D-RMAT-7): the surface math above ran on raw bytes;
	// encode once. Editor overlays/brush tints ride the same transform.
	ALBEDO = nova_gamma_to_linear(result);
}
