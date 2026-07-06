// Shared NovaLogic terrain lighting + fog math, included by both terrain.gdshader
// (runtime, baked-CPT mesh) and terrain_editor.gdshader (editor live-sculpt
// heightmap mesh). Both render the SAME surface; only their geometry source and
// overlays differ, so the surface-shading and fog math live here once.
//
// The surface chain is the witnessed TOP-TIER ps.1.4 splat shader
// [orig: PolyTrn_PS14SplatNormalMap source text @ 0x7dece0, assembled by
// compile_terrain_pixel_shaders @ 0x605260]:
//   splat = c1*blend.r + c2*blend.g + c3*blend.b        (3-way detail splat)
//   dp3   = dot(normalmap(t3), blendmap)
//   light = (cm.a*c1 + c0) / 2                          (c1 = LIGHT, c0 = SKY)
//   out   = light *2 cm *2 dp3 *4 splat
// EXACTLY ONE normal term: the dual-normal product belonged to the separate
// ps.1.1 non-splat tier (PolyTrn_PSDualNormalMap) and was wrongly STACKED on
// the splat here for years (the gobj-era "v23 dual-normal" chimera) — under
// the gamma-faithful pipeline (D-RMAT-7) that squared bump contrast and
// clipped whole regions to white. Corrected 2026-07-06 (D-TRN row in the
// ledger). Fog: [orig: Render_SetFogState @ 0x58a950 ->
// CD3DDevice_SetFogParameters @ 0x677960]. Keep byte-for-byte authoritative.

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

// NovaLogic terrain surface shading — the witnessed ps.1.4 splat chain
// [orig: PolyTrn_PS14SplatNormalMap @ 0x7dece0]. No fog/overlays — callers
// layer those on top. The colormap/detail UVs and world/camera positions
// come from the caller's geometry (runtime: baked patch mesh; editor:
// sculpted heightmap mesh).
vec3 terrain_surface_color(vec2 colormap_uv, vec2 detail_uv, vec3 world_pos, vec3 camera_pos) {
	vec4 cm = texture(u_colormap, colormap_uv);
	vec3 blend = texture(u_blendmap, colormap_uv).rgb;
	vec3 c1 = texture(u_detail_c1, detail_uv).rgb;
	vec3 c2 = texture(u_detail_c2, detail_uv).rgb;
	vec3 c3 = texture(u_detail_c3, detail_uv).rgb;

	// mul/mad: splat = c1*R + c2*G + c3*B [orig: @ 0x7dece0 rows 1-3]
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// The single witnessed normal term: dp3(normalmap, blendmap) * 2
	// [orig: @ 0x7dece0 `dp3 r3, r3, r2` + the mul_x2]. The witnessed t3 is
	// the heightmap-derived normal map (Texture_GenerateNormalMap, scale
	// 1/32 [orig: PolyTrn_InitTextures @ 0x60aaa0]); the host samples the
	// .trn's near/far detail-normal pair with the distance crossfade as the
	// texture-source stand-in until that generator ports (tracked in the
	// D-TRN ledger row). The detailmap2/dist2 pair stays authored (the .trn
	// owns the slots; retail's ps.1.1 dual-normal tier consumes them) but no
	// longer multiplies into this tier — the witnessed shader applies
	// EXACTLY ONE normal term.
	float cam_dist = length(world_pos - camera_pos);
	float detail_range = max(u_detail_far_dist - u_detail_near_dist, 0.0001);
	float fade = clamp((cam_dist - u_detail_near_dist) / detail_range, 0.0, 1.0);
	vec3 nmap_near = texture(u_detailmap, detail_uv).rgb;
	vec3 nmap_far = texture(u_detailmapdist, detail_uv).rgb;
	vec3 nmap = mix(nmap_near, nmap_far, fade);
	float normal_factor = dot(nmap, blend) * 2.0;

	// mad_d2: light = (cm.a * c1 + c0) / 2 with the WITNESSED constants
	// c1 = the LIGHT block, c0 = the SKY block (both /255): colormap alpha is
	// the baked per-texel sun mask blending the sun over the sky ambience.
	// [orig: terrain_setup_lighting_and_shader @ 0x604420 pushes
	//  c0 <- PolyTrn_PSConstC0_Sky @ 0x31a183c, c1 <- PolyTrn_PSConstC1_Light
	//  @ 0x31a182c; values from init_terrain_lighting_color_ramps @ 0x604ee0
	//  <- Render_TerrainScene @ 0x610c80 (Env_LightBlock / Env_SkyBlock)]
	vec3 ambient = (cm.a * u_sun_light + u_sky_ambient) * 0.5;

	// mul_x2 twice, then mul_x4 [orig: @ 0x7dece0 tail]:
	// out = light *2 colormap *2 dp3 *4 splat (+ the terrain tint, env #19).
	vec3 lit = ambient * cm.rgb * 2.0;
	lit *= normal_factor;
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
