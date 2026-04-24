shader_type spatial;
render_mode unshaded, cull_disabled;

uniform sampler2D u_heightmap : filter_nearest, repeat_disable;
uniform float u_heightmap_size = 1024.0;

uniform sampler2D u_colormap : source_color, filter_linear, repeat_disable;
uniform sampler2D u_charmap : source_color, filter_nearest, repeat_disable;
uniform sampler2D u_detail_c1 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c2 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c3 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_blendmap : filter_linear, repeat_disable;
uniform sampler2D u_detailmap : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmap2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist2 : filter_linear_mipmap, repeat_enable;
uniform float u_detail_near_dist = 40.0;
uniform float u_detail_far_dist = 80.0;

uniform vec3 u_fill_light = vec3(0.3, 0.3, 0.35);
uniform vec3 u_sun_light = vec3(0.8, 0.75, 0.65);
uniform vec3 u_sky_ambient = vec3(0.4, 0.45, 0.55);
uniform vec3 u_sun_direction = vec3(0.0, 0.70710678, 0.70710678);
uniform vec3 u_terrain_tint = vec3(1.0, 1.0, 1.0);
uniform float u_detail_density = 128.0;
uniform bool u_show_surface_overlay = false;
uniform bool u_show_sector_overlay = false;

uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

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

float jointops_fog_factor(float dist, float fog_start, float fog_end, int fog_type) {
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
	float fog_range = max(safe_end - start, 1.0);
	return clamp((safe_end - dist) / fog_range, 0.0, 1.0);
}


void fragment() {
	vec3 result;
	if (v_sector_id > 0.5) {
		vec4 cm = texture(u_colormap, v_colormap_uv);
		vec3 blend = texture(u_blendmap, v_colormap_uv).rgb;
		vec3 c1 = texture(u_detail_c1, v_detail_uv).rgb;
		vec3 c2 = texture(u_detail_c2, v_detail_uv).rgb;
		vec3 c3 = texture(u_detail_c3, v_detail_uv).rgb;

		vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

		// Distance-based detail normal crossfade (NovaLogic ps.1.4 near/far t1 selection).
		float cam_dist = length(v_world_pos - CAMERA_POSITION_WORLD);
		float detail_range = max(u_detail_far_dist - u_detail_near_dist, 0.0001);
		float fade = clamp((cam_dist - u_detail_near_dist) / detail_range, 0.0, 1.0);
		vec3 nmap_near = texture(u_detailmap, v_detail_uv).rgb;
		vec3 nmap_far = texture(u_detailmapdist, v_detail_uv).rgb;
		vec3 nmap = mix(nmap_near, nmap_far, fade);
		float normal_factor = dot(nmap, blend) * 2.0;

		vec3 combined_sun = u_sun_light * 0.707 + u_sky_ambient;
		vec3 ambient = (cm.a * combined_sun + u_fill_light) * 0.5;

		vec3 cm_rgb = cm.rgb;
		vec3 lit = ambient * cm_rgb * 2.0;
		lit *= normal_factor;

		// Layer-2 normal pair: detailmap2 (near) + detailmapdist2 (far).
		// See terrain.gdshader for the Jointops sub_60AAA0 / gobj ps.1.1 v23 reference.
		vec3 nmap2_near = texture(u_detailmap2, v_detail_uv).rgb;
		vec3 nmap2_far = texture(u_detailmapdist2, v_detail_uv).rgb;
		vec3 nmap2 = mix(nmap2_near, nmap2_far, fade);
		float normal_factor2 = dot(nmap2, blend) * 2.0;
		lit *= normal_factor2;

		result = lit * detail * 4.0;
		result *= u_terrain_tint;

		float dist = length(v_world_pos - CAMERA_POSITION_WORLD);
		float fog_factor = jointops_fog_factor(dist, u_fog_start, u_fog_end, u_fog_type);
		result = mix(u_fog_color, result, fog_factor);
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

	ALBEDO = clamp(result, vec3(0.0), vec3(1.0));
}
