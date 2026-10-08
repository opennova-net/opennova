#ifndef OPENNOVA_TEXTURE_FILTER_GDSHADERINC
#define OPENNOVA_TEXTURE_FILTER_GDSHADERINC

// The per-stage texture filters (D-RMAT-22). The engine decides each stage's
// sampler (runtime/renderer/texture_filter.h stage_sampler) and the world's
// device leg publishes the two that follow game.cfg's texfilter_level as
// shader filter codes (renderer::shader_filter_code: 0 = bilinear on the
// nearest mip level, 1 = trilinear, 2 = anisotropic):
// - opennova_texfilter_device: the terrain detail family's, the only textures
//   created with flag 0x8, under the session's device mode (level + 1). Its
//   anisotropy is the world viewports' hardware level.
// - opennova_texfilter_effect: the model stages' (every effect's
//   sampLinearWrap2D), from the configuration word.
// Every other fixed-function stage samples bilinear with point mips at any
// level (sample_point_mip).
// [orig: CGfxDevice_ApplyRenderStates @ 0x67e37d..0x67e4a7;
//  HLSLEffect_LoadFromFile @ 0x5ae6b9..0x5ae6e5; _BaseInc.fx samplers]
global uniform int opennova_texfilter_device;
global uniform int opennova_texfilter_effect;

// The device's isotropic level of detail: log2 of the longer of the two
// screen-axis texel footprints.
float retail_isotropic_lod(vec2 uv, vec2 size) {
	float footprint = max(length(dFdx(uv) * size), length(dFdy(uv) * size));
	return log2(max(footprint, 1.0e-8));
}

// The nearest mip level to `lod` (MIPFILTER POINT), at most max_lod; a
// magnified footprint stays on level 0.
float retail_point_mip_level(float lod, float max_lod) {
	return clamp(floor(lod + 0.5), 0.0, max(max_lod, 0.0));
}

// MIN/MAG LINEAR with MIPFILTER POINT: bilinear on the nearest level, never
// past max_lod (the stage's last retail level).
vec4 sample_point_mip(sampler2D tex, vec2 uv, float max_lod) {
	float lod = retail_isotropic_lod(uv, vec2(textureSize(tex, 0)));
	return textureLod(tex, uv, retail_point_mip_level(lod, max_lod));
}

#endif
