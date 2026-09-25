shader_type spatial;
// Particle pass A, type 5 Mod2x: DESTCOLOR/SRCCOLOR, fogged toward mid-gray (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mul, depth_draw_never;

#define PARTICLE_FAR_MOD2X
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
