shader_type spatial;
render_mode blend_mix, depth_prepass_alpha, depth_draw_opaque, cull_back;

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/object_uv1.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/technique/phong_specular.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
