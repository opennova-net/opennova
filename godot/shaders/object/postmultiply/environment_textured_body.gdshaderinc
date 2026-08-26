// BmTxMirrT.fx TSegTanTexMirror P3. This is an independent depth-tested,
// no-depth-write geometry pass after P0/P1, not a factor inside their shader.
// vscPostMultiplyT1(false,false) uses raw Tex0 and Diffuse=(.5,.5,.5,1);
// fixed TSS MODULATE2X therefore makes the blend source exactly Diffuse1.
// DESTCOLOR/SRCCOLOR then writes 2 * source * destination in gamma space.
// [orig: BmTxMirrT.fx P3; _vsPost.fx vsPostMultiplyT1].

#include "res://shaders/object/shared.gdshaderinc"

uniform sampler2D u_post_screen : hint_screen_texture, filter_nearest, repeat_disable;
uniform sampler2D u_diffuse : filter_linear_mipmap, repeat_enable;
uniform float u_alpha_test_threshold = 0.5;
uniform float u_alpha_test_invert = 0.0;

void vertex() {
	v_raw_uv = UV;
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	if (nova_is_q3_pass(CAMERA_VISIBLE_LAYERS, IN_SHADOW_PASS)) {
		discard;
	}
	vec4 source = texture(u_diffuse, v_raw_uv);
	#ifdef OBJ_POSTMULTIPLY_CUTOUT
	bool accepted = u_alpha_test_invert > 0.5
			? source.a <= u_alpha_test_threshold
			: source.a > u_alpha_test_threshold;
	if (!accepted) {
		discard;
	}
	#endif

	vec3 source_gamma = source.rgb;
	if (u_fog_enabled) {
		float visibility = obj_fog_visibility(
				distance(CAMERA_POSITION_WORLD, v_world_pos),
				u_fog_start, u_fog_end, u_fog_type);
		source_gamma = mix(u_fog_color, source_gamma, visibility);
	}
	vec3 destination_gamma = nova_scene_input(
			textureLod(u_post_screen, SCREEN_UV, 0.0).rgb);
	ALBEDO = nova_scene_output(
			2.0 * source_gamma * destination_gamma);
	ALPHA = 1.0;
}
