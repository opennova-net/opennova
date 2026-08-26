// BumpMirrT.fx TSegTanMirror: reflection tinted by ReflectColor, plus one
// additive pow-16 specular pass per point light. There is no diffuse/Fresnel
// term in the retail NORMAL technique.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 reflection = reflect(-view_dir, normal);
	vec3 lit = obj_environment_cube_approx(reflection) * u_reflect_color.rgb;
	lit += obj_mirror_points(normal, view_dir);
	output_lit = lit;
	output_alpha = base.a;
}
