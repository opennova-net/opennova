shader_type spatial;
render_mode unshaded, blend_mix, depth_prepass_alpha, depth_draw_opaque, cull_back;

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/detail.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/technique/fixed.gdshaderinc"
#include "res://shaders/object/output_alpha.gdshaderinc"
