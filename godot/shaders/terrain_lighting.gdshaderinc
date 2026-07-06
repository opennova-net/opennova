// Shared NovaLogic terrain lighting + fog math, included by both terrain.gdshader
// (runtime, baked-CPT mesh) and terrain_editor.gdshader (editor live-sculpt
// heightmap mesh). Both render the SAME surface; only their geometry source and
// overlays differ, so the surface-shading and fog math live here once.
//
// This is the IDA-derived ps.1.4 dual-normal path (Jointops sub_60AAA0 /
// gobj ps.1.1 v23 dual-normal, gobj.dll.c:42829-42840) plus the fog model from
// [orig: Render_SetFogState @ 0x58a950 -> CD3DDevice_SetFogParameters @ 0x677960].
// Keep it byte-for-byte authoritative.

// --- Shared uniforms: terrain surface inputs + lighting + fog -----------------
// Textures sample RAW (no source_color): retail never enables sRGB sampling -
// texel bytes enter the ps.1.4 math as-is. The gamma-space convention
// (D-RMAT-7) lives in nova_color.gdshaderinc; consumers encode the final
// color once at their ALBEDO write.
uniform sampler2D u_colormap : filter_linear, repeat_disable;
uniform sampler2D u_detail_c1 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c3 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_blendmap : filter_linear, repeat_disable;
uniform sampler2D u_detailmap : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmap2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detailmapdist2 : filter_linear_mipmap, repeat_enable;
uniform float u_detail_near_dist = 40.0;
uniform float u_detail_far_dist = 80.0;
// Un-enved defaults = the retail noon register (full_00.env tod 1200:
// sun_rgb 170,170,167; sky_rgb 84,88,89 — bytes /255), one register with the
// object-model/global defaults.
uniform vec3 u_sun_light = vec3(0.666667, 0.666667, 0.654902);
uniform vec3 u_sky_ambient = vec3(0.329412, 0.345098, 0.349020);
uniform vec3 u_sun_direction = vec3(0.0, 0.70710678, 0.70710678);
uniform vec3 u_terrain_tint = vec3(1.0, 1.0, 1.0);
uniform float u_detail_density = 128.0;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

// NovaLogic terrain surface shading: dual detail-normal lit color * detail, with
// the terrain tint applied. No fog/overlays — callers layer those on top. The
// colormap/detail UVs and world/camera positions come from the caller's geometry
// (runtime: baked patch mesh; editor: sculpted heightmap mesh).
vec3 terrain_surface_color(vec2 colormap_uv, vec2 detail_uv, vec3 world_pos, vec3 camera_pos) {
	vec4 cm = texture(u_colormap, colormap_uv);
	vec3 blend = texture(u_blendmap, colormap_uv).rgb;
	vec3 c1 = texture(u_detail_c1, detail_uv).rgb;
	vec3 c2 = texture(u_detail_c2, detail_uv).rgb;
	vec3 c3 = texture(u_detail_c3, detail_uv).rgb;

	// mul/mad: detail = c1*R + c2*G + c3*B
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// Distance-based detail normal crossfade (NovaLogic ps.1.4 near/far t1 selection).
	// Thresholds are world-unit ports of the original 2,621,440 / 5,242,880 (16.16 fixed-point).
	float cam_dist = length(world_pos - camera_pos);
	float detail_range = max(u_detail_far_dist - u_detail_near_dist, 0.0001);
	float fade = clamp((cam_dist - u_detail_near_dist) / detail_range, 0.0, 1.0);
	vec3 nmap_near = texture(u_detailmap, detail_uv).rgb;
	vec3 nmap_far = texture(u_detailmapdist, detail_uv).rgb;
	vec3 nmap = mix(nmap_near, nmap_far, fade);

	// dp3: dot(normalmap, blendweights) -> per-material normal response
	float normal_factor = dot(nmap, blend) * 2.0;

	// mad_d2: lit = (alpha * c1 + c0) / 2 with the WITNESSED retail constants
	// c1 = the LIGHT block, c0 = the SKY block (both /255): colormap alpha is
	// the baked per-texel sun mask blending the sun over the sky ambience —
	// the earlier combinedSun/fillLight pairing was a gobj-era stand-in.
	// [orig: terrain_setup_lighting_and_shader @ 0x604420 pushes
	//  c0 <- PolyTrn_PSConstC0_Sky @ 0x31a183c, c1 <- PolyTrn_PSConstC1_Light
	//  @ 0x31a182c; values from init_terrain_lighting_color_ramps @ 0x604ee0
	//  <- Render_TerrainScene @ 0x610c80 (Env_LightBlock / Env_SkyBlock)]
	vec3 ambient = (cm.a * u_sun_light + u_sky_ambient) * 0.5;

	// mul_x2: lit = ambient * colormap * 2
	vec3 lit = ambient * cm.rgb * 2.0;
	// mul_x2: lit *= normalFactor
	lit *= normal_factor;

	// Layer-2 normal pair: detailmap2 (near) + detailmapdist2 (far), mirror of layer 1.
	// Game init (Jointops sub_60AAA0 LABEL_47) loads both unconditionally and bakes them
	// into one mipmapped texture via sub_687270 (near = mip 0, far = deep mips).
	// Pixel shader does a second dp3 against the blendmap — see ps.1.1 v23 dual-normal
	// (gobj.dll.c:42829-42840): dp3(t1,t2) and dp3(t3,t2) both dot with t2 (blendmap).
	vec3 nmap2_near = texture(u_detailmap2, detail_uv).rgb;
	vec3 nmap2_far = texture(u_detailmapdist2, detail_uv).rgb;
	vec3 nmap2 = mix(nmap2_near, nmap2_far, fade);
	float normal_factor2 = dot(nmap2, blend) * 2.0;
	lit *= normal_factor2;

	// mul_x4: final = lit * detail * 4, then terrain tint
	return lit * detail * 4.0 * u_terrain_tint;
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

// Mix the configured fog color over `color` by world-space camera distance.
// [orig: Render_SetFogState @ 0x58a950 -> CD3DDevice_SetFogParameters @ 0x677960]
vec3 apply_terrain_fog(vec3 color, vec3 world_pos, vec3 camera_pos) {
	float dist = length(world_pos - camera_pos);
	float fog_factor = jointops_fog_factor(dist, u_fog_start, u_fog_end, u_fog_type);
	return mix(u_fog_color, color, fog_factor);
}
