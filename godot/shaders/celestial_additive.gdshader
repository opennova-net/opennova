shader_type spatial;
// Additive sky bodies (glare, stars). The engine loads glare/star models
// additively (mode 0x300000 [orig: EffectWorld_LoadCelestialModels @ 0x5adc50]);
// they write light into the framebuffer rather than occluding. Depth-tested
// (no write) like the plain bodies. Brightness scales by u_opacity - the
// glare's is the witnessed dot^4/2 x occlusion chain
// [orig: render_skybox_sun_glow @ 0x5acd00]. docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_add;

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Raw sampling per the gamma-space convention (nova_color.gdshaderinc).
uniform sampler2D u_diffuse : filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	// Gamma-space modulate + encoded output (D-RMAT-7); additive accumulation
	// happens in the linear buffer — the bounded residual is D-RMAT-8.
	// COLOR = per-instance brightness for the star field's twinkle (env #33;
	// plain meshes default to white).
	ALBEDO = nova_gamma_to_linear(tex.rgb * u_tint * COLOR.rgb * tex.a * u_opacity);
	ALPHA = 1.0;
}
