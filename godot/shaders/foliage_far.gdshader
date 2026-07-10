shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;

// Retail FAR foliage pass. Geometry is the complete replicated source mesh,
// already bent to terrain per vertex by the CPU emitter. Vertex COLOR.r is
// the packed source-height wind weight, never a lighting color.

#include "res://shaders/nova_color.gdshaderinc"

uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_terrain_light_texture : filter_linear, repeat_enable;

// Foliage_WindSwayVS c24.x. Retail supplies
// GetTickCount*0.003 + Env_WaveOscRing[0]*1.5258789e-6. The dispatcher binds
// the clock term; NovaWeather publishes the fixed ring-head term below.
uniform float u_far_wind_phase = 0.0;

// GridPlacementVS/Foliage_WindSwayVS c6: retail terrain-light RGB factors and
// per-sector fade alpha. The alpha is 1 through distance 20, then falls by
// 1/22 per unit; the alternate path multiplies it by 0.1.
uniform vec4 u_far_pass_color = vec4(1.0);

// Foliage high/low pass objects use 180 and 8 respectively.
uniform float u_far_alpha_ref : hint_range(8.0, 180.0, 1.0) = 8.0;

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
	vec2 terrain_uv = vec2(v_world_pos.x, -v_world_pos.z)
		/ vec2(textureSize(u_terrain_light_texture, 0));
	vec4 terrain_light = texture(u_terrain_light_texture, terrain_uv);

	// Foliage_LightmapBlendPS:
	// rgb=t0*(t1*(t1.a*c1+c0))*v0*8, alpha=t0.a*v0.a.
	vec3 lit = terrain_light.rgb
		* (terrain_light.a * opennova_sun_light + opennova_sky_ambient);
	vec3 result = clamp(fd.rgb * lit * u_far_pass_color.rgb * 8.0,
		vec3(0.0), vec3(1.0));

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	float alpha_threshold = clamp(u_far_alpha_ref, 8.0, 180.0) / 255.0;
	ALBEDO = nova_gamma_to_linear(result);
	ALPHA = fd.a * u_far_pass_color.a;
	ALPHA_SCISSOR_THRESHOLD = alpha_threshold;
}
