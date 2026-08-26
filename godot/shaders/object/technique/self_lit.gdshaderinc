void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	output_lit = obj_self_lit(base.rgb);
	// _FFP.fx SELFLUM sets MaterialDiffuse.a = 0; AlphaGenValue is not
	// consumed by this specialization.
	output_alpha = 0.0;
}
