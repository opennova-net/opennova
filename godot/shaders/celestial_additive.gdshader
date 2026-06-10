shader_type spatial;
// Additive sky bodies (glare, stars). The engine submits these with the
// additive flag 0x110 [orig: render_skybox_layers @ 0x5ac230]; they write light
// into the framebuffer rather than occluding. Brightness scales by u_opacity
// (glare follows the view-sun dot; see nova_celestial.gd). docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, depth_test_disabled, cull_disabled, blend_add;

uniform sampler2D u_diffuse : source_color, filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	ALBEDO = tex.rgb * u_tint * tex.a * u_opacity;
	ALPHA = 1.0;
}
