uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = texture(u_normal_map, v_uv).xyz * 2.0 - 1.0;
	if (dot(sampled, sampled) < 0.000001) {
		return geom_normal;
	}
	sampled = normalize(vec3(-sampled.x, sampled.y, sampled.z));
	vec3 world_normal = (model_matrix * vec4(sampled, 0.0)).xyz;
	return dot(world_normal, world_normal) < 0.000001
			? geom_normal
			: normalize(world_normal);
}
