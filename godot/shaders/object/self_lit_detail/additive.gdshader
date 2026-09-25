shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_back;

#define OBJ_RGB_MOD_SELF_LUM
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_ZERO
#define OBJ_CLIP_EXPLICIT
#define OBJ_VERTEX_POINT_LIGHTS_NONE
#define OBJ_VERTEX_FF_DIFFUSE
#define OBJ_MATCHTERRAIN_NO_PASS
#define OBJ_BLENDED_PASS

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/detail.gdshaderinc"
#include "res://shaders/object/coverage/full.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/fog/additive.gdshaderinc"
#include "res://shaders/object/technique/self_lit.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
