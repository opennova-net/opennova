uniform sampler2D u_diffuse : filter_linear_mipmap, repeat_enable;
uniform vec3 u_rgb_mod = vec3(1.0);
uniform float u_alpha_mod = 1.0;

vec4 obj_sample_base() {
	vec4 base = texture(u_diffuse, v_uv);
	#ifdef OBJ_RGB_MOD_SELF_LUM
	base.rgb *= u_rgb_mod;
	#endif
	#ifdef OBJ_ALPHA_MOD_FFP
	base.a *= u_alpha_mod;
	#endif
	return base;
}
