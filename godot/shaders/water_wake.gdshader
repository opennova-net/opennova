// [orig: render_water_surface_decal @ 0x5DE0F0; sub_5DDC90 @ 0x5DDC90]
// render_water_surface_decal owns the draw state: the 0.4 ambient material
// (@ 0x5DE202..0x5DE217, SetMaterial @ 0x5DE232), D3DRS_AMBIENT (139) white
// (@ 0x5DE1EC), GfxShader_ApplyPassChecked pass 0x100000 on the wake effect
// (@ 0x5DE245) and the first-UV scroll off the render frame counter
// (@ 0x5DE277..0x5DE2AD). sub_5DDC90 loads wake5.tga / wakegrad.tga, and its
// CGfxTexture_SetSamplerAddressing call (@ 0x680720) also enables FFP
// lighting with the diffuse and ambient material sources on MATERIAL (the
// +0x58..+0x5B writes CGfxShader_ApplyPassRenderStates commits as RS 0x89 /
// 0x91 / 0x93 @ 0x6808ca..0x68091f): the lit colour is the 0.4 ambient under
// the white D3DRS_AMBIENT and the alpha the material's diffuse alpha, the
// ring's lifetime alpha. Two-texture material: MODULATE2X color, MODULATE
// alpha, source-alpha blending. The portable draw supplies both UVs and each
// ring's lifetime alpha (renderer/water_wake_frame.cpp). The pass
// flags carry no cull-none bit (0x400000, which every water strip pass sets),
// so the rings keep the device's back-face cull: their front faces up and an
// eye below the water sees none.
shader_type spatial;
render_mode unshaded, blend_mix, cull_back, depth_draw_never;

#include "res://shaders/color.gdshaderinc"

uniform sampler2D wake_texture : filter_linear, repeat_enable;
uniform sampler2D gradient_texture : filter_linear, repeat_disable;

void fragment() {
    vec4 wake = texture(wake_texture, UV);
    vec4 gradient = texture(gradient_texture, UV2);
    ALBEDO = scene_output(min(wake.rgb * gradient.rgb * 0.8, vec3(1.0)));
    ALPHA = wake.a * gradient.a * COLOR.a;
}
