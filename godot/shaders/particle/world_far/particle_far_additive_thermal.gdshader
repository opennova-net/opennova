shader_type spatial;
// Particle pass A, types 1 Additive / 2 Premult in a thermal-view frame: the secondary material, MODULATE(TEXTURE, DIFFUSE) under ZERO/INVSRCCOLOR, fogged toward black; the multiply blend over the complemented source is dst x (1 - src) (particle_far_pass.gdshaderinc).
render_mode unshaded, fog_disabled, cull_disabled, blend_mul, depth_draw_never;

#define PARTICLE_FAR_ADDITIVE
#define PARTICLE_FAR_THERMAL
#include "res://shaders/particle/world_far/particle_far_pass.gdshaderinc"
