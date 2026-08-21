uniform vec4 u_reflect_color = vec4(0.7, 0.8, 0.9, 0.35);

void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_albedo, out vec3 output_normal,
		out vec3 output_emission, out float output_roughness,
		out float output_specular, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	output_albedo = mix(base.rgb, u_reflect_color.rgb,
			clamp(u_reflect_color.a, 0.0, 1.0));
	output_normal = normal;
	output_emission = vec3(0.0);
	output_roughness = 0.1;
	output_specular = 0.85;
	output_alpha = clamp(max(base.a, 0.35), 0.0, 1.0);
}
