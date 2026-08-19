void obj_prepare_surface(
	vec3 camera_position_world,
	mat4 model_matrix,
	out vec4 base,
	out vec3 view_dir,
	out vec3 surface_normal,
	out vec3 geom_normal
) {
	base = obj_sample_base();
	obj_apply_coverage(base.a);
	view_dir = normalize(camera_position_world - v_world_pos);
	geom_normal = normalize(v_geom_normal);
	surface_normal = obj_resolve_surface_normal(model_matrix, geom_normal);
}
