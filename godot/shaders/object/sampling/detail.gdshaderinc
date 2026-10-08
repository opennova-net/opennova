uniform sampler2D u_diffuse : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail : filter_linear_mipmap, repeat_enable;
uniform float u_diffuse_max_lod = 1000.0;
uniform float u_detail_max_lod = 1000.0;
uniform vec3 u_rgb_mod = vec3(1.0);
uniform float u_alpha_mod = 1.0;

vec4 obj_sample_base() {
	vec4 base = obj_sample_effect_stage(u_diffuse, v_uv, u_diffuse_max_lod);
	// _MT stage 1: MODULATE2X color and MODULATE alpha over UV2.
	vec4 detail = obj_sample_effect_stage(u_detail, v_uv2, u_detail_max_lod);
	base.rgb *= detail.rgb * 2.0;
	base.a *= detail.a;
	#ifdef OBJ_ALPHA_MOD_FFP
	base.a *= u_alpha_mod;
	#endif
	return base;
}
