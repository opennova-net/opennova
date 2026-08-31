void fragment() {
	vec3 match_terrain;
	if (obj_match_terrain_replaces_normal(match_terrain)) {
		ALBEDO = scene_output(match_terrain);
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
		ALPHA = 1.0;
	} else {
		ObjSurface s = obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX);
		ALBEDO = scene_output(s.albedo);
		EMISSION = clamp(s.emission, vec3(0.0), vec3(2.0));
		NORMAL = normalize((VIEW_MATRIX * vec4(s.normal_ws, 0.0)).xyz);
		ROUGHNESS = s.roughness;
		SPECULAR = s.specular;
		METALLIC = s.metallic;
		AO = obj_interior_ambient_factor();
		AO_LIGHT_AFFECT = 0.0;
		ALPHA = clamp(s.alpha, 0.0, 1.0);
	}
}
