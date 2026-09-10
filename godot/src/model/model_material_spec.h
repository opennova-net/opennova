#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

#include <formats/threedi/threedi_3di3.h>

namespace godot {

// One MTRL texture slot row: the filename the model names, the slot it fills
// (diffuse 1, detail 2, normal 3, normal_b 4), its type word (diffuse 0,
// normal-from-MDT 4, normal-from-TGA-alpha 5), the flag bits (animated,
// clamped) and the animation frame index.
class ModelTextureRow : public Resource {
	GDCLASS(ModelTextureRow, Resource)

	String texture_name_;
	int slot_ = opennova::threedi::THREEDI_TEX_SLOT_DIFFUSE;
	int type_ = opennova::threedi::THREEDI_TEX_TYPE_DIFFUSE;
	int flags_ = 0;
	int frame_ = 0;

protected:
	static void _bind_methods();

public:
	void set_texture_name(const String &p_value) { texture_name_ = p_value; }
	String get_texture_name() const { return texture_name_; }
	void set_slot(int p_value) { slot_ = p_value; }
	int get_slot() const { return slot_; }
	void set_type(int p_value) { type_ = p_value; }
	int get_type() const { return type_; }
	void set_flags(int p_value) { flags_ = p_value; }
	int get_flags() const { return flags_; }
	void set_frame(int p_value) { frame_ = p_value; }
	int get_frame() const { return frame_; }

	void assign(const opennova::threedi::ThreediMaterialTexture &p_tex);
	bool write(opennova::threedi::ThreediMaterialTexture &r_tex) const;
};

// One MTRL record as the author sees it: the shader tag, the flag bytes, the
// generators and the texture rows. Every quantized on-disk value is stored
// in its INTEGER form (bytes, Q8 rates, the phase-or-register byte) so an
// export reproduces bytes without a float round trip; the colors read and
// write as Color and quantize to bytes on set. `material_name` keys the row
// from a surface material's resource_name; `alpha_strips` sorts the strips
// that use the row into their part's alpha bucket (the renderer's second
// walk) instead of the opaque one.
class ModelMaterialSpec : public Resource {
	GDCLASS(ModelMaterialSpec, Resource)

public:
#define MODEL_MATERIAL_INT_FIELDS(X)  \
	X(material_flags)                 \
	X(alpha_test_byte)                \
	X(emissive_type)                  \
	X(emissive_type2)                 \
	X(is_glass)                       \
	X(glass_type2)                    \
	X(alpha_gen_style)                \
	X(alpha_gen_phase_or_reg)         \
	X(alpha_gen_rate_q8)              \
	X(alpha_gen_start)                \
	X(alpha_gen_end)                  \
	X(rgb_gen_style)                  \
	X(rgb_gen_phase_or_reg)           \
	X(rgb_gen_rate_q8)                \
	X(rgb_gen2_style)                 \
	X(rgb_gen2_phase_or_reg)          \
	X(rgb_gen2_rate_q8)               \
	X(u_style)                        \
	X(u_phase_or_reg)                 \
	X(u_rate_q8)                      \
	X(u_start_q8)                     \
	X(u_end_q8)                       \
	X(v_style)                        \
	X(v_phase_or_reg)                 \
	X(v_rate_q8)                      \
	X(v_start_q8)                     \
	X(v_end_q8)                       \
	X(animation_frames)               \
	X(animation_type)                 \
	X(animation_cycle)

#define MODEL_MATERIAL_COLOR_FIELDS(X) \
	X(rgb_gen_start_color)             \
	X(rgb_gen_end_color)               \
	X(rgb_gen2_start_color)            \
	X(rgb_gen2_end_color)              \
	X(reflect_color)                   \
	X(reflect_color2)

private:
	String material_name_;
	String shader_tag_ = "FF_ST_OP";
	bool alpha_strips_ = false;
	TypedArray<ModelTextureRow> textures_;
#define MODEL_MATERIAL_INT_MEMBER(m_name) int m_name##_ = 0;
	MODEL_MATERIAL_INT_FIELDS(MODEL_MATERIAL_INT_MEMBER)
#undef MODEL_MATERIAL_INT_MEMBER
#define MODEL_MATERIAL_COLOR_MEMBER(m_name) uint8_t m_name##_[4] = {0, 0, 0, 0};
	MODEL_MATERIAL_COLOR_FIELDS(MODEL_MATERIAL_COLOR_MEMBER)
#undef MODEL_MATERIAL_COLOR_MEMBER

	static Color color_from_bytes(const uint8_t p_rgba[4]);
	static void bytes_from_color(const Color &p_color, uint8_t r_rgba[4]);

protected:
	static void _bind_methods();

public:
	void set_material_name(const String &p_value) { material_name_ = p_value; }
	String get_material_name() const { return material_name_; }
	void set_shader_tag(const String &p_value) { shader_tag_ = p_value; }
	String get_shader_tag() const { return shader_tag_; }
	void set_alpha_strips(bool p_value) { alpha_strips_ = p_value; }
	bool get_alpha_strips() const { return alpha_strips_; }
	void set_textures(const TypedArray<ModelTextureRow> &p_value) { textures_ = p_value; }
	TypedArray<ModelTextureRow> get_textures() const { return textures_; }

#define MODEL_MATERIAL_INT_ACCESSORS(m_name)                     \
	void set_##m_name(int p_value) { m_name##_ = p_value; }      \
	int get_##m_name() const { return m_name##_; }
	MODEL_MATERIAL_INT_FIELDS(MODEL_MATERIAL_INT_ACCESSORS)
#undef MODEL_MATERIAL_INT_ACCESSORS
#define MODEL_MATERIAL_COLOR_ACCESSORS(m_name)                                       \
	void set_##m_name(const Color &p_value) { bytes_from_color(p_value, m_name##_); } \
	Color get_##m_name() const { return color_from_bytes(m_name##_); }
	MODEL_MATERIAL_COLOR_FIELDS(MODEL_MATERIAL_COLOR_ACCESSORS)
#undef MODEL_MATERIAL_COLOR_ACCESSORS

	// The filename in the diffuse slot (the first slot-1 row), or "".
	String get_diffuse_texture() const;

	// MTRL record <-> spec. `assign` keeps every field; `write` fills a
	// zeroed record (the material index is the caller's).
	void assign(const opennova::threedi::ThreediMaterial &p_material);
	bool write(opennova::threedi::ThreediMaterial &r_material, String &r_error) const;
};

} // namespace godot
