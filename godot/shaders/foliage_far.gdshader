shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;

// Retail FAR foliage pass. Geometry is the complete replicated source mesh,
// already bent to terrain per vertex by the CPU emitter. Vertex COLOR.r is
// the packed source-height wind weight, never a lighting color.

#include "res://shaders/nova_color.gdshaderinc"

uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
// FAR T1. Retail binds the per-tile detail-lightmap RENDER TARGET (the
// colormap x light x detail-splat composite, world-planar UV)
// [orig: render_terrain_lightmaps @ 0x60a1de (sub_6042A0 lookup) +
// 0x60a220..0x60a356 (the texture transform)]. The host recomposes that
// texel from the same .trn sources the terrain shader uses; the exact
// per-sector render-target feed stays on D-FOLIAGE-7.
uniform sampler2D u_terrain_light_texture : filter_linear, repeat_enable;
uniform sampler2D u_terrain_blend_texture : filter_linear, repeat_enable;
uniform sampler2D u_terrain_detail_c1 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_terrain_detail_c2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_terrain_detail_c3 : filter_linear_mipmap, repeat_enable;
uniform float u_terrain_detail_density = 128.0;
uniform bool u_has_terrain_detail = false;

// Foliage_WindSwayVS c24.x. Retail supplies
// GetTickCount*0.003 + Env_WaveOscRing[0]*1.5258789e-6. The dispatcher binds
// the clock term; NovaWeather publishes the fixed ring-head term below.
uniform float u_far_wind_phase = 0.0;

// Foliage_WindSwayVS c6 RGB — witnessed 2026-07-10 (foliage-re.md §The FAR
// draw constants): on the pixel-shader path c6.rgb = avgDetailColor x (1,1,1),
// and the average detail color is the 128/255 constant on the blendmap tier
// [orig: c6 build @ 0x609efe; avg detail = 128/255 per terrain-re.md; the
// (1,1,1) factor's writer @ 0x604ee0]. 0.50196 exactly, not 0.5.
// Alpha rides per cell in u_cell_fade.
uniform vec4 u_far_pass_color = vec4(0.50196078, 0.50196078, 0.50196078, 1.0);

// Per-cell c6.a distance fade: 1 through distance 20, then 1-(d-20)/22,
// reaching 0 exactly at the 42.0 collect edge
// [orig: render_terrain_lightmaps @ 0x60a45d..0x60a483].
instance uniform float u_cell_fade = 1.0;

// Per-cell high/low pass alpha-test ref: 180 when the cell distance is under
// 33.0, else 8 [orig: render_terrain_lightmaps @ 0x60a171;
// Terrain_SetupSectorModelDraw @ 0x6008f5 / 0x600931].
instance uniform float u_cell_alpha_ref = 180.0;

global uniform vec3 opennova_sky_ambient;
global uniform vec3 opennova_sun_light;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
global uniform float opennova_foliage_wave_addend;

varying vec3 v_world_pos;


float fog_factor_for_distance(float dist) {
	float safe_end = max(opennova_fog_end, 1.0);
	if (opennova_fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}

	float start = opennova_fog_start;
	if (opennova_fog_type == 2) {
		start = safe_end * 0.5;
	} else if (opennova_fog_type == 3) {
		start = safe_end * 0.25;
	}
	float fog_range = max(safe_end - start, 1.0);
	return clamp((safe_end - dist) / fog_range, 0.0, 1.0);
}


float foliage_wind_wave(float phase) {
	// c27 maps the phase to [-pi,pi]. The quarter-turn makes the cosine
	// polynomial reproduce the retail sine-like sway.
	const vec4 phase_map = vec4(0.159155, 6.2831898, -3.1415901, 0.25);
	float wrapped = fract(phase * phase_map.x + phase_map.w) * phase_map.y + phase_map.z;
	float x2 = wrapped * wrapped;

	// c26/c28, evaluated as the retail tenth-order cosine polynomial.
	return 1.0 + x2 * (-0.5 + x2 * (0.041666601 + x2 * (-0.00138884
		+ x2 * (0.0000247609 + x2 * -0.00000025239899))));
}


void vertex() {
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float phase = world_pos.x + (u_far_wind_phase + opennova_foliage_wave_addend);
	float wind_weight = COLOR.r;
	float wave = foliage_wind_wave(phase);

	// Foliage_WindSwayVS displaces render Z only; render Z = -Godot world Z.
	world_pos.z -= wind_weight * wave * 0.03;

	v_world_pos = world_pos;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}


void fragment() {
	vec4 fd = texture(u_fd_texture, UV);
	// World-planar T1 projection in the host terrain row order (1 texel per
	// world unit, +z rows - the same mapping the terrain mesh UVs use; the
	// witnessed native query (x, -z) is this same texel through the native-Z
	// sign boundary [orig: Terrain_GetModulatedColorAtPos @ 0x5C5FE0]).
	vec2 terrain_uv = vec2(v_world_pos.x, v_world_pos.z)
		/ vec2(textureSize(u_terrain_light_texture, 0));
	// LOD 0 throughout the T1 chain: retail's tile render target carries no
	// mip chain, and derivative-driven mips collapse planar projections on
	// the near-vertical foliage surfaces to their texture averages.
	vec4 terrain_light = textureLod(u_terrain_light_texture, terrain_uv, 0.0);

	// The T1 texel stand-in: retail's tile render target already contains
	// the colormap x detail-splat composite, so recompose it here. The
	// ps.1.4 terrain chain's dp3 normal term averages ~0.5; folded with its
	// x2 that leaves splat * 2 (detail-less terrains use the same neutral).
	vec3 splat = vec3(0.5);
	if (u_has_terrain_detail) {
		vec3 blend = textureLod(u_terrain_blend_texture, terrain_uv, 0.0).rgb;
		vec2 detail_uv = terrain_uv * u_terrain_detail_density;
		splat = textureLod(u_terrain_detail_c1, detail_uv, 0.0).rgb * blend.r
			+ textureLod(u_terrain_detail_c2, detail_uv, 0.0).rgb * blend.g
			+ textureLod(u_terrain_detail_c3, detail_uv, 0.0).rgb * blend.b;
	}

	// Foliage_LightmapBlendPS:
	// rgb=t0*(t1*(t1.a*c1+c0))*v0*8, alpha=t0.a*v0.a.
	vec3 lit = (terrain_light.rgb * splat * 2.0)
		* (terrain_light.a * opennova_sun_light + opennova_sky_ambient);
	vec3 result = clamp(fd.rgb * lit * u_far_pass_color.rgb * 8.0,
		vec3(0.0), vec3(1.0));

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	float alpha_threshold = clamp(u_cell_alpha_ref, 8.0, 180.0) / 255.0;
	ALBEDO = nova_gamma_to_linear(result);
	// c6.a fade multiplies the ":fd" alpha before the alpha test - the retail
	// dissolve: as fade drops toward the 42.0 edge, fewer texels clear the ref.
	ALPHA = fd.a * u_far_pass_color.a * u_cell_fade;
	ALPHA_SCISSOR_THRESHOLD = alpha_threshold;
}
