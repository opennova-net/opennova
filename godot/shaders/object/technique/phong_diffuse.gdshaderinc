void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 lit = obj_bump_diffuse_lighting(base.rgb, normal, geom_normal);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = lit;
	output_alpha = base.a;
}
