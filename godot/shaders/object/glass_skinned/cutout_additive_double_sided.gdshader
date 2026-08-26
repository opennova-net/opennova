shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_REFLECT_ALPHA
#define OBJ_CLIP_SKINNED_SUBMIT_SKIP
#define OBJ_VERTEX_POINT_LIGHTS_NONE
#define OBJ_PROJSHAD_NO_PASS
#define OBJ_MATCHTERRAIN_NO_PASS
#define OBJ_GLOW_NO_PASS
#define OBJ_Q3_TRANSPARENT

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/glow.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/fog/additive.gdshaderinc"
#include "res://shaders/object/technique/glass_skinned.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
