void fragment() {
	vec3 lit;
	float alpha;
	obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX, lit, alpha);
	ALBEDO = nova_gamma_to_linear(lit);
	ALPHA = clamp(alpha, 0.0, 1.0);
}
