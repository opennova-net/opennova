// [orig: WaterRing_Draw @ 0x5DE0F0; WaterRing_LoadResources @ 0x5DDC90]
// WaterRing_Draw owns the draw state: the 0.4 ambient material
// (@ 0x5DE202..0x5DE217, SetMaterial @ 0x5DE232), D3DRS_AMBIENT (139) white
// (@ 0x5DE1EC), GfxShader_ApplyPassChecked pass 0x100000 on the wake effect
// (@ 0x5DE245) and the first-UV scroll off the render frame counter
// (@ 0x5DE277..0x5DE2AD). WaterRing_LoadResources loads wake5.tga / wakegrad.tga, and its
// GfxShader_SetFfpLightingSources call (@ 0x680720) also enables FFP
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
#include "res://shaders/texture_filter.gdshaderinc"

// Both textures are created with 0x100000, no 0x8, so the device samples them
// MIN/MAG LINEAR with MIPFILTER POINT whatever game.cfg's texfilter_level says,
// over their pixel chains (wake5.tga 256 x 256 and wakegrad.tga 32 x 32, each
// down to 4 x 4) [orig: WaterRing_LoadResources @ 0x5ddca1, @ 0x5ddcbc;
// GTexture_CreateFromPixelData_0 @ 0x6877ba..0x6877e0; CGfxDevice_ApplyRenderStates
// @ 0x67e463..0x67e4a7] (engine renderer/texture_filter.h, TextureStage::WaterWake):
// bilinear on the nearest level, never past *_max_lod (the presenter's
// texture_max_lod).
uniform sampler2D wake_texture : filter_linear_mipmap, repeat_enable;
uniform sampler2D gradient_texture : filter_linear_mipmap, repeat_disable;
uniform float wake_max_lod = 1000.0;
uniform float gradient_max_lod = 1000.0;

void fragment() {
    vec4 wake = sample_point_mip(wake_texture, UV, wake_max_lod);
    // The gradient clamps where the ring wraps, and a shader function takes
    // one sampler setting, so its nearest level is taken here.
    float gradient_lod = retail_isotropic_lod(UV2, vec2(textureSize(gradient_texture, 0)));
    vec4 gradient = textureLod(gradient_texture, UV2,
            retail_point_mip_level(gradient_lod, gradient_max_lod));
    ALBEDO = scene_output(min(wake.rgb * gradient.rgb * 0.8, vec3(1.0)));
    ALPHA = wake.a * gradient.a * COLOR.a;
}
