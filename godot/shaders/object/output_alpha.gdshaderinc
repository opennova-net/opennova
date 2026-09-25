void fragment() {
#ifdef OBJ_BLENDED_PASS
	if (u_viewmodel_pass && viewmodel_world_covers(SCREEN_UV)) {
		discard;
	}
#endif
	vec3 lit;
	// A person's blended strip behind the foliage depth masks fails its
	// depth test and leaves the destination as it was. No technique that
	// blends carries a MATCHTERRAIN pass (shaders/object/pipeline_manifest.json:
	// every skinned_stance match_terrain contract belongs to an opaque or
	// alpha-tested technique, and the blended wrappers exist only for the
	// no_pass Fixed, FixedDetail, SelfLit, SelfLitDetail and Glass families),
	// so no pre-pass colour lies under it (obj_foliage_mask_outcome).
	if (obj_foliage_mask_outcome(FRAGCOORD, CAMERA_POSITION_WORLD) != 0) {
		discard;
	}
	if (obj_match_terrain_replaces_normal(lit)) {
		ALBEDO = scene_output(
				obj_apply_policy_fog(lit, CAMERA_POSITION_WORLD));
		ALPHA = 1.0;
	} else {
		float alpha;
		obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX, lit, alpha);
		vec3 fogged = obj_apply_policy_fog(lit, CAMERA_POSITION_WORLD);
		ALBEDO = scene_output(fogged);
		ALPHA = clamp(alpha, 0.0, 1.0);
	}
}
