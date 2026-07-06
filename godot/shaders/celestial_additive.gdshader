shader_type spatial;
// Additive sky bodies (glare, stars). The engine loads glare/star models
// additively (mode 0x300000 [orig: EffectWorld_LoadCelestialModels @ 0x5adc50]);
// they write light into the framebuffer rather than occluding. Depth-tested
// (no write) like the plain bodies. Brightness scales by u_opacity - the
// glare's is the witnessed dot^4/2 x occlusion chain
// [orig: render_skybox_sun_glow @ 0x5acd00]. docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_add;

uniform sampler2D u_diffuse : source_color, filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	ALBEDO = tex.rgb * u_tint * tex.a * u_opacity;
	ALPHA = 1.0;
}
