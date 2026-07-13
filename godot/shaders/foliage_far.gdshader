shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;

// Retail FAR foliage pass. Geometry is the complete replicated source mesh,
// already bent to terrain per vertex by the CPU emitter. Vertex COLOR.r is
// the packed source-height wind weight; the host uses otherwise-free COLOR.gba
// to carry the underlying terrain normal for the patch-cache N.L fold.

#include "res://shaders/nova_color.gdshaderinc"

uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
// FAR T1. Retail binds the terrain patch-cache RENDER TARGET (world-planar
// UV), not a `.til` overlay list [orig: Foliage_RenderFarPatches @ 0x60a1de
// (Terrain_FindSectorPatchRT lookup) + 0x60a220..0x60a356 (the texture
// transform)]. The base bake was witnessed 2026-07-10: its quad draws the
// colormap quadrant with MODULATE2X(TEXTURE, DIFFUSE=0x808080) - rgb ~= the
// colormap x 1.004 - and diffuse ALPHA byte 0 starts RT alpha at ZERO; the
// additive ONE/ONE DOT3 pass then adds dp3(TrnNMap, packed-light-dir) to
// ALPHA only (its stage 1 selects TFACTOR = 0 for RGB on the multitexture
// path). Optional overlay/decal/scorch draws can composite additional RGB;
// base-only RT rgb ~= colormap.rgb (NO detail splat, NO noise), while alpha
// receives saturate(N.L) with the CURRENT light direction
// [orig: PolyTrn_RenderTile @ 0x60dce5 / 0x60e38a; PolyTrn_TileBakeBasePass
// mode 0x20650; PolyTrn_TileBakeDot3LightPass]. The host binds a colormap-only
// RGB stand-in, omitting optional composite RGB, and reconstructs current-light
// N.L from the central-difference heightfield normal carried in COLOR.gba.
// Retail's exact per-texel normal-map sampling remains D-FOLIAGE-7.
uniform sampler2D u_terrain_light_texture : filter_linear, repeat_enable;

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
global uniform vec3 opennova_sun_direction;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
global uniform float opennova_foliage_wave_addend;

varying vec3 v_world_pos;
varying vec3 v_ground_normal;


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
	// COLOR.r is the packed wind weight. COLOR.gba carries the terrain normal
	// sampled under this transformed vertex so a 24-bit colormap never turns
	// the retail patch-cache N.L input into an implicit 1.0.
	v_ground_normal = normalize(COLOR.gba * 2.0 - 1.0);
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
	// LOD 0: retail's tile render target carries no mip chain, and
	// derivative-driven mips collapse planar projections on the
	// near-vertical foliage surfaces to their texture averages.
	vec4 terrain_light = textureLod(u_terrain_light_texture, terrain_uv, 0.0);

	// Foliage_LightmapBlendPS:
	// rgb=t0*(t1*(t1.a*c1+c0))*v0*8, alpha=t0.a*v0.a.
	// t1.rgb is the host's colormap-only terrain patch-cache RT stand-in (see
	// the T1 uniform note): retail foliage carries NO detail-splat term - c6
	// = the average detail color
	// (128/255 on the blendmap tier) supplies the ground chain's
	// 4*splat x 2*dp3 AVERAGE instead, so tufts sit at the brightness of the
	// ground they grow from [orig: Foliage_LightmapBlendPS @ 0x5ff7a0;
	// avg-detail writer @ PolyTrn_InitTextures 0x60ab00].
	vec3 ground_normal = normalize(v_ground_normal);
	float terrain_n_dot_l = clamp(dot(ground_normal, opennova_sun_direction), 0.0, 1.0);
	vec3 lit = terrain_light.rgb
		* (terrain_n_dot_l * opennova_sun_light + opennova_sky_ambient);
	vec3 result = clamp(fd.rgb * lit * u_far_pass_color.rgb * 8.0,
		vec3(0.0), vec3(1.0));

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	float alpha_threshold = clamp(u_cell_alpha_ref, 8.0, 180.0) / 255.0;
	float far_alpha = fd.a * u_far_pass_color.a * u_cell_fade;
	// Retail applies D3DCMP_GREATER: equality with the byte reference fails.
	if (far_alpha <= alpha_threshold) {
		discard;
	}
	ALBEDO = nova_gamma_to_linear(result);
	// c6.a fade multiplies the ":fd" alpha before the alpha test - the retail
	// dissolve: as fade drops toward the 42.0 edge, fewer texels clear the ref.
	// Do not write Godot ALPHA here. Retail enables binary alpha test, not alpha
	// blending; surviving fragments are opaque cutouts. Writing ALPHA would mix
	// them with bright terrain/sky and wash the foliage toward the background.
}
