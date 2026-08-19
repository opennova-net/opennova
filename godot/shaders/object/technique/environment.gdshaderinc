// D-RLIT-5 hemisphere stand-in for the unhosted scene cubemap.
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 lit = obj_ff_lighting(base.rgb, normal);
	float fresnel = pow(1.0 - max(dot(normal, view_dir), 0.0), 3.0);
	lit = mix(lit, obj_hemi(reflect(-view_dir, normal)) * 2.0, fresnel * 0.35);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = obj_apply_fog(lit, camera_position_world);
	output_alpha = base.a;
}
