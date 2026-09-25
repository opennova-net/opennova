shader_type spatial;
// Particle pass A, type 0 Blend in a thermal-view frame: the secondary material, MODULATE(1 - TEXTURE, 1 - DIFFUSE) on colour, SRCALPHA/INVSRCALPHA, fogged toward the scene fog (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mix, depth_draw_never;

#define PARTICLE_FAR_BLEND
#define PARTICLE_FAR_THERMAL
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
