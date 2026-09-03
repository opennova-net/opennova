#pragma once

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

// One MTRL row of a loaded .3di document as ObjectData reads it back for the
// material builder (object_model_materials.cpp): the shader tag and flags,
// the first diffuse/detail/normal texture names, and the RGB / alpha / UV
// generators with their CTRL registers (formats/threedi/threedi_3di3.h
// ThreediMaterial). A C++-only value (the ClassDB record died with the
// ADR 0043 d10 mirror sweep; the fixture facts it served GUT are pinned by
// the minimal_3di_gen ctest over the same bytes).

namespace godot {

struct MaterialInfo {
	String name;
	String shader_tag;
	int alpha_test = 0;
	bool alpha_invert = false;
	bool two_sided = false;
	bool alpha_test_enabled = false;
	bool is_glass = false;
	bool emissive = false;
	String diffuse_a;
	String detail_a;
	String normal_a;
	Color reflect_color;
	int rgb_gen_style = 0;
	float rgb_gen_rate = 0.0f;
	float rgb_gen_phase = 0.0f;
	Color rgb_gen_start_color;
	Color rgb_gen_end_color;
	int rgb_gen_reg = -1;
	String rgb_gen_reg_name;
	int alpha_gen_style = 0;
	float alpha_gen_rate = 0.0f;
	float alpha_gen_phase = 0.0f;
	int alpha_gen_start = 0;
	int alpha_gen_end = 0;
	int alpha_gen_reg = -1;
	String alpha_gen_reg_name;
	int uv_u_style = 0;
	float uv_u_rate = 0.0f;
	float uv_u_phase = 0.0f;
	float uv_u_start = 0.0f;
	float uv_u_end = 0.0f;
	int uv_u_reg = -1;
	String uv_u_reg_name;
	int uv_v_style = 0;
	float uv_v_rate = 0.0f;
	float uv_v_phase = 0.0f;
	float uv_v_start = 0.0f;
	float uv_v_end = 0.0f;
	int uv_v_reg = -1;
	String uv_v_reg_name;
	int anim_frames = 0;
	int anim_type = 0;
	int anim_frame_time = 0;
};

} // namespace godot
