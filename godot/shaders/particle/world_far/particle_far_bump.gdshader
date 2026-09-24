shader_type spatial;
// Particle pass A, type 3 Bump: DOT3 + specular, SRCALPHA/INVSRCALPHA, no fog (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mix, depth_draw_never;

#define PARTICLE_FAR_BUMP
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
