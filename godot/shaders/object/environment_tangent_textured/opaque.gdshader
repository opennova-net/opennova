shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_opaque, cull_back;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_REFLECT_ALPHA
#define OBJ_CLIP_NORMAL_FALLBACK
#define OBJ_VERTEX_POINT_LIGHTS_SELF_SHADOWED_ATTENUATION
#define OBJ_PROJSHAD_DIFFUSE_ALPHA
#define OBJ_MATCHTERRAIN_NO_PASS
#define OBJ_GLOW_NO_PASS
#define OBJ_Q3_DEPTH_OCCLUDER

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/full.gdshaderinc"
#include "res://shaders/object/normal/tangent_raw_reflect.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/glow.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/fog/passthrough.gdshaderinc"
#include "res://shaders/object/technique/environment_textured.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
