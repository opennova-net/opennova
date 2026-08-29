void fragment() {
	if (obj_is_slot_shadow_capture(CAMERA_VISIBLE_LAYERS,
			IN_SHADOW_PASS)) {
		#ifdef OBJ_PROJSHAD_NO_PASS
		discard;
		#else
		float coverage = obj_proj_shadow_coverage();
		obj_apply_coverage(coverage);
		// Retail PROJSHAD draws black over the slot RT's white clear. The
		// witnessed coverage policy remains active; lighting and fog do not
		// participate in this pass (vsPostBlackT1 / vsSkinPostBlackT1).
		ALBEDO = vec3(0.0);
		#endif
	} else {
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
}
