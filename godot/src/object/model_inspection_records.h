#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

// The read-back records of a loaded .3di document and its live model
// (ObjectData / ObjectModel inspection, ADR 0042 d5): the MTRL row, the PANM
// row with its seven control tracks, the RMDL LOD header, and the model's
// presentation-state channels. Read-write so a stub authors one; each field
// list is an X-macro (type, name, default) generating accessors, members and
// the bindings (model_inspection_records.cpp).

#define INSPECTION_ACCESSORS(m_type, m_name, m_default)       \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define INSPECTION_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// One MTRL row: the shader tag and flags, the first diffuse/detail/normal
// texture names, and the RGB / alpha / UV generators with their CTRL
// registers (formats/threedi/threedi_3di3.h ThreediMaterial).
#define MATERIAL_INFO_FIELDS(X)                   \
	X(String, name, String())                     \
	X(String, shader_tag, String())               \
	X(int, alpha_test, 0)                         \
	X(bool, alpha_invert, false)                  \
	X(bool, two_sided, false)                     \
	X(bool, alpha_test_enabled, false)            \
	X(bool, is_glass, false)                      \
	X(bool, emissive, false)                      \
	X(String, diffuse_a, String())                \
	X(String, detail_a, String())                 \
	X(String, normal_a, String())                 \
	X(Color, reflect_color, Color())              \
	X(int, rgb_gen_style, 0)                      \
	X(float, rgb_gen_rate, 0.0f)                  \
	X(float, rgb_gen_phase, 0.0f)                 \
	X(Color, rgb_gen_start_color, Color())        \
	X(Color, rgb_gen_end_color, Color())          \
	X(int, rgb_gen_reg, -1)                       \
	X(String, rgb_gen_reg_name, String())         \
	X(int, alpha_gen_style, 0)                    \
	X(float, alpha_gen_rate, 0.0f)                \
	X(float, alpha_gen_phase, 0.0f)               \
	X(int, alpha_gen_start, 0)                    \
	X(int, alpha_gen_end, 0)                      \
	X(int, alpha_gen_reg, -1)                     \
	X(String, alpha_gen_reg_name, String())       \
	X(int, uv_u_style, 0)                         \
	X(float, uv_u_rate, 0.0f)                     \
	X(float, uv_u_phase, 0.0f)                    \
	X(float, uv_u_start, 0.0f)                    \
	X(float, uv_u_end, 0.0f)                      \
	X(int, uv_u_reg, -1)                          \
	X(String, uv_u_reg_name, String())            \
	X(int, uv_v_style, 0)                         \
	X(float, uv_v_rate, 0.0f)                     \
	X(float, uv_v_phase, 0.0f)                    \
	X(float, uv_v_start, 0.0f)                    \
	X(float, uv_v_end, 0.0f)                      \
	X(int, uv_v_reg, -1)                          \
	X(String, uv_v_reg_name, String())            \
	X(int, anim_frames, 0)                        \
	X(int, anim_type, 0)                          \
	X(int, anim_frame_time, 0)

class MaterialInfo : public RefCounted {
	GDCLASS(MaterialInfo, RefCounted)

public:
	MATERIAL_INFO_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	MATERIAL_INFO_FIELDS(INSPECTION_MEMBER)
};

// One PANM control track (ThreediTransform): the control style, its
// parameter (the CTRL register ordinal, resolved to reg_name), rate and range.
#define PART_ANIM_TRACK_FIELDS(X)     \
	X(int, control, 0)                \
	X(int, control_param, 0)          \
	X(String, reg_name, String())     \
	X(int, rate, 0)                   \
	X(int, start, 0)                  \
	X(int, end, 0)

class PartAnimTrack : public RefCounted {
	GDCLASS(PartAnimTrack, RefCounted)

public:
	PART_ANIM_TRACK_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	PART_ANIM_TRACK_FIELDS(INSPECTION_MEMBER)
};

// One PANM row (ThreediPartAnimation): the driven part, its parent, the
// decoded flag styles, and the seven tracks.
#define PART_ANIM_INFO_FIELDS(X)          \
	X(int, index, 0)                      \
	X(int, transform_as, 0)               \
	X(int, parent_subobject, 0)           \
	X(int64_t, flags, 0)                  \
	X(int, scale_type, 0)                 \
	X(int, rotation_type, 0)              \
	X(bool, rotation_reversed, false)     \
	X(int, translate_type, 0)

