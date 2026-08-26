float obj_normal_alpha() {
	return 1.0;
}

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	return normalize(v_world_normal);
}
