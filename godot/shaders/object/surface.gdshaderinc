float obj_surface_coverage(vec4 base) {
	float coverage_alpha = base.a;
	#ifdef OBJ_COVERAGE_VERTEX_DIFFUSE_ALPHA
	coverage_alpha = v_dir_self_shadow;
	#endif
	#ifdef OBJ_COVERAGE_NORMAL_ALPHA
	coverage_alpha = obj_normal_alpha();
	#endif
	#ifdef OBJ_COVERAGE_ZERO
	coverage_alpha = 0.0;
	#endif
	#ifdef OBJ_COVERAGE_REFLECT_ALPHA
	coverage_alpha = u_reflect_color.a;
	#endif
	return coverage_alpha;
}

void obj_prepare_surface(
	vec3 camera_position_world,
	mat4 model_matrix,
	out vec4 base,
	out vec3 view_dir,
	out vec3 surface_normal,
	out vec3 geom_normal
) {
	obj_apply_water_reflection_clip(camera_position_world);
	base = obj_sample_base();
	float coverage_alpha = obj_surface_coverage(base);
	obj_apply_coverage(coverage_alpha);
	view_dir = normalize(camera_position_world - v_world_pos);
	geom_normal = normalize(v_geom_normal);
	surface_normal = obj_resolve_surface_normal(model_matrix, geom_normal);
}

// _FFP.fx TBoringFFPClip, drawn instead of NORMAL in the armed reflection
// view: stage 0 is Diffuse1 alone (the clip texture takes stage 1, so the
// _MT detail stage is gone) under MODULATE2X of the fixed-function diffuse
// without point lights (the technique carries no useffplights annotation)
// and without a SELFLUM block; alpha is Diffuse1.a x AlphaGenValue x the clip
// stage, tested by AMODE_CLIP's GREATER 128 in place of the material's own
// test. obj_apply_water_reflection_clip is the clip stage.
// retail CRenderBatchQueue_FlushBatches @ 0x5DA37F..0x5DA399 (AMODE_CLIP),
// @ 0x5DA5D4..0x5DA5DC (FF point lights only for useffplights techniques).
void obj_evaluate_ffp_clip_surface(vec3 camera_position_world,
		out vec3 output_lit, out float output_alpha) {
	obj_apply_water_reflection_clip(camera_position_world);
	vec4 base = obj_sample_aniso2(u_diffuse, v_uv);
	float alpha = base.a * u_alpha_mod;
	if (!(alpha > 128.0 / 255.0)) {
		discard;
	}
	output_lit = base.rgb * v_ff_clip_diffuse * 2.0;
	output_alpha = alpha;
}
