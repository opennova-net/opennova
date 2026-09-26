shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Bumpadd (6).
// Retail uses the same DOT3 + alpha-restore stages as Bump, then blends with
// SRCALPHA/ONE. The parser's substring order makes authored `bumpadd` resolve
// to Bump, but this exact program remains reachable through numeric type 6.
// Its intrinsic pass word is 0x10000 (retail
// CParticleTexture_InitTextureAndChannels @ 0x5E850A): SPECULARENABLE adds the
// SPECULAR colour (CUSTOM0) after the stages, and there is no FOGENABLE.
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

#include "res://shaders/particle/particle_bump.gdshaderinc"
