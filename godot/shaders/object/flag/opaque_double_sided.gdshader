shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_opaque, cull_disabled, fog_disabled;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_DIFFUSE_ALPHA
#define OBJ_CLIP_NORMAL_FALLBACK
#define OBJ_VERTEX_POINT_LIGHTS_NONE
#define OBJ_MATCHTERRAIN_NO_PASS

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/full.gdshaderinc"
#include "res://shaders/object/normal/none.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_flag.gdshaderinc"
#include "res://shaders/object/fog/regular.gdshaderinc"
#include "res://shaders/object/technique/flag.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
