void fragment() {
	vec3 albedo;
	vec3 normal;
	vec3 emission;
	float roughness;
	float specular;
	float alpha;
	obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX,
			albedo, normal, emission, roughness, specular, alpha);
	ALBEDO = albedo;
	NORMAL = normalize((VIEW_MATRIX * vec4(normal, 0.0)).xyz);
	METALLIC = 0.0;
	ROUGHNESS = roughness;
	SPECULAR = specular;
	EMISSION = emission;
	ALPHA = clamp(alpha, 0.0, 1.0);
}
