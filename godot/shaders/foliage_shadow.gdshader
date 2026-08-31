shader_type spatial;
render_mode unshaded, cull_disabled;

// The detail tier's opaque shadow variant (ADR 0043; retail had no foliage
// shadows at all). Godot's shadow pass skips the transparent-class detail
// materials, so each near-detail draw carries a SHADOWS_ONLY twin running
// this alpha-scissor shader: the same wind-displaced vertices and the same
// alpha-reference/high-pass discard selection, with no color contribution.

#include "res://shaders/foliage_fd_sampling.gdshaderinc"

uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_fd_texture = false;
instance uniform float u_wind_phase = 0.0;
instance uniform float u_fade = 1.0;
instance uniform float u_alpha_ref = 0.0;
instance uniform float u_high_pass_cutoff = 0.0;

void vertex() {
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// The retail wind sway [orig: Foliage_WindSwayVS source @ 0x5ff691] —
	// the shadow silhouette follows the visible card exactly.
	world_pos.z += sin(world_pos.x + u_wind_phase) * COLOR.r * 0.03;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	vec4 fd = u_has_fd_texture
		? sample_retail_foliage_fd(u_fd_texture, UV)
		: vec4(0.5, 0.5, 0.5, 1.0);
	float retail_alpha = fd.a * u_fade;
	if (retail_alpha <= u_alpha_ref) {
		discard;
	}
	if (u_high_pass_cutoff > 0.0 && retail_alpha > u_high_pass_cutoff) {
		discard;
	}
	ALBEDO = vec3(0.0);
}
