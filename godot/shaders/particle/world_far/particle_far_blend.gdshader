shader_type spatial;
// Particle pass A, type 0 Blend: SRCALPHA/INVSRCALPHA, fogged toward the scene fog (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mix, depth_draw_never;

#define PARTICLE_FAR_BLEND
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
