void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_albedo, out vec3 output_normal,
		out vec3 output_emission, out float output_roughness,
		out float output_specular, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	output_albedo = vec3(0.0);
	output_normal = dot(view_dir, geom_normal) < 0.0 ? -normal : normal;
	output_emission = base.rgb * 2.0;
	output_roughness = 1.0;
	output_specular = 0.0;
	output_alpha = base.a;
}
