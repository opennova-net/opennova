#ifndef OPENNOVA_PARTICLE_ATLAS_GDSHADERINC
#define OPENNOVA_PARTICLE_ATLAS_GDSHADERINC

// The atlas page a particle batch binds (D-RMAT-23). Retail builds every page
// from pixels with 0x180000, so it carries at most three box-filtered levels,
// and draws it through the particle batch's GfxShader pass, so the stage is
// MIN/MAG LINEAR with MIPFILTER POINT at every texfilter_level: bilinear on the
// nearest level, never past the page's last (albedo_max_lod, from
// renderer::particle_atlas_page_last_level; the Godot image carries the
// retail levels, then a tail no draw reads).
// [orig: CParticleTexture_InitTextureAndChannels @ 0x5E82B2..0x5E82EB;
//  GTexture_CreateFromPixelData_0 @ 0x6877F2..0x687801;
//  CParticleBatch_FlushAndBindMaterial @ 0x5E42DC;
//  CGfxDevice_ApplyRenderStates @ 0x67E463..0x67E4A7]

#include "res://shaders/texture_filter.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_disable;
uniform float albedo_max_lod = 0.0;

vec4 particle_atlas_texel(vec2 uv) {
	return sample_point_mip(albedo_tex, uv, albedo_max_lod);
}

#endif
