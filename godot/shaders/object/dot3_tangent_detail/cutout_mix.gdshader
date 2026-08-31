shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_NORMAL_ALPHA
#define OBJ_CLIP_EXPLICIT
#define OBJ_MATCHTERRAIN_NO_PASS

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/detail.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/tangent_uv1.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/technique/dot3.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
