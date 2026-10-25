// The sun-glare screen veil: a fullscreen WHITE quad whose alpha is the
// dot^16 glare byte, drawn only on a frame whose glare passes the gate
// [orig: Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0: glare <= 2
// skips the quad (cmp @ 0x5ad925, jle @ 0x5ad928), else
// Render_FullscreenDecalQuad called @ 0x5ad954 with quad color
// (glare << 24) + 0xFFFFFF, alpha-blended]. Celestial pushes the global once
// per advanced frame (the gate baked into the pushed value: 0 when it skips);
// PlayerViewEffects owns the rect. A gated frame draws nothing: the quad's
// fragments discard rather than blend a zero alpha over the whole surface.
shader_type canvas_item;

global uniform float opennova_sun_veil_alpha;

void fragment() {
	if (opennova_sun_veil_alpha <= 0.0) {
		discard;
	}
	COLOR = vec4(1.0, 1.0, 1.0, opennova_sun_veil_alpha);
}
