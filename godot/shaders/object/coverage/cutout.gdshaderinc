uniform float u_alpha_test_threshold = 0.5;
uniform float u_alpha_test_invert = 0.0;

void obj_apply_coverage(float alpha) {
	// Retail keeps > ref; invert changes the comparison to <= ref.
	// [orig: CRenderBatchQueue_FlushBatches @ 0x5DA3A9..0x5DA401]
	if (u_alpha_test_invert > 0.5) {
		if (alpha > u_alpha_test_threshold) {
			discard;
		}
	} else if (alpha <= u_alpha_test_threshold) {
		discard;
	}
}
