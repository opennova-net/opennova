float obj_surface_coverage(vec4 base) {
	float coverage_alpha = base.a;
	#ifdef OBJ_COVERAGE_NORMAL_ALPHA
	coverage_alpha = obj_normal_alpha();
	#endif
	#ifdef OBJ_COVERAGE_ZERO
	coverage_alpha = 0.0;
	#endif
	#ifdef OBJ_COVERAGE_REFLECT_ALPHA
	coverage_alpha = u_reflect_color.a;
	#endif
	return coverage_alpha;
}

void obj_prepare_surface(
	vec3 camera_position_world,
	mat4 model_matrix,
	out vec4 base,
	out vec3 view_dir,
	out vec3 surface_normal,
	out vec3 geom_normal
) {
	obj_apply_water_reflection_clip(camera_position_world);
	base = obj_sample_base();
	float coverage_alpha = obj_surface_coverage(base);
	obj_apply_coverage(coverage_alpha);
	view_dir = normalize(camera_position_world - v_world_pos);
	geom_normal = normalize(v_geom_normal);
	surface_normal = obj_resolve_surface_normal(model_matrix, geom_normal);
}
