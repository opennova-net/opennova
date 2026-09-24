void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	// vertex_flag reconstructed and camera-oriented the exact Flag.fx normal.
	// v_ff_diffuse carries no point light: the effect has no point-light pass.
	output_lit = obj_ff_lighting(base.rgb);
	output_alpha = base.a;
}
