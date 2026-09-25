shader_type spatial;
// Particle pass A, type 6 Bumpadd: DOT3 + specular, SRCALPHA/ONE, no fog (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_add, depth_draw_never;

#define PARTICLE_FAR_BUMP
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
