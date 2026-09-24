uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;
uniform float u_normal_max_lod = 1000.0;

float obj_normal_alpha() {
	return obj_sample_aniso2(u_normal_map, v_uv, u_normal_max_lod).a;
}

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = obj_sample_aniso2(u_normal_map, v_uv, u_normal_max_lod).xyz * 2.0 - 1.0;
	sampled = normalize(vec3(-sampled.x, sampled.y, sampled.z));
	return normalize((model_matrix * vec4(sampled, 0.0)).xyz);
}
