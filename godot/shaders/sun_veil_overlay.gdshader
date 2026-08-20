// The sun-glare screen veil: a fullscreen WHITE quad whose alpha is the
// dot^32 glare byte [orig: Environment_ApplySunVeilAndExposureStopdown
// @ 0x5ad8b0 -> render_fullscreen_decal_quad @ 0x5ad94f — quad color
// (glare << 24) + 0xFFFFFF, alpha-blended]. Celestial pushes the global once
// per advanced frame (draw gate glare > 2 baked into the pushed value);
// PlayerViewEffects owns the rect.
shader_type canvas_item;

global uniform float opennova_sun_veil_alpha;

void fragment() {
	COLOR = vec4(1.0, 1.0, 1.0, opennova_sun_veil_alpha);
}
