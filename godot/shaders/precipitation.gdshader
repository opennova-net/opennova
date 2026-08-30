// The precipitation streak pass — the device state of the retail rain/snow
// drawer [orig: render_weather_trail_particles @ 0x5dee10: pass flags
// 0x10500000 = lighting off, z-write off, cull none, no fog, no alpha test;
// the one-texture mode word 0x651 = SRCALPHA/INVSRCALPHA blend, alpha stage
// MODULATE (texture x diffuse), color stage MODULATE2X (texture x diffuse
// x 2); textures eraindrp.tga / jsnwflk.tga; diffuse Env_TerrainLightCombined
// | 0xFF000000]. The geometry (one triangle per drop) arrives compiled from
// engine/runtime/renderer/precipitation_frame.h; this shader only reproduces
// the fixed-function combine.
shader_type spatial;
render_mode unshaded, blend_mix, cull_disabled, depth_draw_never, depth_test_disabled, fog_disabled;

uniform sampler2D drop_texture : source_color, filter_linear, repeat_disable;
uniform vec4 diffuse : source_color = vec4(1.0, 1.0, 1.0, 1.0);

void fragment() {
	vec4 texel = texture(drop_texture, UV);
	// MODULATE2X on the color stage, MODULATE on alpha.
	ALBEDO = min(texel.rgb * diffuse.rgb * 2.0, vec3(1.0));
	ALPHA = texel.a * diffuse.a;
}
