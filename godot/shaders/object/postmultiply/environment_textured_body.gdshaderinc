// BmTxMirrT.fx TSegTanTexMirror P3. This is an independent depth-tested,
// no-depth-write geometry pass after P0/P1, not a factor inside their shader.
// vscPostMultiplyT1(false,false) uses raw Tex0 and Diffuse=(.5,.5,.5,1);
// fixed TSS MODULATE2X therefore makes the blend source exactly Diffuse1.
// DESTCOLOR/SRCCOLOR then writes 2 * source * destination — now over the
// linear scene copy (ADR 0043 linear-scene amendment).
// [orig: BmTxMirrT.fx P3; _vsPost.fx vsPostMultiplyT1].

#include "res://shaders/object/shared.gdshaderinc"

uniform sampler2D u_post_screen : hint_screen_texture, filter_nearest, repeat_disable;
uniform sampler2D u_diffuse : source_color, filter_linear_mipmap, repeat_enable;
uniform float u_alpha_test_threshold = 0.5;
uniform float u_alpha_test_invert = 0.0;

void vertex() {
	v_raw_uv = UV;
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// The P3 mirror layer rides the same first-person fold as the P0/P1 body
	// it overdraws (vertex_standard.gdshaderinc): an un-folded layer on the
	// viewmodel would take the beauty projection and near plane while the gun
	// under it is folded, and land displaced or clipped (the ACOG lens).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}

void fragment() {
	vec4 source = texture(u_diffuse, v_raw_uv);
	#ifdef OBJ_POSTMULTIPLY_CUTOUT
	bool accepted = u_alpha_test_invert > 0.5
			? source.a <= u_alpha_test_threshold
			: source.a > u_alpha_test_threshold;
	if (!accepted) {
		discard;
	}
	#endif

	vec3 source_lin = source.rgb;
	if (opennova_fog_enabled) {
		source_lin = mix(opennova_fog_color, source_lin,
				obj_scene_fog_visibility(CAMERA_POSITION_WORLD));
	}
	vec3 destination_lin = textureLod(u_post_screen, SCREEN_UV, 0.0).rgb;
	ALBEDO = clamp(2.0 * source_lin * destination_lin, vec3(0.0), vec3(1.0));
	ALPHA = 1.0;
}
