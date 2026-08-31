shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled, fog_disabled;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_FFP
#define OBJ_COVERAGE_DIFFUSE_ALPHA
#define OBJ_CLIP_EXPLICIT
#define OBJ_MATCHTERRAIN_NO_PASS

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/technique/fixed.gdshaderinc"
#include "res://shaders/object/output_add.gdshaderinc"
