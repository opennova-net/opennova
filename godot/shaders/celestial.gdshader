shader_type spatial;
// Sun/moon/star/glare bodies attached to the sky dome.
// [orig: render_celestial_bodies @ 0x5acaa0] places these at camera +
// direction * 64; the engine draws them after the dome and before the world,
// so the world overdraws them - DEPTH-TESTED (no write) is the structural
// equivalent here. We keep the 3DI diffuse texture and modulate by the TOD
// sun/moon color. See docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_mix;

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Raw sampling per the gamma-space convention (nova_color.gdshaderinc).
uniform sampler2D u_diffuse : filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	// Gamma-space modulate + encoded output (D-RMAT-7); the alpha-blend dst
	// term stays linear-space (D-RMAT-8).
	ALBEDO = nova_gamma_to_linear(tex.rgb * u_tint);
	ALPHA = tex.a * u_opacity;
}
