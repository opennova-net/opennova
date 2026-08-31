// The scene is linear and canonical (ADR 0043, linear-scene amendment):
// color textures decode through source_color, witnessed palette bytes decode
// once at the Godot seam, and framebuffer blending plus the output transfer
// are stock Godot. Retail's selected renderer was gamma-space end to end
// [orig: CD3DDevice_InitializeDisplay @ 0x679c1b..0x679d29;
// CGfxDevice_ApplyRenderStates @ 0x67e3ec..0x67e4f3;
// GLib_SetGammaRamp @ 0x677be0; default gamma @ 0x84f354] — the retired
// gamma-domain scene contract and its terminal DisplayDecodeEffect are
// recorded in docs/render/render-material-re.md (D-RMAT-7/-8, superseded).
//
// These helpers serve the passes that still evaluate witnessed math on
// gamma bytes: canvas posts over the displayed frame (nvg_view) and vertex
// streams carrying witnessed byte-encoded colors (the water strip).

vec3 srgb_decode(vec3 srgb_rgb) {
	vec3 c = clamp(srgb_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}

vec3 srgb_encode(vec3 linear_rgb) {
	vec3 c = clamp(linear_rgb, vec3(0.0), vec3(1.0));
	return mix(vec3(1.055) * pow(c, vec3(1.0 / 2.4)) - vec3(0.055),
			c * 12.92, lessThanEqual(c, vec3(0.0031308)));
}
