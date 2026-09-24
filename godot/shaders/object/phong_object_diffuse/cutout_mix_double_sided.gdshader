shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_opaque, cull_disabled;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_DIFFUSE_ALPHA
#define OBJ_CLIP_EXPLICIT_UNSKINNED_OR_SUBMIT_SKIP_SKINNED
#define OBJ_CLIP_DOT3
#define OBJ_VERTEX_POINT_LIGHTS_SELF_SHADOWED_ATTENUATION
#define OBJ_MATCHTERRAIN_SKINNED_STANCE

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/sampling/single.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/object_uv1.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/fog/regular.gdshaderinc"
#include "res://shaders/object/technique/phong_diffuse.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
