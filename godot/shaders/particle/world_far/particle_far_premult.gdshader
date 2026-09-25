shader_type spatial;
// Particle pass A, type 2 Premult: ONE/INVSRCALPHA, fogged toward black (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_premul_alpha, depth_draw_never;

#define PARTICLE_FAR_ADDITIVE
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
