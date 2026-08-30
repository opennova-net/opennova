void fragment() {
	vec3 lit;
	if (obj_match_terrain_replaces_normal(lit)) {
		ALBEDO = scene_output(
				obj_apply_policy_fog(lit, CAMERA_POSITION_WORLD));
	} else {
		float alpha;
		obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX, lit, alpha);
		vec3 fogged = obj_apply_policy_fog(lit, CAMERA_POSITION_WORLD);
		ALBEDO = scene_output(fogged);
	}
}
