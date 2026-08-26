uniform sampler2D u_diffuse : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail : filter_linear_mipmap, repeat_enable;
uniform vec3 u_rgb_mod = vec3(1.0);
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

float obj_proj_shadow_coverage() {
	#ifdef OBJ_PROJSHAD_NO_PASS
	return 0.0;
	#else
	float coverage = texture(u_diffuse, v_uv).a;
	#ifdef OBJ_PROJSHAD_DIFFUSE_DETAIL_ALPHA_FFP
	coverage *= texture(u_detail, v_uv2).a;
	coverage *= u_alpha_mod;
	#endif
	#ifdef OBJ_PROJSHAD_DIFFUSE_ALPHA_FFP
	coverage *= u_alpha_mod;
	#endif
	return coverage;
	#endif
}
