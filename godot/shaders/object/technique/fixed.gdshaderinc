void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	#ifdef OBJ_CLIP_EXPLICIT
	if (obj_water_reflection_clip_view(camera_position_world)) {
		obj_evaluate_ffp_clip_surface(camera_position_world, output_lit, output_alpha);
		return;
	}
	#endif
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	vec3 lit = obj_ff_lighting(base.rgb);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = lit;
	output_alpha = base.a;
}