#define PART_ANIM_INFO_TRACKS(X) \
	X(rotation_x)                \
	X(rotation_y)                \
	X(rotation_z)                \
	X(scale_x)                   \
	X(scale_y)                   \
	X(scale_z)                   \
	X(translation)

class PartAnimInfo : public RefCounted {
	GDCLASS(PartAnimInfo, RefCounted)

public:
	PART_ANIM_INFO_FIELDS(INSPECTION_ACCESSORS)
#define PART_ANIM_INFO_TRACK_ACCESSORS(m_name)                                      \
	Ref<PartAnimTrack> get_##m_name() const { return m_name##_; }                  \
	void set_##m_name(const Ref<PartAnimTrack> &p_value) { m_name##_ = p_value; }
	PART_ANIM_INFO_TRACKS(PART_ANIM_INFO_TRACK_ACCESSORS)
#undef PART_ANIM_INFO_TRACK_ACCESSORS
	// The track by its property name ("rotation_x" .. "translation"); null otherwise.
	Ref<PartAnimTrack> get_track(const String &p_name) const;

protected:
	static void _bind_methods();

private:
	PART_ANIM_INFO_FIELDS(INSPECTION_MEMBER)
#define PART_ANIM_INFO_TRACK_MEMBER(m_name) Ref<PartAnimTrack> m_name##_;
	PART_ANIM_INFO_TRACKS(PART_ANIM_INFO_TRACK_MEMBER)
#undef PART_ANIM_INFO_TRACK_MEMBER
};

// One RMDL LOD header: the render function tag, the switch threshold and the
// part / strip / vertex / index counts.
#define RENDER_LOD_INFO_FIELDS(X)          \
	X(String, render_function, String())   \
	X(int, threshold, 0)                   \
	X(int, part_count, 0)                  \
	X(int, strip_count, 0)                 \
	X(int, vertex_count, 0)                \
	X(int, index_count, 0)

class RenderLodInfo : public RefCounted {
	GDCLASS(RenderLodInfo, RefCounted)

public:
	RENDER_LOD_INFO_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	RENDER_LOD_INFO_FIELDS(INSPECTION_MEMBER)
};

// The model's active two-channel body blend (ObjectModel::get_body_blend, null
// when a single channel poses the body).
#define BODY_BLEND_STATE_FIELDS(X)      \
	X(String, source_key, String())     \
	X(float, source_time, 0.0f)         \
	X(float, weight, 1.0f)

class BodyBlendState : public RefCounted {
	GDCLASS(BodyBlendState, RefCounted)

public:
	BODY_BLEND_STATE_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	BODY_BLEND_STATE_FIELDS(INSPECTION_MEMBER)
};

// The model's applied weapon-channel pose (ObjectModel::get_weapon_channel,
// null when no channel is held).
#define WEAPON_CHANNEL_STATE_FIELDS(X)  \
	X(String, key, String())            \
	X(int, phase_ticks, 0)              \
	X(String, prev_key, String())       \
	X(int, prev_phase_ticks, 0)         \
	X(float, blend_weight, 1.0f)        \
	X(int, variant, 0)                  \
	X(int, prev_variant, 0)

class WeaponChannelState : public RefCounted {
	GDCLASS(WeaponChannelState, RefCounted)

public:
	WEAPON_CHANNEL_STATE_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	WEAPON_CHANNEL_STATE_FIELDS(INSPECTION_MEMBER)
};

// One running PLAYPARTANIM sweep on a CTRL register (ObjectModel::get_active_part_anims).
#define PART_ANIM_CHANNEL_STATE_FIELDS(X)  \
	X(String, register, String())          \
	X(int, dir, 0)                         \
	X(int, rate, 0)                        \
	X(int64_t, value, 0)

class PartAnimChannelState : public RefCounted {
	GDCLASS(PartAnimChannelState, RefCounted)

public:
	PART_ANIM_CHANNEL_STATE_FIELDS(INSPECTION_ACCESSORS)

protected:
	static void _bind_methods();

private:
	PART_ANIM_CHANNEL_STATE_FIELDS(INSPECTION_MEMBER)
};

} // namespace godot

#undef INSPECTION_ACCESSORS
#undef INSPECTION_MEMBER
