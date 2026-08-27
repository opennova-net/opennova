void fragment() {
	if (is_q3_pass(CAMERA_VISIBLE_LAYERS, IN_SHADOW_PASS)) {
		#ifdef OBJ_GLOW_NO_PASS
			#ifdef OBJ_Q3_DEPTH_OCCLUDER
			obj_apply_q3_coverage(CAMERA_POSITION_WORLD);
			ALBEDO = vec3(0.0);
			ALPHA = 1.0;
			#else
			discard;
			#endif
		#endif
		#ifdef OBJ_GLOW_ROTATED_SPECULAR
		obj_apply_q3_coverage(CAMERA_POSITION_WORLD);
		ALBEDO = scene_output(obj_apply_additive_fog(
				obj_glass_glow(CAMERA_POSITION_WORLD), CAMERA_POSITION_WORLD));
		ALPHA = 1.0;
		#endif
		#ifdef OBJ_GLOW_NORMAL_COPY
		float q3_alpha;
		vec3 q3_lit;
		obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX,
				q3_lit, q3_alpha);
		ALBEDO = scene_output(
				obj_apply_policy_fog(q3_lit, CAMERA_POSITION_WORLD));
		ALPHA = clamp(q3_alpha, 0.0, 1.0);
		#endif
	} else if (obj_is_slot_shadow_capture(CAMERA_VISIBLE_LAYERS,
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
		ALPHA = 1.0;
		#endif
	} else {
		vec3 lit;
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
}
