// [orig: sub_5DE0F0 @ 0x5DE0F0; sub_5DDC90 @ 0x5DDC90]
// Water wake's two-texture 0x651 material: MODULATE2X color,
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
