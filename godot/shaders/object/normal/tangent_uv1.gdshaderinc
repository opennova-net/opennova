uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = texture(u_normal_map, v_uv).xyz * 2.0 - 1.0;
	return normalize(
		sampled.x * normalize(v_world_tangent) +
		sampled.y * normalize(v_world_binormal) +
		sampled.z * geom_normal
	);
}
