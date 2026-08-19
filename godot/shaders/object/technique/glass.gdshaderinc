uniform vec4 u_reflect_color = vec4(0.7, 0.8, 0.9, 0.35);

// Glass NORMAL technique with the D-RLIT-5 hemisphere scene-cube stand-in.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 ff_lit = obj_ff_lighting(base.rgb, normal);
	vec3 environment = obj_hemi(reflect(-view_dir, normal)) * 2.0;
	float fresnel = pow(1.0 - max(dot(normal, view_dir), 0.0), 2.0);
	float mix_amount = clamp(
		u_reflect_color.a + fresnel * (1.0 - u_reflect_color.a), 0.0, 1.0);
	vec3 lit = mix(ff_lit, environment * u_reflect_color.rgb, mix_amount);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = obj_apply_fog(lit, camera_position_world);
	output_alpha = clamp(max(base.a, 0.35), 0.0, 1.0);
}
