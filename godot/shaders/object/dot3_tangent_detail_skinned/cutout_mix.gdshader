shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_opaque, cull_back;

#define OBJ_RGB_MOD_NONE
#define OBJ_ALPHA_MOD_NONE
#define OBJ_COVERAGE_DIFFUSE_ALPHA
#define OBJ_CLIP_SKINNED_SUBMIT_SKIP
#define OBJ_VERTEX_POINT_LIGHTS_ATTENUATION
#define OBJ_SKIN_NORMAL_FIRST_BONE
#define OBJ_MATCHTERRAIN_SKINNED_STANCE
#define OBJ_FOG_VERTEX_SHADER

#include "res://shaders/object/shared.gdshaderinc"
#include "res://shaders/object/skin.gdshaderinc"
#include "res://shaders/object/sampling/detail.gdshaderinc"
#include "res://shaders/object/coverage/cutout.gdshaderinc"
#include "res://shaders/object/normal/tangent_uv1.gdshaderinc"
#include "res://shaders/object/surface.gdshaderinc"
#include "res://shaders/object/match_terrain.gdshaderinc"
#include "res://shaders/object/vertex_standard.gdshaderinc"
#include "res://shaders/object/fog/regular.gdshaderinc"
#include "res://shaders/object/technique/dot3_skinned.gdshaderinc"
#include "res://shaders/object/output_opaque.gdshaderinc"
