shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_back, fog_disabled;

#define OBJ_POSTMULTIPLY_CUTOUT
#include "res://shaders/object/postmultiply/environment_textured_body.gdshaderinc"
