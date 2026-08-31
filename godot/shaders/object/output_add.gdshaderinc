// The additive island (ADR 0043): additive wrappers stay unshaded — an
// additive surface is a glow, not a lit body — and keep the retail
// fade-to-black fog. The technique's emission rides as-is; a technique that
// filled only albedo (fixed/detail additive) takes the self-lit gain law.
void fragment() {
	ObjSurface s = obj_evaluate_surface(CAMERA_POSITION_WORLD, MODEL_MATRIX);
	vec3 lit = s.emission;
	if (lit == vec3(0.0)) {
		lit = obj_self_lit(s.albedo);
	}
	ALBEDO = clamp(obj_apply_additive_fog(lit, CAMERA_POSITION_WORLD),
			vec3(0.0), vec3(1.0));
	ALPHA = clamp(s.alpha, 0.0, 1.0);
}
