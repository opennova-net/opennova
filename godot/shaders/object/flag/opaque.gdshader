shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_opaque, cull_back;

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/full.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/vertex_flag.gdshaderinc"
#include "res://shaders/object/technique/flag.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
