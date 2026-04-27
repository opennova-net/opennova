shader_type spatial;
render_mode unshaded;

uniform sampler2D u_colormap : source_color, filter_linear, repeat_disable;
uniform sampler2D u_detail_c1 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c2 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c3 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_blendmap : filter_linear, repeat_disable;
uniform sampler2D u_detailmap : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmap2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_tile_overlay : source_color, filter_linear, repeat_enable;
uniform bool u_has_tile_overlay = false;
uniform float u_detail_near_dist = 40.0;
uniform float u_detail_far_dist = 80.0;
uniform vec3 u_fill_light = vec3(0.3, 0.3, 0.35);
uniform vec3 u_sun_light = vec3(0.8, 0.75, 0.65);
uniform vec3 u_sky_ambient = vec3(0.4, 0.45, 0.55);
uniform vec3 u_sun_direction = vec3(0.0, 0.70710678, 0.70710678);
uniform vec3 u_terrain_tint = vec3(1.0, 1.0, 1.0);
uniform vec3 u_tile_overlay_tint = vec3(1.0, 1.0, 1.0);
uniform float u_detail_density = 128.0;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

// Debug: 0=off, 1=LOD colors, 2=sector colors, 3=normals, 4=heightmap
uniform int u_debug_mode = 0;

// Per-instance debug data (set via instance_geometry_set_shader_parameter)
instance uniform float u_instance_lod : hint_range(0.0, 7.0) = 0.0;
instance uniform float u_instance_sector_x = 0.0;
instance uniform float u_instance_sector_z = 0.0;

varying vec2 v_colormap_uv;
varying vec2 v_detail_uv;
varying vec3 v_world_pos;
varying vec3 v_world_normal;

void vertex() {
	v_colormap_uv = UV;
	v_detail_uv = UV * u_detail_density;
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
	// Sample textures
	vec4 cm = texture(u_colormap, v_colormap_uv);
	vec3 blend = texture(u_blendmap, v_colormap_uv).rgb;
	vec3 c1 = texture(u_detail_c1, v_detail_uv).rgb;
	vec3 c2 = texture(u_detail_c2, v_detail_uv).rgb;
	vec3 c3 = texture(u_detail_c3, v_detail_uv).rgb;

	// mul/mad: detail = c1*R + c2*G + c3*B
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// Distance-based detail normal crossfade (NovaLogic ps.1.4 near/far t1 selection).
	// Thresholds are world-unit ports of the original 2,621,440 / 5,242,880 (16.16 fixed-point).
	float cam_dist = length(v_world_pos - CAMERA_POSITION_WORLD);
	float detail_range = max(u_detail_far_dist - u_detail_near_dist, 0.0001);
	float fade = clamp((cam_dist - u_detail_near_dist) / detail_range, 0.0, 1.0);
	vec3 nmap_near = texture(u_detailmap, v_detail_uv).rgb;
	vec3 nmap_far = texture(u_detailmapdist, v_detail_uv).rgb;
	vec3 nmap = mix(nmap_near, nmap_far, fade);

	// dp3: dot(normalmap, blendweights) -> per-material normal response
	float normal_factor = dot(nmap, blend) * 2.0;

	// mad_d2: ambient = (alpha * combinedSun + fillLight) / 2
	// combinedSun = sunLight * 0.707 + skyAmbient
	// NovaLogic ps.1.4 dual-normal path reads colormap.alpha directly.
	vec3 combined_sun = u_sun_light * 0.707 + u_sky_ambient;
	vec3 ambient = (cm.a * combined_sun + u_fill_light) * 0.5;

	// mul_x2: lit = ambient * colormap * 2
	vec3 lit = ambient * cm.rgb * 2.0;

	// mul_x2: lit *= normalFactor
	lit *= normal_factor;

	// Layer-2 normal pair: detailmap2 (near) + detailmapdist2 (far), mirror of layer 1.
	// Game init (Jointops sub_60AAA0 LABEL_47) loads both unconditionally and bakes them
	// into one mipmapped texture via sub_687270 (near = mip 0, far = deep mips).
	// Pixel shader does a second dp3 against the blendmap — see ps.1.1 v23 dual-normal
	// (gobj.dll.c:42829-42840): dp3(t1,t2) and dp3(t3,t2) both dot with t2 (blendmap).
	vec3 nmap2_near = texture(u_detailmap2, v_detail_uv).rgb;
	vec3 nmap2_far = texture(u_detailmapdist2, v_detail_uv).rgb;
	vec3 nmap2 = mix(nmap2_near, nmap2_far, fade);
	float normal_factor2 = dot(nmap2, blend) * 2.0;
	lit *= normal_factor2;

	// mul_x4: final = lit * detail * 4
	vec3 result = lit * detail * 4.0;

	// Apply terrain tint
	result *= u_terrain_tint;
	if (u_has_tile_overlay) {
		vec2 tile_uv = fract(vec2(v_world_pos.x, -v_world_pos.z) * (1.0 / 1024.0));
		vec4 tile = texture(u_tile_overlay, tile_uv);
		result = mix(result, tile.rgb * u_tile_overlay_tint, tile.a);
	}

	// Jointops.exe Render_SetFogParams@0x0054B4B0 -> Render_ConfigureFog@0x005F9890.
	{
		float dist = length(v_world_pos - CAMERA_POSITION_WORLD);
		float fog_factor = jointops_fog_factor(dist, u_fog_start, u_fog_end, u_fog_type);
		result = mix(u_fog_color, result, fog_factor);
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

	ALBEDO = clamp(result, vec3(0.0), vec3(1.0));
}
