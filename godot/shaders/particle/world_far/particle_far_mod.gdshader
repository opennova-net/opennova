shader_type spatial;
// Particle pass A, type 4 Mod: DESTCOLOR/ZERO, fogged toward white (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mul, depth_draw_never;

#define PARTICLE_FAR_MOD
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
