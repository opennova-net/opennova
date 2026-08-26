// Retail's selected renderer is gamma-space end to end: texture bytes enter
// the effect math raw, every combine and framebuffer blend runs on those
// gamma-encoded values, and the framebuffer byte is the displayed value.
// There is no D3DSAMP_SRGBTEXTURE or D3DRS_SRGBWRITEENABLE in the witnessed
// device path or 44-file effect corpus, and the default display ramp is the
// identity. [orig: CD3DDevice_InitializeDisplay @ 0x679c1b..0x679d29;
// CGfxDevice_ApplyRenderStates @ 0x67e3ec..0x67e4f3;
// GLib_SetGammaRamp @ 0x677be0; default gamma @ 0x84f354].
//
// Spatial retail passes therefore write gamma-domain numeric values directly
// into Godot's floating-point scene target. FrameFxCompositorEffect runs
// after the final 3D/particle blend and performs the sole display decode.
// Offscreen retail devices intentionally bypass that compositor and retain
// gamma-domain values. Canvas post effects use the explicit display helpers.

vec3 nova_scene_output(vec3 gamma_rgb) {
	return clamp(gamma_rgb, vec3(0.0), vec3(1.0));
}

vec3 nova_scene_input(vec3 gamma_rgb) {
	return clamp(gamma_rgb, vec3(0.0), vec3(1.0));
}

vec3 nova_display_decode_gamma(vec3 gamma_rgb) {
	vec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}

vec3 nova_display_encode_gamma(vec3 linear_rgb) {
	vec3 c = clamp(linear_rgb, vec3(0.0), vec3(1.0));
	return mix(vec3(1.055) * pow(c, vec3(1.0 / 2.4)) - vec3(0.055),
			c * 12.92, lessThanEqual(c, vec3(0.0031308)));
}
