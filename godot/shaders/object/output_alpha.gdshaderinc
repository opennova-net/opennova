void fragment() {
#ifdef OBJ_BLENDED_PASS
	if (u_viewmodel_pass && viewmodel_world_covers(SCREEN_UV)) {
		discard;
	}
#endif
	vec3 lit;
	// A person's blended strip behind the foliage depth masks fails its
	// depth test like the opaque output does (obj_foliage_mask_outcome).
	int foliage_mask = obj_foliage_mask_outcome(FRAGCOORD, CAMERA_POSITION_WORLD);
	if (foliage_mask == 2 || (foliage_mask == 1 && !obj_match_terrain_active())) {
		discard;
	}
	if (foliage_mask == 1) {
		ALBEDO = scene_output(
				obj_apply_policy_fog(obj_match_terrain_color(), CAMERA_POSITION_WORLD));
		ALPHA = 1.0;
	} else if (obj_match_terrain_replaces_normal(lit)) {
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
