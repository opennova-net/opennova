uniform sampler2D u_diffuse : source_color, filter_linear_mipmap, repeat_enable;
uniform vec3 u_rgb_mod = vec3(1.0);
uniform float u_alpha_mod = 1.0;

vec4 obj_sample_base() {
	vec4 base = texture(u_diffuse, v_uv);
	base.rgb *= u_rgb_mod;
	base.a *= u_alpha_mod;
	return base;
}
