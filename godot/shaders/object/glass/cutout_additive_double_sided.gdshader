shader_type spatial;
render_mode unshaded, blend_add, depth_prepass_alpha, depth_draw_opaque, cull_disabled;

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/technique/glass.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
