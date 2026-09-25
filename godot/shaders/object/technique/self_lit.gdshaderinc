void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	#ifdef OBJ_CLIP_EXPLICIT
	// TBoringFFPClip has no SELFLUM block: the reflection lights self-lit
	// materials like any other _FFP material.
	if (obj_water_reflection_clip_view(camera_position_world)) {
		obj_evaluate_ffp_clip_surface(camera_position_world, output_lit, output_alpha);
		return;
	}
	#endif
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	output_lit = obj_self_lit(base.rgb, u_rgb_mod);
	// _FFP.fx SELFLUM sets MaterialDiffuse.a = 0; AlphaGenValue is not
	// consumed by this specialization.
	output_alpha = 0.0;
}
