// [orig: render_water_surface_decal @ 0x5DE0F0; sub_5DDC90 @ 0x5DDC90]
// render_water_surface_decal owns the draw state: the 0.4 ambient material
// (@ 0x5DE202..0x5DE217, SetMaterial @ 0x5DE232), D3DRS_AMBIENT (139) white
// (@ 0x5DE1EC), GfxShader_ApplyPassChecked pass 0x100000 on the wake effect
// (@ 0x5DE245) and the first-UV scroll off the render frame counter
// (@ 0x5DE277..0x5DE2AD). sub_5DDC90 only loads wake5.tga / wakegrad.tga and
// sets their sampler addressing. Two-texture material: MODULATE2X color,
// MODULATE alpha, source-alpha blending. The portable draw supplies both UVs
// and each ring's lifetime alpha (renderer/water_wake_frame.cpp).
shader_type spatial;
render_mode unshaded, blend_mix, cull_disabled, depth_draw_never;

#include "res://shaders/color.gdshaderinc"

uniform sampler2D wake_texture : filter_linear, repeat_enable;
uniform sampler2D gradient_texture : filter_linear, repeat_disable;

void fragment() {
    vec4 wake = texture(wake_texture, UV);
    vec4 gradient = texture(gradient_texture, UV2);
    ALBEDO = scene_output(min(wake.rgb * gradient.rgb * 0.8, vec3(1.0)));
    ALPHA = wake.a * gradient.a * COLOR.a;
}
