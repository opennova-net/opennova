shader_type spatial;
// Sun/moon/star/glare bodies attached to the sky dome.
// [orig: render_skybox_layers @ 0x5ac230] submits these as additive entities at
// camera + direction*2000 with no depth test; the engine draws them after the
// dome and before the world. We keep the 3DI diffuse texture and modulate by the
// TOD sun/moon color. See docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, depth_test_disabled, cull_disabled, blend_mix;

uniform sampler2D u_diffuse : source_color, filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;

void fragment() {
	vec4 tex = texture(u_diffuse, UV);
	ALBEDO = tex.rgb * u_tint;
	ALPHA = tex.a * u_opacity;
}
