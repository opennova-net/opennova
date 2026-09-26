shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Bump (3): the
// DOT3 + alpha-restore stages (particle_bump.gdshaderinc) under
// SRCALPHA/INVSRCALPHA. The material's intrinsic pass word is 0x10000 (retail
// CParticleTexture_InitTextureAndChannels @ 0x5E8424).
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

#include "res://shaders/particle/particle_bump.gdshaderinc"
