uniform sampler2D u_diffuse : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail : source_color, filter_linear_mipmap, repeat_enable;
uniform vec3 u_rgb_mod : source_color = vec3(1.0);
uniform float u_alpha_mod = 1.0;

vec4 obj_sample_base() {
	vec4 base = texture(u_diffuse, v_uv);
	// _MT stage 1: MODULATE2X color and MODULATE alpha over UV2.
	vec4 detail = texture(u_detail, v_uv2);
	base.rgb *= detail.rgb * 2.0;
	base.a *= detail.a;
	#ifdef OBJ_RGB_MOD_SELF_LUM
	base.rgb *= u_rgb_mod;
	#endif
	#ifdef OBJ_ALPHA_MOD_FFP
	base.a *= u_alpha_mod;
	#endif
	return base;
}
